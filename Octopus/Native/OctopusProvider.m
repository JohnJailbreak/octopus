#import "OctopusProvider.h"
#import "../Protocol/oct_responder.h"
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

/* The native responder retains ownership of sessions, transaction numbers,
 * USB buffers, asynchronous writes, cancellation and final responses. This
 * module supplies provider return values only. See native-routing-findings.md. */
static pthread_mutex_t coreLock = PTHREAD_MUTEX_INITIALIZER;
static oct_store *store;
static oct_responder *encoder;
static uint32_t localTransaction;

static atomic_bool installed;

static IMP originalIDs, originalStorage, originalInfo, originalHandles;
static IMP originalSource, originalValue, originalList, originalSupported;
static IMP originalLookup;

@interface OCTNativeDataset : NSObject
@property(nonatomic, strong) NSData *content;
@property(nonatomic, strong) NSString *filename;
@property(nonatomic) uint32_t objectCompressedSize;
@property(nonatomic) uint64_t objectCompressedSize64;
@end
@implementation OCTNativeDataset
@end

/*
 * The object Apple's writer reads a download from. PTPDataPacket::copyToBuffer:numBytes: sends it
 * -read:length:offset: for each chunk, so the bytes are fetched from the store on demand rather
 * than held here: a download costs one chunk of memory, not the size of the file, and is not
 * capped by the responder's buffered-transfer ceiling.
 *
 * Reads are refused once authorization lapses, which is checked per chunk and not only when the
 * source is handed out, so a download in progress stops at the next chunk after a lease expires.
 */
@interface OCTNativeSource : NSObject
@property(nonatomic) uint32_t handle;
@property(nonatomic) uint64_t size;
@property(nonatomic) uint64_t authorizationEpoch;
- (uint64_t)length;
- (int64_t)read:(void *)buffer length:(uint64_t)length offset:(int64_t)offset;
@end
@implementation OCTNativeSource
- (uint64_t)length { return self.size; }
- (int64_t)read:(void *)buffer length:(uint64_t)length offset:(int64_t)offset {
    if (!OCTNativeProviderEnabled() || self.authorizationEpoch != OCTNativeAuthorizationEpoch() ||
        offset < 0 || (!buffer && length)) {
        OCTNativeProviderEvent("provider-read-denied", self.handle, OCT_RC_ACCESS_DENIED);
        return -1;
    }
    if ((uint64_t)offset > self.size) return -1;
    if (!length) return 0;
    /* One chunk at a time, clamped so a huge request can't ask the store for more than remains. */
    uint64_t remaining = self.size - (uint64_t)offset;
    size_t wanted = (size_t)(length < remaining ? length : remaining);
    if (!wanted) return 0; /* at the end: the writer stops here */
    size_t got = 0;
    pthread_mutex_lock(&coreLock);
    oct_status status = store ? oct_store_read(store, self.handle, (uint64_t)offset, buffer, wanted, &got)
                              : OCT_E_STATE;
    pthread_mutex_unlock(&coreLock);
    if (status != OCT_OK) {
        OCTNativeProviderEvent("provider-read-failed", self.handle, (int32_t)status);
        return -1;
    }
    OCTNativeProviderEvent("provider-read", self.handle, (int32_t)got);
    return (int64_t)got;
}
@end

/* This local encoder is never an external USB session. Its sole purpose is to
 * reuse the tested core's datasets/filtering. Its containers are stripped here;
 * Apple's responder constructs and sends the real transaction's containers. */
static NSData *query(uint16_t operation, const uint32_t *params, uint32_t count, uint16_t *code) {
    pthread_mutex_lock(&coreLock);
    oct_buf scratch; oct_buf_init(&scratch, 65536);
    oct_exchange exchange = {0};
    if (!localTransaction) {
        uint32_t session = 1;
        oct_responder_reset(encoder);
        if (oct_exchange_run(encoder, OCT_OP_OPEN_SESSION, 0, &session, 1, false,
                             NULL, 0, &scratch, &exchange) != OCT_OK || exchange.response_code != OCT_RC_OK) {
            *code = OCT_RC_GENERAL_ERROR;
            oct_buf_free(&scratch); pthread_mutex_unlock(&coreLock); return nil;
        }
        localTransaction = 1;
    }
    oct_status status = oct_exchange_run(encoder, operation, localTransaction++, params, count,
                                         false, NULL, 0, &scratch, &exchange);
    *code = status == OCT_OK ? exchange.response_code : OCT_RC_GENERAL_ERROR;
    NSData *data = (*code == OCT_RC_OK && exchange.has_data)
                    ? [NSData dataWithBytes:exchange.data length:exchange.data_length] : nil;
    oct_buf_free(&scratch);
    pthread_mutex_unlock(&coreLock);
    return data;
}

/*
 * Whether a handle is one of ours. The store assigns handles from OCT_NATIVE_FIRST_HANDLE
 * upwards and nothing removes them, so a handle this store knows is ours by construction. Asking
 * the store is O(1) and, unlike a fixed list, works when objects are discovered as the host
 * browses rather than all at once.
 */
static BOOL isSynthetic(uint32_t handle) {
    if (handle < OCT_NATIVE_FIRST_HANDLE) return NO;
    oct_object_info info = { 0 }; /* defined on every path: clang -Wsometimes-uninitialized */
    pthread_mutex_lock(&coreLock);
    oct_status status = store ? oct_store_get(store, handle, &info) : OCT_E_STATE;
    pthread_mutex_unlock(&coreLock);
    return status == OCT_OK;
}

/*
 * Never shadow an Apple storage or object, even if one appears after we started serving: Apple can
 * add a storage, or allocate a handle in our range, at any time, so both are rechecked on every
 * call. The saved original IMPs keep these lookups out of our own hooks.
 *
 * The handle check asks about one handle, OCT_NATIVE_FIRST_HANDLE, rather than every handle we
 * own. Apple's object handles come from FUN_100004224, which increments a global counter and
 * returns the new value, and whose only callers are the PTPFolder and PTPFile initializers. It only
 * counts upward, so Apple cannot hold any handle in our range without first having allocated the
 * base of it. Asking about each owned handle meant one message into Apple's code per object per
 * call, which a filesystem makes unbounded; this is one message per call, whatever we serve.
 */
static BOOL available(id camera) {
    if (!atomic_load_explicit(&installed, memory_order_acquire) || !OCTNativeProviderEnabled()) return NO;
    id ids = ((id (*)(id,SEL))originalIDs)(camera, sel_registerName("allStorageIDs"));
    if (![ids isKindOfClass:[NSArray class]] || [ids containsObject:@(OCT_NATIVE_STORAGE_ID)]) {
        OCTNativeProviderEvent("provider-storage-collision", OCT_NATIVE_STORAGE_ID, OCT_RC_ACCESS_DENIED);
        return NO;
    }
    if (((id (*)(id,SEL,id))originalLookup)(camera, sel_registerName("objectForObjectHandle:"),
                                            @(OCT_NATIVE_FIRST_HANDLE))) {
        OCTNativeProviderEvent("provider-handle-collision", OCT_NATIVE_FIRST_HANDLE, OCT_RC_ACCESS_DENIED);
        return NO;
    }
    return YES;
}

static id storageIDs(id self, SEL cmd) {
    id base = ((id (*)(id,SEL))originalIDs)(self,cmd);
    if (!available(self) || ![base isKindOfClass:[NSArray class]]) return base;
    OCTNativeProviderEvent("provider-storage-enumeration", OCT_NATIVE_STORAGE_ID, OCT_RC_OK);
    return [base arrayByAddingObject:@(OCT_NATIVE_STORAGE_ID)];
}

static id storageInfo(id self, SEL cmd, uint32_t storageID) {
    if (storageID != OCT_NATIVE_STORAGE_ID || !available(self))
        return ((id (*)(id,SEL,uint32_t))originalStorage)(self,cmd,storageID);
    uint16_t code;
    NSData *body = query(OCT_OP_GET_STORAGE_INFO, &storageID, 1, &code);
    if (!body) return nil;
    OCTNativeDataset *dataset = [OCTNativeDataset new]; dataset.content = body;
    OCTNativeProviderEvent("provider-storage-info", storageID, code);
    return dataset;
}

static id objectInfo(id self, SEL cmd, uint32_t handle) {
    if (!isSynthetic(handle) || !available(self))
        return ((id (*)(id,SEL,uint32_t))originalInfo)(self,cmd,handle);
    uint16_t code; NSData *body = query(OCT_OP_GET_OBJECT_INFO, &handle, 1, &code);
    if (!body) return nil;
    oct_object_info info;
    pthread_mutex_lock(&coreLock); oct_status status = oct_store_get(store, handle, &info); pthread_mutex_unlock(&coreLock);
    if (status != OCT_OK || info.size > UINT32_MAX) return nil;
    OCTNativeDataset *dataset = [OCTNativeDataset new]; dataset.content = body;
    dataset.filename = [NSString stringWithUTF8String:info.name];
    dataset.objectCompressedSize = (uint32_t)info.size; dataset.objectCompressedSize64 = info.size;
    OCTNativeProviderEvent("provider-object-info", handle, code);
    return dataset;
}

static NSArray *decodeHandles(NSData *body, uint16_t *code) {
    if (!body) return nil;
    oct_reader reader; oct_reader_init(&reader, body.bytes, body.length);
    uint32_t count = oct_read_u32(&reader);
    /* Bounded by the store's own enumeration budget rather than the six objects of the synthetic
     * tree, which would refuse every real directory with more than six entries. The store set
     * that budget when it was opened and never changes it. */
    if (reader.failed || count > oct_store_max_objects(store) ||
        oct_reader_remaining(&reader) != (size_t)count * 4) {
        *code = OCT_RC_GENERAL_ERROR; return nil;
    }
    NSMutableArray *result = [NSMutableArray arrayWithCapacity:count];
    for (uint32_t i=0;i<count;i++) [result addObject:@(oct_read_u32(&reader))];
    return result;
}

static id objectHandles(id self, SEL cmd, uint32_t storageID, uint16_t format, uint32_t association, uint16_t *code) {
    BOOL all = storageID == UINT32_MAX;
    if ((!all && storageID != OCT_NATIVE_STORAGE_ID) || !available(self))
        return ((id (*)(id,SEL,uint32_t,uint16_t,uint32_t,uint16_t *))originalHandles)(self,cmd,storageID,format,association,code);
    BOOL ownParent = isSynthetic(association);
    if (all && association != 0 && association != UINT32_MAX && !ownParent)
        return ((id (*)(id,SEL,uint32_t,uint16_t,uint32_t,uint16_t *))originalHandles)(self,cmd,storageID,format,association,code);
    id base = @[];
    if (all && !ownParent) {
        base = ((id (*)(id,SEL,uint32_t,uint16_t,uint32_t,uint16_t *))originalHandles)(self,cmd,storageID,format,association,code);
        if (*code != OCT_RC_OK || ![base isKindOfClass:[NSArray class]]) return base;
    }
    uint32_t params[] = {OCT_NATIVE_STORAGE_ID,format,association};
    uint16_t localCode; NSData *body = query(OCT_OP_GET_OBJECT_HANDLES,params,3,&localCode);
    NSArray *extra = decodeHandles(body,&localCode);
    if (localCode != OCT_RC_OK || !extra) {
        if (all && !ownParent) return base; /* preserve Apple's successful enumeration */
        *code = localCode; return nil;
    }
    *code = OCT_RC_OK;
    OCTNativeProviderEvent("provider-object-enumeration", association, (int32_t)extra.count);
    return [base arrayByAddingObjectsFromArray:extra];
}

static id dataSource(id self, SEL cmd, uint32_t handle, uint16_t *code, uint32_t *size) {
    uint64_t epoch = OCTNativeAuthorizationEpoch();
    if (!isSynthetic(handle) || !available(self))
        return ((id (*)(id,SEL,uint32_t,uint16_t *,uint32_t *))originalSource)(self,cmd,handle,code,size);
    *size = 0;
    /* Only the object's size is needed now; the bytes follow one chunk at a time. */
    oct_object_info info = { 0 }; /* defined on every path: clang -Wsometimes-uninitialized */
    pthread_mutex_lock(&coreLock);
    oct_status status = store ? oct_store_get(store, handle, &info) : OCT_E_STATE;
    pthread_mutex_unlock(&coreLock);
    if (status != OCT_OK || info.is_directory) { *code = OCT_RC_INVALID_OBJECT_HANDLE; return nil; }
    /* The caller's size argument is 32-bit, so refuse rather than truncate: a truncated size
     * would make Apple send a short data phase and the host would store a corrupt file. */
    if (info.size > UINT32_MAX) { *code = OCT_RC_OBJECT_TOO_LARGE; return nil; }
    if (!OCTNativeProviderEnabled() || epoch != OCTNativeAuthorizationEpoch()) {
        *code = OCT_RC_ACCESS_DENIED; return nil;
    }
    OCTNativeSource *source = [OCTNativeSource new];
    source.handle = handle;
    source.size = info.size;
    source.authorizationEpoch = epoch;
    *size = (uint32_t)info.size;
    *code = OCT_RC_OK;
    OCTNativeProviderEvent("provider-download-source",handle,(int32_t)info.size);
    return source;
}

static id propertyValue(id self, SEL cmd, uint32_t handle, uint16_t property, uint16_t *code) {
    if (!isSynthetic(handle) || !available(self))
        return ((id (*)(id,SEL,uint32_t,uint16_t,uint16_t *))originalValue)(self,cmd,handle,property,code);
    uint32_t params[] = {handle,property};
    return query(OCT_OP_GET_OBJECT_PROP_VALUE,params,2,code);
}

static id propertyList(id self, SEL cmd, uint32_t handle, uint32_t format, uint32_t property,
                       uint32_t group, uint32_t depth, uint16_t *code) {
    BOOL own = isSynthetic(handle), wildcard = handle == 0 || handle == UINT32_MAX;
    if ((!own && !wildcard) || !available(self))
        return ((id (*)(id,SEL,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint16_t *))originalList)(self,cmd,handle,format,property,group,depth,code);
    uint32_t params[] = {handle,format,property,group,depth};
    if (own) return query(OCT_OP_GET_OBJECT_PROP_LIST,params,5,code);
    id base = ((id (*)(id,SEL,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint16_t *))originalList)(self,cmd,handle,format,property,group,depth,code);
    /* This ptpd returns nil for wildcard properties. Preserve that behavior:
     * a synthetic-only result would silently remove Apple's photo properties. */
    if (*code != OCT_RC_OK || ![base isKindOfClass:[NSData class]] || [base length] < 4) return base;
    uint16_t localCode; NSData *extra = query(OCT_OP_GET_OBJECT_PROP_LIST,params,5,&localCode);
    if (localCode != OCT_RC_OK || extra.length < 4 || [base length] > OCT_PTP_MAX_TRANSFER - extra.length) return base;
    oct_reader r; oct_reader_init(&r,[base bytes],[base length]); uint32_t n = oct_read_u32(&r);
    oct_reader_init(&r,extra.bytes,extra.length); uint32_t m = oct_read_u32(&r);
    if (n > UINT32_MAX-m || n > ([base length]-4)/8 || m > (extra.length-4)/8) return base;
    uint32_t total = n+m; uint8_t le[] = {(uint8_t)total,(uint8_t)(total>>8),(uint8_t)(total>>16),(uint8_t)(total>>24)};
    NSMutableData *merged = [base mutableCopy]; [merged replaceBytesInRange:NSMakeRange(0,4) withBytes:le];
    [merged appendBytes:(const uint8_t *)extra.bytes+4 length:extra.length-4];
    return merged;
}

static id supportedProperties(id self, SEL cmd, uint16_t format, uint16_t *code) {
    /* Only extend the text format. Do not change photo/folder capabilities. */
    if (format != OCT_FORMAT_TEXT || !available(self))
        return ((id (*)(id,SEL,uint16_t,uint16_t *))originalSupported)(self,cmd,format,code);
    size_t count; const uint16_t *properties=oct_object_properties(&count);
    NSMutableArray *result=[NSMutableArray arrayWithCapacity:count];
    for(size_t i=0;i<count;i++)[result addObject:@(properties[i])];
    *code=OCT_RC_OK;return result;
}

typedef struct { const char *selector; const char *encoding; IMP replacement; IMP *original; } Hook;
static Hook hooks[] = {
    {"allStorageIDs","@16@0:8",(IMP)storageIDs,&originalIDs},
    {"storageInfoDatasetForStorageID:","@20@0:8I16",(IMP)storageInfo,&originalStorage},
    {"objectInfoDatasetForObjectHandle:","@20@0:8I16",(IMP)objectInfo,&originalInfo},
    {"objectHandlesInStorage:forObjectFormatCode:inAssociation:responseCode:","@36@0:8I16S20I24^S28",(IMP)objectHandles,&originalHandles},
    {"dataSourceForObjectHandle:responseCode:size:","@36@0:8I16^S20^I28",(IMP)dataSource,&originalSource},
    {"objectPropValueForHandle:withObjectPropertyCode:responseCode:","@32@0:8I16S20^S24",(IMP)propertyValue,&originalValue},
    {"objectPropListForHandle:withObjectFormatCode:andPropertyCode:andPropertyGroup:andDepth:responseCode:","@44@0:8I16I20I24I28I32^S36",(IMP)propertyList,&originalList},
    {"objectPropertiesSupportedForObjectFormatCode:responseCode:","@28@0:8S16^S20",(IMP)supportedProperties,&originalSupported}
};

BOOL OCTProviderValidate(NSString **reason) {
    for(size_t i=0;i<sizeof(hooks)/sizeof(hooks[0]);i++)
        if(!OCTRuntimeCheck("PTPVirtualCamera",hooks[i].selector,hooks[i].encoding,reason))return NO;
    if(!OCTRuntimeCheck("PTPVirtualCamera","objectForObjectHandle:","@24@0:8@16",reason))return NO;
    if(reason)*reason=@"Provider method encodings and original implementation provenance matched";
    return YES;
}

BOOL OCTProviderInstall(NSString **reason) {
    if (atomic_load_explicit(&installed,memory_order_acquire)) return YES;
    if (!OCTProviderValidate(reason)) return NO;
    originalLookup=method_getImplementation(class_getInstanceMethod(objc_getClass("PTPVirtualCamera"),sel_registerName("objectForObjectHandle:")));
    oct_backend backend={0}; oct_synthetic_options options={0};
    oct_store_options storeOptions={.storage_id=OCT_NATIVE_STORAGE_ID,.first_handle=OCT_NATIVE_FIRST_HANDLE,
        .description="Octopus (read-only)",.volume_label="Octopus"};
    if(oct_synthetic_backend_create(&options,&backend)!=OCT_OK ||
       oct_store_open(&storeOptions,backend,&store)!=OCT_OK || oct_store_index_all(store)!=OCT_OK ||
       oct_responder_create(store,NULL,&encoder)!=OCT_OK) {
        oct_responder_destroy(encoder);encoder=NULL;oct_store_close(store);store=NULL;
        if(reason)*reason=@"Cannot initialize immutable synthetic provider";return NO;
    }
    oct_handle_list list={0};
    if(oct_store_list(store,OCT_ROOT,true,&list)!=OCT_OK || list.count!=6) {
        oct_handle_list_free(&list);oct_responder_destroy(encoder);encoder=NULL;oct_store_close(store);store=NULL;
        if(reason)*reason=@"Synthetic tree identity mismatch";return NO;
    }
    oct_handle_list_free(&list);
    for(size_t i=0;i<sizeof(hooks)/sizeof(hooks[0]);i++) {
        if(!OCTRuntimeHook("PTPVirtualCamera",hooks[i].selector,hooks[i].encoding,hooks[i].replacement,hooks[i].original,reason)) {
            /* Leave pass-through hooks resident. Never uninstall over a later
             * module's replacement or free objects another thread may see. */
            OCTNativeProviderEvent("provider-install-failed",0,OCT_RC_GENERAL_ERROR);return NO;
        }
    }
    atomic_store_explicit(&installed,true,memory_order_release);
    OCTNativeProviderEvent("provider-installed",OCT_NATIVE_STORAGE_ID,OCT_RC_OK);
    return YES;
}

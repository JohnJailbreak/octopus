#import "OctopusProvider.h"
#import "../Protocol/oct_responder.h"
#include "oct_native_dataset.h"
#include "../Core/oct_readonly.h"
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
static IMP originalDeviceInfo, originalBulkInfo, originalDelete;
/* Whether the store being served accepts changes, which decides what DeviceInfo advertises. */
static bool storeWritable;
/* Whether the delete hook actually installed. DeleteObject is advertised only when this and
 * storeWritable are both true: offering an operation the provider cannot serve would leave a host
 * deleting into Apple's implementation, which knows nothing of our handles. */
static atomic_bool deleteHooked;
/* Whether the upload receiver is hooked. Apple routes an incoming object to the storage or folder
 * it belongs in, through appendObjectData:..., which is the only method in ptpd that takes one.
 * SendObjectInfo and SendObject are advertised only when this is in place and the store accepts
 * changes: a host offered an upload it cannot complete would fail mid-copy instead of not offering. */
static atomic_bool uploadHooked;
/* Set when a store opens and cleared once a host has asked for the storage list, so the
 * announcement can be repeated meanwhile: an event sent before the session is up goes nowhere. */
static atomic_bool announceWanted;

/*
 * The camera a host is talking to, kept from the first camera hook that runs. Apple's own
 * sendEventAddStorageWithID:, recorded as v20@0:8I16, announces a storage exactly as Apple does when
 * one appears, through its own responder and transport, with its own packet. It is preferred over
 * the observer's hand-built event, which remains the fallback for a camera not yet seen.
 *
 * Weak: the camera is ptpd's, and keeping a released one alive to send to would be worse than not
 * announcing at all. A host that has not reached a camera hook has not enumerated yet, and will see
 * the storage when it does, so the camera being unknown costs nothing.
 */
static __weak id knownCamera;

@interface NSObject (OctopusAddStorage)
- (void)sendEventAddStorageWithID:(unsigned int)storage;
@end

static void rememberCamera(id camera) {
    if (camera && !knownCamera) knownCamera = camera;
}

/* Announce the storage, through Apple's method when the camera is known, otherwise the observer's. */
static void announce(void) {
    id camera = knownCamera;
    if (camera && [camera respondsToSelector:@selector(sendEventAddStorageWithID:)]) {
        [camera sendEventAddStorageWithID:OCT_NATIVE_STORAGE_ID];
        OCTNativeProviderEvent("provider-announced-by-camera", OCT_NATIVE_STORAGE_ID, OCT_RC_OK);
        return;
    }
    (void)OCTNativeAnnounceStorage(OCT_NATIVE_STORAGE_ID);
}
static IMP originalAppendStorage, originalAppendFolder;
/* The name and parent a host sent in SendObjectInfo, kept until the data arrives in SendObject.
 * PTP splits an upload across two operations and only the first carries the name. */
static pthread_mutex_t pendingLock = PTHREAD_MUTEX_INITIALIZER;
static char pendingName[OCT_NAME_MAX + 1];
static uint32_t pendingParent;

/* Dynamic class lookup avoids linking against private classes exported by the
 * executable. These are only the exact signatures verified in this image. */
@protocol OCTNativeContent <NSObject>
- (id)content;
@end
@protocol OCTNativeObjectInfo <NSObject>
- (instancetype)initWithData:(id)data;
- (void)setObjectHandle:(uint32_t)handle;
- (id)content:(int32_t)type;
@end

@interface OCTNativeDataset : NSObject
@property(nonatomic, strong) NSData *content;
@property(nonatomic, strong) NSString *filename;
@property(nonatomic) uint32_t objectCompressedSize;
@property(nonatomic) uint64_t objectCompressedSize64;
/* Apple's own object this one stands in for, when there is one. */
@property(nonatomic, strong) id original;
@end
/*
 * When this object replaces one of Apple's (the DeviceInfo dataset), any message it doesn't
 * implement goes to Apple's genuine object, which answers with its real signature and real values.
 * That is delegation, not fabrication: nothing here invents a method signature or a return value.
 *
 * It exists because the evidence shows getDeviceInfo: building its data packet from bytes, which
 * fits it reading only -content, but getDeviceInfo:'s own body (0x100020bbc) was never captured,
 * so that is an inference. PTPDeviceInfoDataset has many accessors (standardVersion,
 * vendorExtensionID, ...). Without this, any one of them sent to the substitute would raise inside
 * ptpd. With no original, behaviour is exactly NSObject's, so the other uses are unchanged.
 */
@implementation OCTNativeDataset
- (id)forwardingTargetForSelector:(SEL)selector
{
    return self.original ?: [super forwardingTargetForSelector:selector];
}
- (BOOL)respondsToSelector:(SEL)selector
{
    return [super respondsToSelector:selector] || [self.original respondsToSelector:selector];
}
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

/* Announce the storage again if a host has not yet read the list since a store opened. Called from
 * the observer's timer. The storage ID stays here, the one place that serves it: the observer
 * naming it would mean depending on the provider's header, which it does not include, and on 0.4.61
 * that was a compile error. */
void OCTProviderAnnounceIfNeeded(void) {
    if (atomic_load_explicit(&announceWanted, memory_order_acquire) &&
        atomic_load_explicit(&installed, memory_order_acquire)) {
        announce();
    }
}

static id storageIDs(id self, SEL cmd) {
    rememberCamera(self);
    id base = ((id (*)(id,SEL))originalIDs)(self,cmd);
    if (!available(self) || ![base isKindOfClass:[NSArray class]]) return base;
    OCTNativeProviderEvent("provider-storage-enumeration", OCT_NATIVE_STORAGE_ID, OCT_RC_OK);
    /* The host has read the list, so it needs no further telling. */
    atomic_store_explicit(&announceWanted,false,memory_order_release);
    return [base arrayByAddingObject:@(OCT_NATIVE_STORAGE_ID)];
}

static id deviceInfo(id self, SEL cmd) {
    rememberCamera(self);
    id base=((id (*)(id,SEL))originalDeviceInfo)(self,cmd);
    if (!available(self) || object_getClass(base)!=objc_getClass("PTPDeviceInfoDataset")) return base;
    id body=[(id<OCTNativeContent>)base content];
    if (![body isKindOfClass:[NSData class]] || [body length]>OCT_NATIVE_DEVICE_INFO_MAX-2) return base;
    NSMutableData *copy=[NSMutableData dataWithLength:[body length]+2];size_t length=0;
    int result=oct_native_device_info_add_format([body bytes],[body length],OCT_FORMAT_TEXT,
                                                copy.mutableBytes,copy.length,&length);
    if (result!=1) return base;
    copy.length=length;
    /* When the served store accepts changes, advertise the operations that change it: a host
     * reads OperationsSupported to decide what to offer, and offers nothing it does not see there.
     * Each operation is gated on its own hook below. This used to sit inside a condition on the
     * delete hook alone, written before uploads existed, so a missing delete hook would also have
     * hidden uploads whose hooks were in place. */
    if (storeWritable) {
        /* Recorded separately from what was advertised, because the two failing differ: a store not
         * yet writable when a host asked is a timing fault, an operation missing from the list with
         * the store writable is a hook that did not install. */
        OCTNativeProviderEvent("provider-device-info-writable",0,OCT_RC_OK);
        /* Each append grows the dataset by two bytes, so each is its own call on the result of the
         * last. An operation is advertised only when the hook that serves it is in place: a host
         * offered one that cannot be served fails part-way through a copy instead of not offering. */
        /* The events, in the array before the operations. A host ignores an event it was not told
         * to expect, so StoreAdded has to be advertised for the announcement to mean anything, and
         * ObjectAdded for a file uploaded or deleted to appear without a refresh. */
        uint16_t events[2]={OCT_EVENT_STORE_ADDED,OCT_EVENT_OBJECT_ADDED};
        for (unsigned i=0;i<2;i++) {
            NSMutableData *withEvent=[NSMutableData dataWithLength:copy.length+2];
            size_t grownBy=0;
            if (oct_native_device_info_add_event(copy.bytes,copy.length,events[i],
                                                 withEvent.mutableBytes,withEvent.length,&grownBy)==1) {
                withEvent.length=grownBy;
                copy=withEvent;
                OCTNativeProviderEvent("provider-advertised-event",events[i],OCT_RC_OK);
            }
        }
        uint16_t operations[3];
        unsigned count=0;
        if (atomic_load_explicit(&deleteHooked,memory_order_acquire)) operations[count++]=OCT_OP_DELETE_OBJECT;
        if (atomic_load_explicit(&uploadHooked,memory_order_acquire)) {
            operations[count++]=OCT_OP_SEND_OBJECT_INFO;
            operations[count++]=OCT_OP_SEND_OBJECT;
        }
        for (unsigned i=0;i<count;i++) {
            NSMutableData *grownData=[NSMutableData dataWithLength:copy.length+2];
            size_t grown=0;
            if (oct_native_device_info_add_operation(copy.bytes,copy.length,operations[i],
                                                     grownData.mutableBytes,grownData.length,&grown)==1) {
                grownData.length=grown;
                copy=grownData;
                OCTNativeProviderEvent("provider-advertised",operations[i],OCT_RC_OK);
            }
        }
    }
    OCTNativeDataset *dataset=[OCTNativeDataset new];dataset.content=copy;dataset.original=base;
    OCTNativeProviderEvent("provider-device-info",OCT_FORMAT_TEXT,OCT_RC_OK);
    return dataset;
}

/*
 * Deleting one of our objects. Apple's responder calls this on the camera for DeleteObject, so
 * unlike an upload it needs no interception of the request itself: the handle arrives here and the
 * response code goes back through the pointer.
 *
 * A handle that is not ours is forwarded untouched, so Apple's own photo deletion is unaffected.
 */
static void deleteObject(id self, SEL cmd, uint32_t handle, unsigned short *code) {
    if (!isSynthetic(handle) || !available(self)) {
        ((void (*)(id,SEL,uint32_t,unsigned short *))originalDelete)(self,cmd,handle,code);
        return;
    }
    uint32_t removed=0;
    pthread_mutex_lock(&coreLock);
    oct_status status=store?oct_store_delete(store,handle,&removed):OCT_E_STATE;
    pthread_mutex_unlock(&coreLock);
    uint16_t response=oct_response_for_status(status);
    if (code) *code=(unsigned short)response;
    OCTNativeProviderEvent("provider-delete",handle,response);
}

/*
 * Whether the provider is serving, asked without a camera. available() runs the camera's own
 * allStorageIDs and objectForObjectHandle: on its argument, so it may only ever be given the
 * camera. The upload hooks are called on a PTPStorage or a PTPFolder, and passing one of those
 * there ran camera code against an object of another class: in the tests the check failed and the
 * upload went to Apple's receiver, and on a phone it would have read a storage's memory as a
 * camera's, inside the process that serves Photos.
 *
 * The collision checks available() makes are not needed here. They decide whether the camera
 * reports our storage at all; a host that was never shown it cannot upload into it.
 */
static BOOL providerServing(void) {
    return atomic_load_explicit(&installed, memory_order_acquire) && OCTNativeProviderEnabled();
}

/*
 * The ObjectInfo a host sends before the object itself. PTP splits an upload in two: SendObjectInfo
 * carries the name, storage and parent, SendObject carries the bytes and neither the name nor the
 * storage. Apple builds this dataset from the host's bytes, so the same bytes are parsed here to
 * keep the name until the data arrives.
 *
 * Only an upload into our storage is remembered; one into Apple's is left entirely alone.
 */
static IMP originalIncomingInfo;
static id incomingObjectInfo(id self, SEL cmd, id data, int contentType) {
    id dataset = ((id (*)(id,SEL,id,int))originalIncomingInfo)(self, cmd, data, contentType);
    if (!dataset || !data || !providerServing()) return dataset;
    oct_object_info_request parsed;
    if (oct_dataset_parse_object_info([data bytes], [data length], &parsed) != OCT_OK) return dataset;
    if (parsed.storage_id != OCT_NATIVE_STORAGE_ID) return dataset;
    /* A parent of zero means the storage's root in PTP, which for us is the root handle. */
    uint32_t parent = parsed.parent == 0 ? OCT_NATIVE_FIRST_HANDLE : parsed.parent;
    pthread_mutex_lock(&pendingLock);
    memcpy(pendingName, parsed.name, sizeof pendingName);
    pendingName[sizeof pendingName - 1] = '\0';
    pendingParent = parent;
    pthread_mutex_unlock(&pendingLock);
    OCTNativeProviderEvent("provider-upload-named", parent, parsed.format);
    return dataset;
}

/*
 * An object arriving from a host. Apple calls this on the storage or folder the object belongs in,
 * with the bytes, the format, the parent handle and a content type; the 64-bit result is the
 * handle of what was created, or zero when nothing was.
 *
 * Only an association that is ours is handled here; anything else is Apple's, forwarded untouched,
 * so importing a photo into the library still works exactly as before.
 */

static unsigned long long appendObject(id self, SEL cmd, id data, unsigned short format,
                                       unsigned int association, unsigned int contentType, IMP original) {
    if (!isSynthetic(association) || !providerServing()) {
        return ((unsigned long long (*)(id,SEL,id,unsigned short,unsigned int,unsigned int))original)
            (self, cmd, data, format, association, contentType);
    }
    /* The name came in SendObjectInfo, which Apple has already accepted by the time the data
     * arrives. Without one there is nothing to call the file, so the upload is refused rather than
     * written under a name of our invention. */
    char name[OCT_NAME_MAX + 1];
    pthread_mutex_lock(&pendingLock);
    bool named = pendingName[0] != '\0' && pendingParent == association;
    if (named) { memcpy(name, pendingName, sizeof name); pendingName[0] = '\0'; }
    pthread_mutex_unlock(&pendingLock);
    if (!named) {
        OCTNativeProviderEvent("provider-upload-unnamed", association, OCT_RC_INVALID_PARENT_OBJECT);
        return 0;
    }

    const void *bytes = [data bytes];
    NSUInteger length = [data length];
    uint32_t handle = 0;
    pthread_mutex_lock(&coreLock);
    oct_status status = store ? oct_store_create(store, association, name, false, &handle)
                              : OCT_E_STATE;
    if (status == OCT_OK && length > 0) {
        status = oct_store_write(store, handle, 0, bytes, (size_t)length);
        if (status != OCT_OK) {
            /* A file created and then not written is worse than no file: remove it rather than
             * leave an empty one a host believes it has uploaded. */
            uint32_t removed = 0;
            (void)oct_store_delete(store, handle, &removed);
            handle = 0;
        }
    }
    pthread_mutex_unlock(&coreLock);
    OCTNativeProviderEvent(status == OCT_OK ? "provider-upload" : "provider-upload-failed",
                           handle, oct_response_for_status(status));
    return status == OCT_OK ? (unsigned long long)handle : 0;
}

static unsigned long long appendOnStorage(id self, SEL cmd, id data, unsigned short format,
                                          unsigned int association, unsigned int contentType) {
    return appendObject(self, cmd, data, format, association, contentType, originalAppendStorage);
}

static unsigned long long appendOnFolder(id self, SEL cmd, id data, unsigned short format,
                                         unsigned int association, unsigned int contentType) {
    return appendObject(self, cmd, data, format, association, contentType, originalAppendFolder);
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
    /* Report a format the host accepts. See oct_native_object_info_set_format: a host asked about
     * every object here and displayed only those whose format is not 0x3004. */
    NSMutableData *reported = [body mutableCopy];
    int mapped = oct_native_object_info_set_format(reported.mutableBytes, reported.length,
                                                   OCT_FORMAT_TEXT, OCT_FORMAT_UNDEFINED);
    if (mapped >= 0) body = reported;
    if (mapped == 1) OCTNativeProviderEvent("provider-format-mapped", handle, OCT_FORMAT_UNDEFINED);
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

static id bulkObjectInfo(id self,SEL cmd,uint32_t storageID,uint16_t format,
                         uint32_t association,int32_t type,uint16_t *code) {
    uint64_t epoch=OCTNativeAuthorizationEpoch();
    /* Apple's all-storage bulk path rejects 0xffffffff. Preserve its behavior,
     * including Photos filters and native format extensions. */
    if(storageID!=OCT_NATIVE_STORAGE_ID || type<1 || type>3 || !available(self))
        return ((id (*)(id,SEL,uint32_t,uint16_t,uint32_t,int32_t,uint16_t *))originalBulkInfo)
            (self,cmd,storageID,format,association,type,code);
    uint32_t params[]={storageID,format,association};
    NSArray *handles=decodeHandles(query(OCT_OP_GET_OBJECT_HANDLES,params,3,code),code);
    if (!handles || *code!=OCT_RC_OK) return nil;
    NSMutableData *result=[NSMutableData data];
    if (!handles.count) {
        const uint32_t empty=0;[result appendBytes:&empty length:4];
    } else if (type==3) {
        uint64_t count=handles.count;uint8_t little[8];
        for(unsigned i=0;i<8;i++)little[i]=(uint8_t)(count>>(i*8));
        [result appendBytes:little length:8];
    }
    Class datasetClass=objc_getClass("PTPObjectInfoDataset");
    for(NSNumber *number in handles) {
        uint32_t handle=number.unsignedIntValue;
        NSData *body=query(OCT_OP_GET_OBJECT_INFO,&handle,1,code);
        if(!body || *code!=OCT_RC_OK)return nil;
        id<OCTNativeObjectInfo> dataset=[(id<OCTNativeObjectInfo>)[datasetClass alloc] initWithData:body];
        if(!dataset){*code=OCT_RC_GENERAL_ERROR;return nil;}
        [dataset setObjectHandle:handle];
        id record=[dataset content:type];
        if(![record isKindOfClass:[NSData class]] || ![record length] ||
           [record length]>OCT_PTP_MAX_TRANSFER-result.length) {
            *code=OCT_RC_GENERAL_ERROR;return nil;
        }
        [result appendData:record];
    }
    if(!OCTNativeProviderEnabled() || epoch!=OCTNativeAuthorizationEpoch()) {
        *code=OCT_RC_ACCESS_DENIED;return nil;
    }
    *code=OCT_RC_OK;
    OCTNativeProviderEvent("provider-bulk-object-info",association,(int32_t)handles.count);
    return result;
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
    {"deviceInfoDataset","@16@0:8",(IMP)deviceInfo,&originalDeviceInfo},
    {"allStorageIDs","@16@0:8",(IMP)storageIDs,&originalIDs},
    {"storageInfoDatasetForStorageID:","@20@0:8I16",(IMP)storageInfo,&originalStorage},
    {"objectInfoDatasetForObjectHandle:","@20@0:8I16",(IMP)objectInfo,&originalInfo},
    {"objectHandlesInStorage:forObjectFormatCode:inAssociation:responseCode:","@36@0:8I16S20I24^S28",(IMP)objectHandles,&originalHandles},
    {"objectInfoForObjectsInStorage:forObjectFormatCode:inAssociation:withContentType:responseCode:","@40@0:8I16S20I24i28^S32",(IMP)bulkObjectInfo,&originalBulkInfo},
    {"dataSourceForObjectHandle:responseCode:size:","@36@0:8I16^S20^I28",(IMP)dataSource,&originalSource},
    {"objectPropValueForHandle:withObjectPropertyCode:responseCode:","@32@0:8I16S20^S24",(IMP)propertyValue,&originalValue},
    {"objectPropListForHandle:withObjectFormatCode:andPropertyCode:andPropertyGroup:andDepth:responseCode:","@44@0:8I16I20I24I28I32^S36",(IMP)propertyList,&originalList},
    {"objectPropertiesSupportedForObjectFormatCode:responseCode:","@28@0:8S16^S20",(IMP)supportedProperties,&originalSupported}
};

BOOL OCTProviderValidate(NSString **reason) {
    for(size_t i=0;i<sizeof(hooks)/sizeof(hooks[0]);i++)
        if(!OCTRuntimeCheck("PTPVirtualCamera",hooks[i].selector,hooks[i].encoding,reason))return NO;
    if(!OCTRuntimeCheck("PTPVirtualCamera","objectForObjectHandle:","@24@0:8@16",reason))return NO;
    if(!OCTRuntimeCheck("PTPDeviceInfoDataset","content","@16@0:8",reason) ||
       !OCTRuntimeCheck("PTPObjectInfoDataset","initWithData:","@24@0:8@16",reason) ||
       !OCTRuntimeCheck("PTPObjectInfoDataset","setObjectHandle:","v20@0:8I16",reason) ||
       !OCTRuntimeCheck("PTPObjectInfoDataset","content:","@20@0:8i16",reason))return NO;
    if(reason)*reason=@"Provider method encodings and original implementation provenance matched";
    return YES;
}

/*
 * The store the provider serves. Synthetic by default; a read-only view of one of the fixed roots
 * in oct_native_lease_root when a scope is granted. Switching closes the old store and opens a new
 * one, which is also how the read-only backend refreshes its directory snapshots.
 *
 * Called with coreLock held, and only from the authorization pass, which is the single thread that
 * owns `mode`. The hooks read `store` under the same lock.
 */
static oct_native_lease_mode storeScope;

static bool providerAuthorizedForStore(void *context)
{
    (void)context;
    /* Checked throughout every read-only operation, so a lapsed lease stops a walk in progress
     * rather than only preventing the next one. */
    return OCTNativeProviderEnabled();
}

static BOOL openStoreLocked(oct_native_lease_mode scope, NSString **reason)
{
    oct_responder_destroy(encoder);encoder=NULL;
    oct_store_close(store);store=NULL;
    storeScope=OCT_NATIVE_LEASE_NONE;
    const char *root=oct_native_lease_root(scope);
    /* The name a host shows for the volume. It said "(read-only)" whatever the grant, so with writes
     * switched on Explorer still announced a read-only volume and there was no way to tell from the
     * computer which mode was being served. */
    const bool writableScope=root && oct_native_lease_is_writable(scope);
    oct_store_options storeOptions={.storage_id=OCT_NATIVE_STORAGE_ID,.first_handle=OCT_NATIVE_FIRST_HANDLE,
        .description=writableScope?"Octopus (read-write)":"Octopus (read-only)",.volume_label="Octopus",
        /* Bounded so a deep root cannot grow the handle table without limit inside ptpd, which also
         * serves Photos. Reaching it answers a whole-hierarchy request with 0xA808, which makes a
         * host enumerate one directory at a time.
         *
         * The budget is larger for a real root than for the sample tree: this phone has about
         * 600,000 objects under /private and 291,000 under /System, so browsing a few directories
         * deep passes the old 65,536 quickly, after which every further listing fails. A handle
         * costs on the order of a hundred bytes, so this ceiling is a few tens of megabytes at
         * worst, and only for objects a host has actually asked about. */
        .max_objects=root?OCT_READONLY_MAX_NODES:OCT_STORE_DEFAULT_MAX_OBJECTS};
    if(root){
        oct_readonly_options ro={.root=root,.authorized=providerAuthorizedForStore,
            /* Per-directory and depth limits keep one listing bounded; the store's budget bounds
             * the total.
             *
             * Filesystem boundaries are crossed, because on iOS they must be: / is the system
             * partition and everything of interest, /var and so all of /var/mobile, lives on the
             * Data partition reached through a symlink. Refusing to cross left a host looking at
             * eleven of the fifteen entries at / and nothing underneath the interesting ones.
             * Escape is prevented by the backend verifying that a link's target is inside the
             * served root, by device and inode, rather than by refusing to cross. */
            .max_nodes=OCT_READONLY_MAX_NODES,
            .max_entries=4096,.max_depth=64,.max_read_bytes=1u<<20,.cross_filesystems=true,
            /* Writes follow the lease: a scope that asks for them gets create, write, truncate and
             * delete inside that root, each re-checking authorization as it runs. */
            .writable=oct_native_lease_is_writable(scope)};
        if(oct_readonly_store_open(&ro,&storeOptions,&store)!=OCT_OK){
            store=NULL;
            if(reason)*reason=[NSString stringWithFormat:@"Cannot open %s read-only",root];
            return NO;
        }
    } else {
        oct_backend backend={0};
        /* Probes on: this build is asking a host which of name and size decides whether it
         * displays an object. See oct_synthetic_options.probes. */
        oct_synthetic_options options={.probes=true};
        if(oct_synthetic_backend_create(&options,&backend)!=OCT_OK ||
           oct_store_open(&storeOptions,backend,&store)!=OCT_OK || oct_store_index_all(store)!=OCT_OK){
            oct_store_close(store);store=NULL;
            if(reason)*reason=@"Cannot initialize immutable synthetic provider";return NO;
        }
        oct_handle_list list={0};
        if(oct_store_list(store,OCT_ROOT,true,&list)!=OCT_OK ||
           list.count!=OCT_SYNTHETIC_SEED_OBJECTS+OCT_SYNTHETIC_PROBE_OBJECTS){
            oct_handle_list_free(&list);oct_store_close(store);store=NULL;
            if(reason)*reason=@"Synthetic tree identity mismatch";return NO;
        }
        oct_handle_list_free(&list);
    }
    if(oct_responder_create(store,NULL,&encoder)!=OCT_OK){
        oct_store_close(store);store=NULL;
        if(reason)*reason=@"Cannot create the provider's responder";return NO;
    }
    storeScope=scope;
    storeWritable=writableScope;
    OCTNativeProviderEvent("provider-store-opened",(uint32_t)scope,OCT_RC_OK);
    /* Tell the host the storage list changed, so it reads it again. Without this a store opened
     * after the host's first enumeration is invisible until the cable is pulled, which is exactly
     * what restarting ptpd failed to fix: the session went, and nothing asked Windows to look
     * again. It does nothing when no session is up, and the announcement is retried from the timer
     * until a host actually asks, since one sent too early is simply missed. */
    atomic_store_explicit(&announceWanted,true,memory_order_release);
    announce();
    return YES;
}

/* Switches the served store when the granted scope changes. Safe to call every poll. */
BOOL OCTProviderSetScope(oct_native_lease_mode scope, NSString **reason)
{
    if(!atomic_load_explicit(&installed,memory_order_acquire)) return NO;
    pthread_mutex_lock(&coreLock);
    BOOL ok=YES;
    if(scope!=storeScope){
        ok=openStoreLocked(scope,reason);
        /* A root that cannot be opened falls back to the synthetic tree rather than serving
         * nothing, so a host keeps seeing a working storage. */
        if(!ok&&oct_native_lease_root(scope)) openStoreLocked(OCT_NATIVE_LEASE_EXPOSE_SYNTHETIC,NULL);
    }
    pthread_mutex_unlock(&coreLock);
    return ok;
}

/*
 * Hooks that add a capability rather than make the provider work at all. Each is checked and
 * installed on its own, after the mandatory set, and a failure leaves that capability off rather
 * than the provider. Delete was first put in the mandatory table, which made a missing or differently
 * typed delete method on any build disable browsing and downloads along with it; the macOS tests
 * caught that because their stand-in camera had no delete method.
 */
static void installOptionalHooks(void) {
    NSString *why=nil;
    /* The upload path, in three parts: the name arrives with the ObjectInfo, and the bytes arrive at
     * the storage or the folder they belong in. All three must be in place, or a host would be
     * offered an upload that fails part-way; the advertisement below depends on it. */
    BOOL named = OCTRuntimeHook("PTPObjectInfoDataset","initWithData:contentType:","@28@0:8@16i24",
                                (IMP)incomingObjectInfo,&originalIncomingInfo,&why);
    BOOL onStorage = OCTRuntimeHook("PTPStorage",
                                    "appendObjectData:forObjectFormatCode:inAssociation:withContentType:",
                                    "Q36@0:8@16S24I28I32",(IMP)appendOnStorage,&originalAppendStorage,&why);
    BOOL onFolder = OCTRuntimeHook("PTPFolder",
                                   "appendObjectData:forObjectFormatCode:inAssociation:withContentType:",
                                   "Q36@0:8@16S24I28I32",(IMP)appendOnFolder,&originalAppendFolder,&why);
    if (named && (onStorage || onFolder)) {
        atomic_store_explicit(&uploadHooked,true,memory_order_release);
        OCTNativeProviderEvent("provider-upload-hooked",onStorage && onFolder ? 2u : 1u,OCT_RC_OK);
    } else {
        OCTNativeProviderEvent("provider-upload-unavailable",0,OCT_RC_OPERATION_NOT_SUPPORTED);
    }
    /* Recorded from the device: v28@0:8I16^S20, void with a uint32 handle and a response-code
     * pointer. OCTRuntimeHook checks that encoding before installing, so a method of a different
     * shape is left alone rather than called with the wrong arguments. */
    if(OCTRuntimeHook("PTPVirtualCamera","deleteObjectWithObjectHandle:responseCode:","v28@0:8I16^S20",
                      (IMP)deleteObject,&originalDelete,&why)) {
        atomic_store_explicit(&deleteHooked,true,memory_order_release);
        OCTNativeProviderEvent("provider-delete-hooked",0,OCT_RC_OK);
    } else {
        OCTNativeProviderEvent("provider-delete-unavailable",0,OCT_RC_OPERATION_NOT_SUPPORTED);
    }
}

/*
 * `scope` is the root to serve from the first request. It used to install on the sample tree and be
 * moved afterwards, which opened the store twice inside ptpd's constructor, before its own startup
 * had run: on 0.4.59 that ptpd then served no host at all, 0 commands and not one hook called,
 * while Windows kept talking to another. Opening once, on the granted root, is both correct and
 * half the work at the one moment where work is most costly.
 */
BOOL OCTProviderInstall(oct_native_lease_mode scope, NSString **reason) {
    if (atomic_load_explicit(&installed,memory_order_acquire)) return YES;
    if (!OCTProviderValidate(reason)) return NO;
    originalLookup=method_getImplementation(class_getInstanceMethod(objc_getClass("PTPVirtualCamera"),sel_registerName("objectForObjectHandle:")));
    /* Probes on: this build is asking a host which of name and size decides whether it displays
     * an object. See oct_synthetic_options.probes. */
    pthread_mutex_lock(&coreLock);
    /* A scope that names no filesystem, including none at all, serves the sample tree. */
    BOOL opened=openStoreLocked(oct_native_lease_root(scope)?scope:OCT_NATIVE_LEASE_EXPOSE_SYNTHETIC,reason);
    pthread_mutex_unlock(&coreLock);
    if(!opened) return NO;
    for(size_t i=0;i<sizeof(hooks)/sizeof(hooks[0]);i++) {
        if(!OCTRuntimeHook("PTPVirtualCamera",hooks[i].selector,hooks[i].encoding,hooks[i].replacement,hooks[i].original,reason)) {
            /* Leave pass-through hooks resident. Never uninstall over a later
             * module's replacement or free objects another thread may see. */
            OCTNativeProviderEvent("provider-install-failed",0,OCT_RC_GENERAL_ERROR);return NO;
        }
    }
    atomic_store_explicit(&installed,true,memory_order_release);
    OCTNativeProviderEvent("provider-installed",OCT_NATIVE_STORAGE_ID,OCT_RC_OK);
    installOptionalHooks();
    return YES;
}

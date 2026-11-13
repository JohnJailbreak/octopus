/* Deterministic provider boundary test. These stand-in Objective-C methods
 * exercise the real overlay/core implementation, not USB or Apple behavior. */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <CommonCrypto/CommonDigest.h>
#import "../OctopusProvider.h"
#import "../../Protocol/oct_ptp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static bool authorized;
static uint64_t authorizationEpoch=1;
static unsigned events;
bool OCTNativeProviderEnabled(void) { return authorized; }
uint64_t OCTNativeAuthorizationEpoch(void) { return authorizationEpoch; }
void OCTNativeProviderEvent(const char *event,uint32_t handle,int32_t result) {
    (void)event;(void)handle;(void)result;events++;
}

/* Stand-ins for Apple's dataset classes, with the exact method encodings the provider checks:
 * PTPDeviceInfoDataset content @16@0:8; PTPObjectInfoDataset initWithData: @24@0:8@16,
 * setObjectHandle: v20@0:8I16 and content: @20@0:8i16. standardVersion is an accessor the
 * provider does not implement, used to prove its substitute delegates to the genuine object. */
@interface PTPDeviceInfoDataset : NSObject
- (id)content;
- (uint16_t)standardVersion;
@end
@implementation PTPDeviceInfoDataset
- (id)content {
    /* A valid DeviceInfo: standard version 100, four empty arrays, playback formats [JPEG]. */
    static const uint8_t bytes[]={0x64,0,6,0,0,0,0x64,0, 0, 0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
                                  1,0,0,0, 0x01,0x38, 0,0,0,0};
    return [NSData dataWithBytes:bytes length:sizeof bytes];
}
- (uint16_t)standardVersion { return 100; }
@end
@interface PTPObjectInfoDataset : NSObject
- (instancetype)initWithData:(id)data;
/* Apple's parser for an ObjectInfo arriving from a host, recorded as @28@0:8@16i24. The provider
 * hooks it to keep the filename, which SendObject does not carry. */
- (instancetype)initWithData:(id)data contentType:(int32_t)type;
- (void)setObjectHandle:(uint32_t)handle;
- (id)content:(int32_t)type;
@end
@implementation PTPObjectInfoDataset { NSData *_body; uint32_t _objectHandle; }
- (instancetype)initWithData:(id)data { if ((self=[super init])) _body=data; return self; }
- (instancetype)initWithData:(id)data contentType:(int32_t)type {
    (void)type;
    if ((self=[super init])) _body=data;
    return self;
}
- (void)setObjectHandle:(uint32_t)handle { _objectHandle=handle; }
- (id)content:(int32_t)type {
    (void)type;
    NSMutableData *record=[NSMutableData dataWithBytes:&_objectHandle length:sizeof _objectHandle];
    [record appendData:_body];
    return record;
}
@end

@interface PTPVirtualCamera : NSObject
@property BOOL storageCollision;
@property BOOL handleCollision;
@property NSUInteger ordinaryCalls;
@property BOOL validWildcard;
- (id)allStorageIDs;
- (id)objectForObjectHandle:(id)handle;
- (id)storageInfoDatasetForStorageID:(uint32_t)storageID;
- (id)objectInfoDatasetForObjectHandle:(uint32_t)handle;
- (id)objectHandlesInStorage:(uint32_t)s forObjectFormatCode:(uint16_t)f inAssociation:(uint32_t)a responseCode:(uint16_t *)r;
- (id)dataSourceForObjectHandle:(uint32_t)h responseCode:(uint16_t *)r size:(uint32_t *)s;
- (id)objectPropValueForHandle:(uint32_t)h withObjectPropertyCode:(uint16_t)p responseCode:(uint16_t *)r;
- (id)objectPropListForHandle:(uint32_t)h withObjectFormatCode:(uint32_t)f andPropertyCode:(uint32_t)p andPropertyGroup:(uint32_t)g andDepth:(uint32_t)d responseCode:(uint16_t *)r;
- (id)objectPropertiesSupportedForObjectFormatCode:(uint16_t)f responseCode:(uint16_t *)r;
- (id)deviceInfoDataset;
- (id)objectInfoForObjectsInStorage:(uint32_t)s forObjectFormatCode:(uint16_t)f inAssociation:(uint32_t)a withContentType:(int32_t)t responseCode:(uint16_t *)r;
/* Apple's delete, recorded from the device as v28@0:8I16^S20. Here it records the handle it was
 * asked about, so a test can tell a forwarded call from one the provider answered itself. */
- (void)deleteObjectWithObjectHandle:(uint32_t)handle responseCode:(unsigned short *)code;
@end
/* The observer holds the USB transport, and this build does not link it. The announcement is
 * recorded instead, so a test can tell that opening a store asks for one. */
static uint32_t lastAnnouncedStorage;
bool OCTNativeAnnounceStorage(uint32_t storage) {
    lastAnnouncedStorage = storage;
    return false;   /* no session in a test, which is what the real one reports then too */
}

/* The last response the provider sent to a host, for the upload tests: the observer holds the
 * transport and is not linked here. */
static uint16_t lastResponseCode;
static uint32_t lastResponseTransaction, lastResponseHandle;
static unsigned lastResponseParameters;
bool OCTNativeSendResponse(uint16_t code, uint32_t transaction, unsigned parameters,
                           uint32_t one, uint32_t two, uint32_t three) {
    (void)one; (void)two;
    lastResponseCode = code;
    lastResponseTransaction = transaction;
    lastResponseParameters = parameters;
    lastResponseHandle = three;
    return true;
}

/* Every event the provider sends, in order, for the refresh sequence to be checked. */
static uint16_t sentEvents[16];
static unsigned sentEventCount;
/* Whether a send succeeds. False by default: no session in a test, which is what the real one
 * reports then too. Set to check that a repeated event stops once it has gone. */
static BOOL sendSucceeds;
bool OCTNativeSendEvent(uint16_t code, uint32_t parameter) {
    (void)parameter;
    if (sentEventCount < 16) sentEvents[sentEventCount++] = code;
    return sendSucceeds;
}

/* One array of a DeviceInfo dataset: its element count and where its elements start. Walks the
 * fixed header, the vendor description string and each array before the one asked for, so nothing
 * depends on how long anything earlier has grown. */
static BOOL infoArray(const uint8_t *b, size_t n, unsigned index, uint32_t *count, size_t *at) {
    size_t p = 2 + 4 + 2;                    /* StandardVersion, VendorExtensionID, its version */
    if (p >= n) return NO;
    p += 1 + (size_t)b[p] * 2;               /* VendorExtensionDesc: a length, then UTF-16 units */
    p += 2;                                  /* FunctionalMode */
    for (unsigned i = 0; ; i++) {
        if (p + 4 > n) return NO;
        uint32_t c = (uint32_t)b[p] | (uint32_t)b[p+1] << 8 | (uint32_t)b[p+2] << 16 | (uint32_t)b[p+3] << 24;
        if (i == index) { *count = c; *at = p + 4; return p + 4 + (size_t)c * 2 <= n; }
        p += 4 + (size_t)c * 2;
    }
}

/* Whether that array lists a value. */
static BOOL infoHas(NSData *d, unsigned index, uint16_t value) {
    uint32_t count = 0; size_t at = 0;
    if (!infoArray(d.bytes, d.length, index, &count, &at)) return NO;
    const uint8_t *b = d.bytes;
    for (uint32_t i = 0; i < count; i++)
        if ((uint16_t)(b[at + 2*i] | b[at + 2*i + 1] << 8) == value) return YES;
    return NO;
}

/* A request or data packet as Apple's responder would be given one: the operation, the transaction,
 * the two parameters an upload uses, and the data phase's bytes. Encodings as recorded from the
 * device: operationCode S16@0:8, transactionID and parameter1/2 I16@0:8, data @16@0:8. */
@interface OctopusTestPacket : NSObject
@property (nonatomic) unsigned short operation;
@property (nonatomic) unsigned int transaction;
@property (nonatomic) unsigned int one;
@property (nonatomic) unsigned int two;
@property (nonatomic, strong) NSData *body;
@end
@implementation OctopusTestPacket
- (unsigned short)operationCode { return self.operation; }
- (unsigned int)transactionID { return self.transaction; }
- (unsigned int)parameter1 { return self.one; }
- (unsigned int)parameter2 { return self.two; }
- (id)data { return self.body; }
@end

/* An ObjectInfo as a host sends it with SendObjectInfo: the fixed fields, then the filename as a
 * counted UTF-16 string, then three empty date/keyword strings. */
static NSData *objectInfoFor(const char *name, uint32_t size, uint32_t parent, uint16_t format) {
    NSMutableData *info = [NSMutableData data];
    uint32_t storage = OCT_NATIVE_STORAGE_ID;
    [info appendBytes:&storage length:4];
    [info appendBytes:&format length:2];
    uint16_t protection = 0;
    [info appendBytes:&protection length:2];
    [info appendBytes:&size length:4];
    uint8_t zeros[2 + 4 * 6] = {0};            /* thumb format, sizes, image dimensions */
    [info appendBytes:zeros length:sizeof zeros];
    [info appendBytes:&parent length:4];
    uint16_t association = format == OCT_FORMAT_ASSOCIATION ? 1 : 0;
    [info appendBytes:&association length:2];
    uint32_t associationDesc = 0;
    [info appendBytes:&associationDesc length:4];
    uint32_t sequence = 0;
    [info appendBytes:&sequence length:4];
    size_t length = strlen(name);
    uint8_t count = (uint8_t)(length + 1);     /* PTP counts the terminator */
    [info appendBytes:&count length:1];
    for (size_t i = 0; i < length; i++) {
        uint16_t unit = (uint16_t)name[i];
        [info appendBytes:&unit length:2];
    }
    uint16_t terminator = 0;
    [info appendBytes:&terminator length:2];
    uint8_t empty = 0;
    for (int i = 0; i < 3; i++) [info appendBytes:&empty length:1];  /* created, modified, keywords */
    return info;
}

/* The last handle Apple's own delete was asked about, or zero. */
static uint32_t lastAppleDelete;
/* The association Apple's own upload receiver was asked about, or zero: an upload into Apple's own
 * storage must reach Apple's implementation, not ours. */
static uint32_t lastAppleAppend;

/* The storage and folder an incoming object is appended to. Apple calls one of these with the
 * bytes, the format, the parent and a content type, returning the new object's handle.
 * Recorded as Q36@0:8@16S24I28I32. */
@interface PTPStorage : NSObject
- (unsigned long long)appendObjectData:(id)data forObjectFormatCode:(unsigned short)format
                         inAssociation:(unsigned int)association withContentType:(unsigned int)type;
@end
@implementation PTPStorage
- (unsigned long long)appendObjectData:(id)data forObjectFormatCode:(unsigned short)format
                         inAssociation:(unsigned int)association withContentType:(unsigned int)type {
    (void)data; (void)format; (void)type;
    lastAppleAppend = association;
    return 0x1234;   /* a handle of Apple's own, distinguishable from ours */
}
@end

@interface PTPFolder : NSObject
- (unsigned long long)appendObjectData:(id)data forObjectFormatCode:(unsigned short)format
                         inAssociation:(unsigned int)association withContentType:(unsigned int)type;
@end
@implementation PTPFolder
- (unsigned long long)appendObjectData:(id)data forObjectFormatCode:(unsigned short)format
                         inAssociation:(unsigned int)association withContentType:(unsigned int)type {
    (void)data; (void)format; (void)type;
    lastAppleAppend = association;
    return 0x5678;
}
@end

@implementation PTPVirtualCamera
- (id)deviceInfoDataset { return [PTPDeviceInfoDataset new]; }
- (void)deleteObjectWithObjectHandle:(uint32_t)handle responseCode:(unsigned short *)code {
    lastAppleDelete=handle;
    if(code)*code=(unsigned short)OCT_RC_OK;
}
- (id)objectInfoForObjectsInStorage:(uint32_t)s forObjectFormatCode:(uint16_t)f inAssociation:(uint32_t)a withContentType:(int32_t)t responseCode:(uint16_t *)r {
    (void)f;(void)a;(void)t;self.ordinaryCalls++;*r=s==0x10001?OCT_RC_OK:OCT_RC_INVALID_STORAGE_ID;
    return s==0x10001?[@"photo-bulk" dataUsingEncoding:NSUTF8StringEncoding]:nil;
}
- (id)allStorageIDs { return self.storageCollision ? @[@0x10001,@(OCT_NATIVE_STORAGE_ID)] : @[@0x10001]; }
- (id)objectForObjectHandle:(id)handle {
    if([handle unsignedIntValue]==42)return @"photo";
    return self.handleCollision && [handle unsignedIntValue]==OCT_NATIVE_FIRST_HANDLE ? @"collision" : nil;
}
- (id)storageInfoDatasetForStorageID:(uint32_t)s { self.ordinaryCalls++;return s==0x10001?@"photo-storage":nil; }
- (id)objectInfoDatasetForObjectHandle:(uint32_t)h { self.ordinaryCalls++;return h==42?@"photo-info":nil; }
- (id)objectHandlesInStorage:(uint32_t)s forObjectFormatCode:(uint16_t)f inAssociation:(uint32_t)a responseCode:(uint16_t *)r {
    (void)f;(void)a;self.ordinaryCalls++;*r=(s==0x10001||s==UINT32_MAX)?OCT_RC_OK:OCT_RC_INVALID_STORAGE_ID;
    return *r==OCT_RC_OK?@[@42]:nil;
}
- (id)dataSourceForObjectHandle:(uint32_t)h responseCode:(uint16_t *)r size:(uint32_t *)s {
    self.ordinaryCalls++;*r=h==42?OCT_RC_OK:OCT_RC_INVALID_OBJECT_HANDLE;*s=h==42?5:0;return h==42?@"photo":nil;
}
- (id)objectPropValueForHandle:(uint32_t)h withObjectPropertyCode:(uint16_t)p responseCode:(uint16_t *)r {
    (void)p;self.ordinaryCalls++;*r=h==42?OCT_RC_OK:OCT_RC_INVALID_OBJECT_HANDLE;return h==42?[@"photo" dataUsingEncoding:NSUTF8StringEncoding]:nil;
}
- (id)objectPropListForHandle:(uint32_t)h withObjectFormatCode:(uint32_t)f andPropertyCode:(uint32_t)p andPropertyGroup:(uint32_t)g andDepth:(uint32_t)d responseCode:(uint16_t *)r {
    (void)h;(void)f;(void)p;(void)g;(void)d;self.ordinaryCalls++;*r=OCT_RC_OK;
    if(!self.validWildcard)return nil;
    /* One native property: photo handle 42, UINT16 ObjectFormat, JPEG. */
    const uint8_t bytes[]={1,0,0,0,42,0,0,0,2,0xdc,4,0,1,0x38};
    return [NSData dataWithBytes:bytes length:sizeof(bytes)];
}
- (id)objectPropertiesSupportedForObjectFormatCode:(uint16_t)f responseCode:(uint16_t *)r {
    (void)f;self.ordinaryCalls++;*r=OCT_RC_OK;return @[@0xdc02];
}
@end

@interface InheritedCamera : PTPVirtualCamera @end
@implementation InheritedCamera @end

@interface NSObject (ProviderResultTest)
- (NSData *)content;
- (NSString *)filename;
- (uint64_t)length;
- (int64_t)read:(void *)buffer length:(uint64_t)length offset:(int64_t)offset;
@end

static uint32_t findNamed(PTPVirtualCamera *camera,NSArray *handles,NSString *name) {
    for(NSNumber *h in handles)if([[[camera objectInfoDatasetForObjectHandle:h.unsignedIntValue] filename] isEqualToString:name])return h.unsignedIntValue;
    return 0;
}

int main(void) { @autoreleasepool {
    NSString *reason=nil;
    assert(!OCTRuntimeCheck("MissingCamera","allStorageIDs","@16@0:8",&reason));
    assert(!OCTRuntimeCheck("InheritedCamera","allStorageIDs","@16@0:8",&reason));
    assert(!OCTRuntimeCheck("PTPVirtualCamera","allStorageIDs","v16@0:8",&reason));
    /* The reason is printed, because a bare assertion here says only that something did not match,
     * and the last time it failed that was the whole of the evidence. */
    if(!OCTProviderValidate(&reason)){fprintf(stderr,"validate: %s\n",reason.UTF8String);abort();}
    /* The sample tree, which is what a grant naming no filesystem serves. */
    lastAnnouncedStorage=0;
    sentEventCount=0;
    if(!OCTProviderInstall(OCT_NATIVE_LEASE_EXPOSE_SYNTHETIC,&reason)){fprintf(stderr,"install: %s\n",reason.UTF8String);abort();}
    /* Nothing is announced while the store opens: that runs holding coreLock, and an announcement
     * that called back into a hook taking it would freeze ptpd. The timer does it instead. */
    assert(sentEventCount==0);
    assert(lastAnnouncedStorage==0);
    OCTProviderAnnounceIfNeeded();
    /* The first announcement after a store opens is a disconnect of that storage alone: removed,
     * then added, then DeviceInfo to be read again. No camera seen yet, so all by the observer. */
    assert(sentEventCount==2);
    assert(sentEvents[0]==OCT_EVENT_STORE_REMOVED);
    assert(sentEvents[1]==OCT_EVENT_DEVICE_INFO_CHANGED);
    /* Retries only add: no second removal, no second DeviceInfoChanged, so the drive does not flicker. */
    sentEventCount=0;
    /* The timer's announcement named our storage. No camera has been seen yet, so it went the
     * observer's way, the fallback; a host that has not reached a camera hook has not enumerated. */
    assert(lastAnnouncedStorage==OCT_NATIVE_STORAGE_ID);
    /* And it keeps asking to be announced until a host reads the storage list. */
    lastAnnouncedStorage=0;
    OCTProviderAnnounceIfNeeded();
    assert(lastAnnouncedStorage==OCT_NATIVE_STORAGE_ID);
    /* A retry never removes the storage again, so the drive does not flicker. DeviceInfoChanged is
     * repeated, because the first did not go: on 0.4.66 it was tried once, failed silently, and
     * Windows kept the DeviceInfo it read before Octopus served, with no uploads in it. */
    assert(sentEventCount==1 && sentEvents[0]==OCT_EVENT_DEVICE_INFO_CHANGED);
    /* Once it goes, it stops. */
    sendSucceeds=YES; sentEventCount=0;
    OCTProviderAnnounceIfNeeded();
    assert(sentEventCount==1 && sentEvents[0]==OCT_EVENT_DEVICE_INFO_CHANGED);
    sentEventCount=0;
    OCTProviderAnnounceIfNeeded();
    assert(sentEventCount==0);
    sendSucceeds=NO;
    if(!OCTProviderInstall(OCT_NATIVE_LEASE_EXPOSE_SYNTHETIC,&reason)){fprintf(stderr,"reinstall: %s\n",reason.UTF8String);abort();}
    PTPVirtualCamera *camera=[PTPVirtualCamera new];
    assert([[camera allStorageIDs] isEqual:@[@0x10001]]);
    assert(object_getClass([camera deviceInfoDataset])==[PTPDeviceInfoDataset class]); /* untouched while unauthorized */
    authorized=true;
    assert(([[camera allStorageIDs] isEqual:@[@0x10001,@(OCT_NATIVE_STORAGE_ID)]]));
    /* A host has now read the list, so the announcement stops. */
    lastAnnouncedStorage=0;
    OCTProviderAnnounceIfNeeded();
    assert(lastAnnouncedStorage==0);
    assert([[camera storageInfoDatasetForStorageID:0x10001] isEqual:@"photo-storage"]);
    assert([[camera objectInfoDatasetForObjectHandle:42] isEqual:@"photo-info"]);
    NSData *storage=[[camera storageInfoDatasetForStorageID:OCT_NATIVE_STORAGE_ID] content];assert(storage.length>20);
    oct_reader sr;oct_reader_init(&sr,storage.bytes,storage.length);(void)oct_read_u16(&sr);(void)oct_read_u16(&sr);assert(oct_read_u16(&sr)==1);
    uint16_t code=0;uint32_t size=0;
    NSArray *all=[camera objectHandlesInStorage:OCT_NATIVE_STORAGE_ID forObjectFormatCode:0 inAssociation:0 responseCode:&code];
    /* Derived from the constants: the provider enables the probe objects, so a literal here
     * breaks the moment that changes, which is exactly what happened. */
    const NSUInteger expectedObjects=OCT_SYNTHETIC_SEED_OBJECTS+OCT_SYNTHETIC_PROBE_OBJECTS;
    assert(code==OCT_RC_OK&&all.count==expectedObjects);
    NSArray *combined=[camera objectHandlesInStorage:UINT32_MAX forObjectFormatCode:0 inAssociation:0 responseCode:&code];
    assert(code==OCT_RC_OK&&combined.count==expectedObjects+1&&[combined containsObject:@42]);
    NSArray *root=[camera objectHandlesInStorage:OCT_NATIVE_STORAGE_ID forObjectFormatCode:0 inAssociation:UINT32_MAX responseCode:&code];
    assert(code==OCT_RC_OK&&root.count==3);
    uint32_t docs=findNamed(camera,root,@"Documents");assert(docs);
    NSArray *children=[camera objectHandlesInStorage:UINT32_MAX forObjectFormatCode:0 inAssociation:docs responseCode:&code];
    /* example.txt and sample.bin, plus the two probes the provider enables. */
    assert(code==OCT_RC_OK&&children.count==2+OCT_SYNTHETIC_PROBE_OBJECTS&&![children containsObject:@42]);
    /* Every probe is reachable through the provider and every one is the same size, so a host
     * listing some and not others is deciding on the extension alone. */
    NSArray<NSString *> *probeNames=@[@"probe.dat",@"probe.txt",@"probe.jpg",@"probe.png",@"probe.pdf",
                                      @"probe.json",@"probe.log",@"probe.mov",@"probe.plist",@"probe"];
    assert(probeNames.count==OCT_SYNTHETIC_PROBE_OBJECTS);
    for(NSString *probeName in probeNames){
        uint32_t probeHandle=findNamed(camera,children,probeName);assert(probeHandle);
        uint16_t probeCode=0;uint32_t probeSize=0;
        id probeSource=[camera dataSourceForObjectHandle:probeHandle responseCode:&probeCode size:&probeSize];
        assert(probeCode==OCT_RC_OK&&probeSize==76&&[probeSource length]==76);
    }
    uint32_t example=findNamed(camera,children,@"example.txt");assert(example);
    /* Delete, through the optional hook. A handle that is not ours goes to Apple's implementation
     * untouched, so photo deletion is unaffected. One of ours is answered by the provider and never
     * reaches Apple's, which knows nothing of these handles; the sample tree is not writable, so the
     * answer is a refusal, and the object is still there afterwards. */
    unsigned short deleteCode=0xFFFF;
    lastAppleDelete=0;
    [camera deleteObjectWithObjectHandle:42 responseCode:&deleteCode];
    assert(lastAppleDelete==42&&deleteCode==OCT_RC_OK);           /* Apple's: forwarded */
    lastAppleDelete=0;deleteCode=0xFFFF;
    [camera deleteObjectWithObjectHandle:example responseCode:&deleteCode];
    assert(lastAppleDelete==0);                                      /* ours: never forwarded */
    assert(deleteCode!=OCT_RC_OK&&deleteCode!=0xFFFF);               /* answered, and refused */
    /* A fresh listing, not the array fetched earlier: re-searching that would prove nothing. */
    uint16_t relistCode=0;
    NSArray *relisted=[camera objectHandlesInStorage:UINT32_MAX forObjectFormatCode:0 inAssociation:docs responseCode:&relistCode];
    assert(relistCode==OCT_RC_OK&&findNamed(camera,relisted,@"example.txt")==example); /* not deleted */
    /* Uploads. An object bound for Apple's own storage reaches Apple's receiver untouched; one
     * bound for ours never does. The sample tree accepts no changes, so ours is refused rather
     * than written, which is the correct answer for a read-only store and proves the routing. */
    PTPStorage *appleStorage=[PTPStorage new];
    PTPFolder *appleFolder=[PTPFolder new];
    NSData *payload=[@"uploaded" dataUsingEncoding:NSUTF8StringEncoding];
    lastAppleAppend=0;
    unsigned long long theirs=[appleStorage appendObjectData:payload forObjectFormatCode:OCT_FORMAT_UNDEFINED
                                               inAssociation:42 withContentType:0];
    assert(lastAppleAppend==42&&theirs==0x1234);            /* forwarded */
    lastAppleAppend=0;
    unsigned long long refused=[appleStorage appendObjectData:payload forObjectFormatCode:OCT_FORMAT_UNDEFINED
                                                inAssociation:docs withContentType:0];
    assert(lastAppleAppend==0);                             /* ours: never forwarded */
    assert(refused==0);                                     /* and refused: the sample tree is read-only */
    lastAppleAppend=0;
    unsigned long long viaFolder=[appleFolder appendObjectData:payload forObjectFormatCode:OCT_FORMAT_UNDEFINED
                                                 inAssociation:42 withContentType:0];
    assert(lastAppleAppend==42&&viaFolder==0x5678);         /* the folder path forwards too */
    /* And the object count is unchanged: nothing was created by any of that. */
    NSArray *afterUpload=[camera objectHandlesInStorage:UINT32_MAX forObjectFormatCode:0 inAssociation:docs responseCode:&code];
    assert(code==OCT_RC_OK&&afterUpload.count==children.count);

    /* Uploads through Apple's responder. The sample tree accepts no changes, so the file cannot be
     * created, which is the right answer for a read-only store and still exercises the whole path:
     * the request is taken over, the data is received, and the host is answered rather than left
     * waiting as Apple leaves it. */
    OctopusTestPacket *request = [OctopusTestPacket new];
    request.operation = OCT_OP_SEND_OBJECT_INFO;
    request.transaction = 77;
    request.one = OCT_NATIVE_STORAGE_ID;
    request.two = docs;
    id answer = nil;
    assert(OCTProviderHandleRequest(request, &answer));
    assert([answer isKindOfClass:[NSNumber class]] && [answer boolValue]);   /* read the data phase */

    /* An upload to Apple's own storage is not ours: Apple must answer it. */
    OctopusTestPacket *theirs = [OctopusTestPacket new];
    theirs.operation = OCT_OP_SEND_OBJECT_INFO;
    theirs.transaction = 78;
    theirs.one = 0x10001;
    id theirAnswer = nil;
    assert(!OCTProviderHandleRequest(theirs, &theirAnswer));
    assert(theirAnswer == nil);
    /* And neither is a read. */
    OctopusTestPacket *read = [OctopusTestPacket new];
    read.operation = OCT_OP_GET_OBJECT;
    read.transaction = 79;
    assert(!OCTProviderHandleRequest(read, &theirAnswer));

    /* The ObjectInfo arrives. The store refuses, so the host is told so with no handle. */
    lastResponseCode = 0; lastResponseTransaction = 0; lastResponseParameters = 99;
    OctopusTestPacket *info = [OctopusTestPacket new];
    info.operation = OCT_OP_SEND_OBJECT_INFO;
    info.transaction = 77;
    info.body = objectInfoFor("upload.txt", 299, docs, OCT_FORMAT_UNDEFINED);
    assert(OCTProviderHandleData(info));
    assert(lastResponseTransaction == 77);
    assert(lastResponseCode != OCT_RC_OK);          /* the sample tree accepts no changes */
    assert(lastResponseParameters == 0);            /* and no handle is offered */

    /* A second data phase with nothing pending is Apple's, not ours. */
    assert(!OCTProviderHandleData(info));

    /* An object larger than one USB read is refused when its ObjectInfo arrives, not left to
     * vanish in the transport, which is what hangs a host. */
    lastResponseCode = 0;
    assert(OCTProviderHandleRequest(request, &answer));
    OctopusTestPacket *big = [OctopusTestPacket new];
    big.operation = OCT_OP_SEND_OBJECT_INFO;
    big.transaction = 77;
    big.body = objectInfoFor("big.bin", OCT_NATIVE_MAX_UPLOAD_BYTES + 1, docs, OCT_FORMAT_UNDEFINED);
    assert(OCTProviderHandleData(big));
    assert(lastResponseCode == OCT_RC_DEVICE_BUSY);
    /* Exactly at the limit is not refused for size: it reaches the store, which refuses it. */
    lastResponseCode = 0;
    assert(OCTProviderHandleRequest(request, &answer));
    OctopusTestPacket *edge = [OctopusTestPacket new];
    edge.operation = OCT_OP_SEND_OBJECT_INFO;
    edge.transaction = 77;
    edge.body = objectInfoFor("edge.bin", OCT_NATIVE_MAX_UPLOAD_BYTES, docs, OCT_FORMAT_UNDEFINED);
    assert(OCTProviderHandleData(edge));
    assert(lastResponseCode != OCT_RC_DEVICE_BUSY);

    /* SendObject with no accepted ObjectInfo before it is Apple's too. */
    OctopusTestPacket *orphan = [OctopusTestPacket new];
    orphan.operation = OCT_OP_SEND_OBJECT;
    orphan.transaction = 80;
    assert(!OCTProviderHandleRequest(orphan, &theirAnswer));

    /* Malformed ObjectInfo: answered, not ignored, so the host is never left waiting. */
    lastResponseCode = 0;
    assert(OCTProviderHandleRequest(request, &answer));
    OctopusTestPacket *broken = [OctopusTestPacket new];
    broken.operation = OCT_OP_SEND_OBJECT_INFO;
    broken.transaction = 77;
    broken.body = [NSData dataWithBytes:"\x01\x02\x03" length:3];
    assert(OCTProviderHandleData(broken));
    assert(lastResponseCode != OCT_RC_OK);

    /* Nothing was created by any of that. */
    NSArray *afterUploads=[camera objectHandlesInStorage:UINT32_MAX forObjectFormatCode:0 inAssociation:docs responseCode:&code];
    assert(code==OCT_RC_OK&&afterUploads.count==children.count);

    id source=[camera dataSourceForObjectHandle:example responseCode:&code size:&size];
    assert(code==OCT_RC_OK&&size==76&&[source length]==76);
    uint8_t bytes[76],partial[19];
    assert([source read:bytes length:76 offset:0]==76);
    assert([source read:partial length:sizeof(partial) offset:13]==(int64_t)sizeof(partial));assert(!memcmp(bytes+13,partial,sizeof(partial)));
    id info=[camera deviceInfoDataset];
    assert(object_getClass(info)!=[PTPDeviceInfoDataset class]);         /* the provider's copy */
    NSData *infoBody=[info content];
    /* Read array by array, not by byte position. Everything the provider adds moves what follows,
     * and fixed offsets broke the moment events were advertised for every store: they sit before
     * the formats and pushed them eight bytes along. Arrays: 0 operations, 1 events, 2 device
     * properties, 3 capture formats, 4 playback formats. */
    assert(infoHas(infoBody,4,0x3801) && infoHas(infoBody,4,OCT_FORMAT_TEXT));   /* JPEG kept, text added */
    /* The events a host must expect to act on an announcement, offered for this read-only store
     * too: appearing without the cable pulled matters whether or not the drive can be written. */
    assert(infoHas(infoBody,1,OCT_EVENT_STORE_ADDED) && infoHas(infoBody,1,OCT_EVENT_STORE_REMOVED));
    assert(infoHas(infoBody,1,OCT_EVENT_DEVICE_INFO_CHANGED) && infoHas(infoBody,1,OCT_EVENT_OBJECT_ADDED));
    /* And nothing that writes. The sample tree accepts no changes, so no upload or delete is offered
     * and neither a file nor a folder is listed as creatable: a host offered one would fail part-way
     * through a copy. Never asserted before, so a write path leaking into a read-only store would
     * have gone unnoticed. */
    assert(!infoHas(infoBody,0,OCT_OP_SEND_OBJECT_INFO) && !infoHas(infoBody,0,OCT_OP_SEND_OBJECT));
    assert(!infoHas(infoBody,0,OCT_OP_DELETE_OBJECT));
    assert(!infoHas(infoBody,4,OCT_FORMAT_UNDEFINED) && !infoHas(infoBody,4,OCT_FORMAT_ASSOCIATION));
    assert(!infoHas(infoBody,3,OCT_FORMAT_UNDEFINED) && !infoHas(infoBody,3,OCT_FORMAT_ASSOCIATION));
    /* The size follows: Apple's 37 bytes, two for the text format, eight for four events. */
    assert(infoBody.length==37+2+8);
    /* A grant that allows writing offers it in DeviceInfo at once, before the store has moved off
     * the sample tree. The first DeviceInfo of a session is the only one Windows reads: on 0.4.67
     * DeviceInfoChanged went out and Windows never asked again. */
    OCTProviderSetWritesGranted(true);
    NSData *granted=[[camera deviceInfoDataset] content];
    assert(infoHas(granted,0,OCT_OP_SEND_OBJECT_INFO) && infoHas(granted,0,OCT_OP_SEND_OBJECT));
    assert(infoHas(granted,0,OCT_OP_DELETE_OBJECT));
    assert(infoHas(granted,4,OCT_FORMAT_UNDEFINED) && infoHas(granted,4,OCT_FORMAT_ASSOCIATION));
    assert(infoHas(granted,3,OCT_FORMAT_UNDEFINED) && infoHas(granted,3,OCT_FORMAT_ASSOCIATION));
    assert(infoHas(granted,1,OCT_EVENT_DEVICE_INFO_CHANGED));          /* the events stay */
    /* Withdrawn, and the sample tree accepts no changes, so nothing that writes is offered again. */
    OCTProviderSetWritesGranted(false);
    NSData *withdrawn=[[camera deviceInfoDataset] content];
    assert(!infoHas(withdrawn,0,OCT_OP_SEND_OBJECT_INFO) && !infoHas(withdrawn,0,OCT_OP_DELETE_OBJECT));
    assert(!infoHas(withdrawn,4,OCT_FORMAT_UNDEFINED));
    assert(withdrawn.length==infoBody.length);
    assert([info respondsToSelector:@selector(standardVersion)]);
    assert([info standardVersion]==100);                                  /* delegated to Apple's object */
    /* Bulk object info for our storage: type 3 carries an 8-byte count, then one record each. */
    NSData *bulk=[camera objectInfoForObjectsInStorage:OCT_NATIVE_STORAGE_ID forObjectFormatCode:0 inAssociation:0 withContentType:3 responseCode:&code];
    assert(code==OCT_RC_OK && bulk.length>8);
    uint64_t bulkCount=0;for(unsigned i=0;i<8;i++)bulkCount|=(uint64_t)((const uint8_t *)bulk.bytes)[i]<<(i*8);
    assert(bulkCount==expectedObjects);
    /* Message syntax: the method returns id, and dot syntax needs a static type. */
    assert([[camera objectInfoForObjectsInStorage:OCT_NATIVE_STORAGE_ID forObjectFormatCode:0 inAssociation:0 withContentType:1 responseCode:&code] length]>0 && code==OCT_RC_OK);
    NSUInteger before=camera.ordinaryCalls;
    assert([[camera objectInfoForObjectsInStorage:0x10001 forObjectFormatCode:0 inAssociation:0 withContentType:3 responseCode:&code] isEqual:[@"photo-bulk" dataUsingEncoding:NSUTF8StringEncoding]]);
    assert(camera.ordinaryCalls==before+1);                               /* Photos goes to Apple */
    [camera objectInfoForObjectsInStorage:OCT_NATIVE_STORAGE_ID forObjectFormatCode:0 inAssociation:0 withContentType:4 responseCode:&code];
    assert(camera.ordinaryCalls==before+2);                               /* an unknown type goes to Apple */
    uint8_t hash[CC_SHA256_DIGEST_LENGTH];CC_SHA256(bytes,sizeof(bytes),hash);
    NSMutableString *hex=[NSMutableString string];for(size_t i=0;i<sizeof(hash);i++)[hex appendFormat:@"%02x",hash[i]];
    assert([hex isEqualToString:@"aeeb6cba90d7d6adfcd42d673f64897f26cfcc479c5cf489a86146e09a9d2bd2"]);
    assert([source read:partial length:19 offset:76]==0);assert([source read:partial length:19 offset:77]==-1);
    assert([source read:partial length:19 offset:-1]==-1);assert([source read:NULL length:1 offset:0]==-1);
    /* Compare to the independently exercised portable provider and responder. */
    oct_backend backend;oct_store *store=NULL;oct_synthetic_options options={0};
    assert(oct_synthetic_backend_create(&options,&backend)==OCT_OK);assert(oct_store_open(NULL,backend,&store)==OCT_OK);
    uint32_t folder,file;assert(oct_store_find(store,0,"Documents",&folder)==OCT_OK);assert(oct_store_find(store,folder,"example.txt",&file)==OCT_OK);
    uint8_t expected[76];size_t count=0;assert(oct_store_read(store,file,0,expected,sizeof(expected),&count)==OCT_OK);assert(count==76&&!memcmp(expected,bytes,76));oct_store_close(store);
    NSData *protection=[camera objectPropValueForHandle:example withObjectPropertyCode:OCT_PROP_PROTECTION_STATUS responseCode:&code];
    assert(code==OCT_RC_OK&&protection.length==2&&((const uint8_t *)protection.bytes)[0]==1);
    assert([camera objectPropValueForHandle:example withObjectPropertyCode:0xdead responseCode:&code]==nil&&code==OCT_RC_INVALID_OBJECT_PROP_CODE);
    NSData *props=[camera objectPropListForHandle:example withObjectFormatCode:0 andPropertyCode:UINT32_MAX andPropertyGroup:0 andDepth:0 responseCode:&code];
    assert(code==OCT_RC_OK&&props.length>4);
    assert([camera objectPropListForHandle:UINT32_MAX withObjectFormatCode:0 andPropertyCode:UINT32_MAX andPropertyGroup:0 andDepth:0 responseCode:&code]==nil);
    camera.validWildcard=YES;
    NSData *merged=[camera objectPropListForHandle:UINT32_MAX withObjectFormatCode:0 andPropertyCode:UINT32_MAX andPropertyGroup:0 andDepth:0 responseCode:&code];
    oct_reader mr;oct_reader_init(&mr,merged.bytes,merged.length);assert(oct_read_u32(&mr)>1);assert(oct_read_u32(&mr)==42);
    camera.handleCollision=YES;assert([[camera allStorageIDs] isEqual:@[@0x10001]]);
    assert([camera objectInfoDatasetForObjectHandle:example]==nil);
    camera.handleCollision=NO;camera.storageCollision=YES;
    assert([[camera allStorageIDs] count]==2); /* no duplicate synthetic ID */
    assert([camera storageInfoDatasetForStorageID:OCT_NATIVE_STORAGE_ID]==nil);
    camera.storageCollision=NO;
    authorized=false;authorizationEpoch++;
    assert([[camera allStorageIDs] isEqual:@[@0x10001]]);
    assert(object_getClass([camera deviceInfoDataset])==[PTPDeviceInfoDataset class]); /* revoked: Apple's again */
    assert([camera dataSourceForObjectHandle:example responseCode:&code size:&size]==nil);
    assert([source read:bytes length:1 offset:0]==-1); /* queued data source revocation */
    authorized=true;
    assert([source read:bytes length:1 offset:0]==-1); /* old lease cannot resume */
    assert([[camera objectInfoDatasetForObjectHandle:42] isEqual:@"photo-info"]);
    assert(camera.ordinaryCalls>0&&events>0);
    puts("PASS native provider: hierarchy, merged Photos enumeration, DeviceInfo text format and delegation, bulk object info, data/partial reads, properties, collisions, revocation (LOCAL boundary test)");
} return 0; }

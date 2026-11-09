#import "OctopusRuntime.h"
#import <mach-o/dyld.h>
#import <CommonCrypto/CommonDigest.h>
#include "../Core/oct_device.h"
#include "../Core/oct_link.h"
#include "../Core/oct_native.h"
#include "../Core/oct_native_io.h"
#include <stdatomic.h>
#include <pthread.h>
#include <sys/sysctl.h>
#include <unistd.h>

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static oct_native_journal journal;
static _Atomic(uint64_t) deadline;
static _Atomic(unsigned) mode;
static _Atomic(bool) ready;
static _Atomic(bool) providerReady;
static _Atomic(unsigned) observedStorage;
static _Atomic(uint64_t) leaseEpoch;
static __thread uint64_t usbScope;
static IMP originalBulk,originalRequest,originalResponse,originalData,originalConnected,originalStop;
static IMP requestOp,requestTID,responseCode,responseTID,dataTID,transportConnected;
static void *transportPointer;
static uint64_t transportToken;
static NSString *compatibility=@"Not checked", *hookState=@"Not installed", *providerState=@"Not installed";
static NSString *leaseToken=@"",*kernelBuild=@"unknown",*lastProviderEvent=@"none";
static uint64_t providerEvents;
static uint64_t leaseSequence,leaseCommands,leaseResponses,leaseProviderEvents;
static bool attemptedHooks,attemptedProvider;
static dispatch_source_t timer;
static uint64_t now(void){int64_t n=oct_link_now_ms();return n>0?(uint64_t)n:0;}
static bool authorized(void){return atomic_load(&ready)&&atomic_load(&mode)>0&&now()<atomic_load(&deadline);}
bool OCTNativeProviderEnabled(void){return authorized()&&atomic_load(&mode)==2&&atomic_load(&providerReady)&&atomic_load(&observedStorage)==3;}
uint64_t OCTNativeAuthorizationEpoch(void){return atomic_load(&leaseEpoch);}
void OCTNativeProviderEvent(const char *event,uint32_t handle,int32_t result){
    pthread_mutex_lock(&lock);providerEvents++;lastProviderEvent=[NSString stringWithFormat:@"%s handle=%u result=%d",event,handle,result];pthread_mutex_unlock(&lock);
}
static uint64_t tokenFor(id transport){
    void *p=(__bridge void*)transport;
    if(transportPointer!=p){transportPointer=p;transportToken++;}
    return transportToken;
}
static bool packetIs(id packet,const char *name){return packet&&object_getClass(packet)==objc_getClass(name);}
static BOOL bulk(id self,SEL cmd,uint64_t bytes,int32_t result){
    uint64_t previous=usbScope,scope=0;
    if(authorized()){
        // Read the verified native connection property only during a real bulk
        // callback. This also supports enabling after Apple's connection setup.
        BOOL isConnected=((BOOL(*)(id,SEL))transportConnected)(self,sel_registerName("connected"));
        pthread_mutex_lock(&lock);
        oct_native_connect(&journal,tokenFor(self),isConnected);
        scope=oct_native_receive_begin(&journal,tokenFor(self),result,bytes>UINT32_MAX?UINT32_MAX:(uint32_t)bytes);
        pthread_mutex_unlock(&lock);
    }
    usbScope=scope;
    @try{return ((BOOL(*)(id,SEL,uint64_t,int32_t))originalBulk)(self,cmd,bytes,result);}
    @finally{usbScope=previous;pthread_mutex_lock(&lock);oct_native_receive_end(&journal,scope);pthread_mutex_unlock(&lock);}
}
static id request(id self,SEL cmd,id packet){
    if(authorized()&&usbScope&&packetIs(packet,"PTPOperationRequestPacket")){
        uint16_t op=((uint16_t(*)(id,SEL))requestOp)(packet,sel_registerName("operationCode"));
        uint32_t tid=((uint32_t(*)(id,SEL))requestTID)(packet,sel_registerName("transactionID"));
        pthread_mutex_lock(&lock);oct_native_command(&journal,usbScope,op,tid);pthread_mutex_unlock(&lock);
    }
    return ((id(*)(id,SEL,id))originalRequest)(self,cmd,packet);
}
static BOOL response(id self,SEL cmd,id packet){
    BOOL accepted=((BOOL(*)(id,SEL,id))originalResponse)(self,cmd,packet);
    if(authorized()&&packetIs(packet,"PTPOperationResponsePacket")){
        uint32_t tid=((uint32_t(*)(id,SEL))responseTID)(packet,sel_registerName("transactionID"));
        uint16_t rc=((uint16_t(*)(id,SEL))responseCode)(packet,sel_registerName("responseCode"));
        pthread_mutex_lock(&lock);
        if(oct_native_response(&journal,tokenFor(self),tid,rc,accepted)&&rc==0x2001&&atomic_load(&mode)==1){
            oct_native_snapshot s;oct_native_get_snapshot(&journal,&s);
            uint16_t op=s.event_count?s.events[s.event_count-1].operation:0;
            if(op==0x1004)atomic_fetch_or(&observedStorage,1);
            if(op==0x1005)atomic_fetch_or(&observedStorage,2);
        }
        pthread_mutex_unlock(&lock);
    }
    return accepted;
}
static void data(id self,SEL cmd,id packet){
    if(authorized()&&packetIs(packet,"PTPDataPacket")){
        uint32_t tid=((uint32_t(*)(id,SEL))dataTID)(packet,sel_registerName("transactionID"));
        pthread_mutex_lock(&lock);oct_native_data_submitted(&journal,tokenFor(self),tid,0);pthread_mutex_unlock(&lock);
    }
    ((void(*)(id,SEL,id))originalData)(self,cmd,packet); // async enqueue, NOT transmission success
}
static void connected(id self,SEL cmd,BOOL value){
    ((void(*)(id,SEL,BOOL))originalConnected)(self,cmd,value);
    if(authorized()){pthread_mutex_lock(&lock);oct_native_connect(&journal,tokenFor(self),value);pthread_mutex_unlock(&lock);}
}
static void stop(id self,SEL cmd){
    pthread_mutex_lock(&lock);oct_native_connect(&journal,tokenFor(self),false);pthread_mutex_unlock(&lock);
    ((void(*)(id,SEL))originalStop)(self,cmd);
}
static BOOL install(NSString **reason){
    struct Entry{const char *cls,*sel,*encoding;IMP hook;IMP *old;} entries[]={
        {"PTPDeviceUSBTransport","handleBulkData:result:","B28@0:8Q16i24",(IMP)bulk,&originalBulk},
        {"PTPResponder","handleRequest:","@24@0:8@16",(IMP)request,&originalRequest},
        {"PTPDeviceUSBTransport","sendResponse:","B24@0:8@16",(IMP)response,&originalResponse},
        {"PTPDeviceUSBTransport","sendData:","v24@0:8@16",(IMP)data,&originalData},
        {"PTPDeviceUSBTransport","setConnected:","v20@0:8B16",(IMP)connected,&originalConnected},
        {"PTPDeviceUSBTransport","stop","v16@0:8",(IMP)stop,&originalStop}
    };
    struct Getter{const char *cls,*sel,*encoding;IMP *out;} getters[]={
        {"PTPDeviceUSBTransport","connected","B16@0:8",&transportConnected},
        {"PTPOperationRequestPacket","operationCode","S16@0:8",&requestOp},
        {"PTPOperationRequestPacket","transactionID","I16@0:8",&requestTID},
        {"PTPOperationResponsePacket","responseCode","S16@0:8",&responseCode},
        {"PTPOperationResponsePacket","transactionID","I16@0:8",&responseTID},
        {"PTPDataPacket","transactionID","I16@0:8",&dataTID}
    };
    for(size_t i=0;i<sizeof(entries)/sizeof(entries[0]);i++)if(!OCTRuntimeCheck(entries[i].cls,entries[i].sel,entries[i].encoding,reason))return NO;
    for(size_t i=0;i<sizeof(getters)/sizeof(getters[0]);i++){
        if(!OCTRuntimeCheck(getters[i].cls,getters[i].sel,getters[i].encoding,reason))return NO;
        *getters[i].out=method_getImplementation(class_getInstanceMethod(objc_getClass(getters[i].cls),sel_registerName(getters[i].sel)));
    }
    for(size_t i=0;i<sizeof(entries)/sizeof(entries[0]);i++)if(!OCTRuntimeHook(entries[i].cls,entries[i].sel,entries[i].encoding,entries[i].hook,entries[i].old,reason))return NO;
    return YES;
}
static BOOL compatible(NSString **reason){
    if(strcmp(getprogname(),"ptpd")){*reason=@"Wrong process";return NO;}
    NSOperatingSystemVersion os=NSProcessInfo.processInfo.operatingSystemVersion;
    char build[64]={0};size_t size=sizeof build;
    if(sysctlbyname("kern.osversion",build,&size,NULL,0)==0)kernelBuild=[NSString stringWithUTF8String:build];
    if(os.majorVersion!=17||os.minorVersion!=0||os.patchVersion!=0){*reason=@"Requires iOS 17.0.0";return NO;}
    const struct mach_header_64 *header=(const struct mach_header_64*)_dyld_get_image_header(0);
    oct_macho_identity identity;
    if(!header||header->sizeofcmds>65536||oct_device_parse_identity((const uint8_t*)header,32+(size_t)header->sizeofcmds,&identity)!=OCT_OK||strcmp(identity.uuid,"FFD54FC7-0508-3AB8-8D17-2F698B611E10")||identity.cpu_type!=0x100000c||identity.cpu_subtype!=0x80000002){*reason=@"Loaded ptpd UUID/architecture mismatch";return NO;}
    NSError *error=nil;NSData *bytes=[NSData dataWithContentsOfFile:@"/usr/libexec/ptpd" options:NSDataReadingMappedIfSafe error:&error];
    if(!bytes||bytes.length>8*1024*1024){*reason=@"Cannot verify on-disk ptpd";return NO;}
    uint8_t digest[CC_SHA256_DIGEST_LENGTH];CC_SHA256(bytes.bytes,(CC_LONG)bytes.length,digest);
    NSMutableString *hash=[NSMutableString string];for(unsigned i=0;i<sizeof digest;i++)[hash appendFormat:@"%02x",digest[i]];
    if(![hash isEqualToString:@"a1742358da3e257204a0f190211cda26a155dfe0dabb3c9fa572117460085dcb"]){*reason=@"ptpd hash mismatch";return NO;}
    *reason=@"Exact ptpd UUID, arm64e, SHA-256 and iOS 17.0 matched";return YES;
}
static void publish(void){
    oct_native_snapshot snapshot;NSString *event;uint64_t count;
    pthread_mutex_lock(&lock);oct_native_get_snapshot(&journal,&snapshot);event=lastProviderEvent;count=providerEvents;pthread_mutex_unlock(&lock);
    NSMutableArray *events=[NSMutableArray array];
    for(size_t i=0;i<snapshot.event_count;i++){
        oct_native_event e=snapshot.events[i];
        if(e.sequence<=leaseSequence)continue;
        [events addObject:@{@"sequence":@(e.sequence),@"scope":@(e.scope),@"utc_ms":@(e.utc_ms),@"epoch":@(e.epoch),@"kind":@(e.kind),@"transport":@(e.transport),@"operation":@(e.operation),@"transaction":@(e.transaction),@"response":@(e.response),@"result":@(e.result),@"bytes":@(e.bytes)}];
    }
    NSDictionary *report=@{
        @"schema":@"octopus-native/1",@"module_version":@1,@"pid":@(getpid()),@"uid":@(geteuid()),
        @"generated_at":@([NSDate date].timeIntervalSince1970),@"generation":@(snapshot.generation),
        @"kernel_build":kernelBuild,@"compatibility":compatibility,@"hooks":hookState,@"provider":providerState,
        @"authorized":@(authorized()),@"mode":@(atomic_load(&mode)),@"lease":leaseToken,
        @"expires_at_ms":@(atomic_load(&deadline)),@"authorization_epoch":@(atomic_load(&leaseEpoch)),
        @"provider_eligible":@(atomic_load(&observedStorage)==3),@"observed_storage_flags":@(atomic_load(&observedStorage)),
        @"connected":@(snapshot.connected),@"commands":@(snapshot.commands),@"paired_responses":@(snapshot.paired_responses),
        @"lease_commands":@(snapshot.commands-leaseCommands),@"lease_paired_responses":@(snapshot.paired_responses-leaseResponses),
        @"failed_responses":@(snapshot.failed_responses),@"unmatched_responses":@(snapshot.unmatched_responses),
        @"data_submissions":@(snapshot.data_submissions),@"overwritten_events":@(snapshot.overwritten_events),
        @"pairing_disabled":@(snapshot.pairing_disabled),@"provider_events":@(count),@"lease_provider_events":@(count-leaseProviderEvents),
        @"last_provider_event":count>leaseProviderEvents?event:@"none in current authorization",@"events":events,
        @"claim":@"Native method observations. Successful write result is not proof of host receipt or Explorer download."
    };
    NSData *json=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingSortedKeys error:nil];
    if(json&&json.length<=65536)oct_native_io_write(OCT_NATIVE_DIRECTORY,"status",json.bytes,json.length);
}
__attribute__((constructor)) static void startObserver(void){
    @autoreleasepool {
        if(strcmp(getprogname(),"ptpd"))return;
        oct_native_init(&journal,((uint64_t)arc4random()<<32)|arc4random()|1);
        NSString *reason=nil;BOOL match=compatible(&reason);compatibility=reason?:@"Identity check failed";
        dispatch_queue_t queue=dispatch_queue_create("com.parv.octopus.native",DISPATCH_QUEUE_SERIAL);
        timer=dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,queue);
        dispatch_source_set_timer(timer,dispatch_time(DISPATCH_TIME_NOW,0),NSEC_PER_SEC,NSEC_PER_SEC/10);
        dispatch_source_set_event_handler(timer,^{@autoreleasepool{
            char token[33]={0};uint64_t until=0;unsigned wanted=match?oct_native_read_lease(OCT_NATIVE_DIRECTORY,now(),&until,token):0;
            NSString *current=[NSString stringWithUTF8String:token];
            pthread_mutex_lock(&lock);
            if(![current isEqualToString:leaseToken]||wanted!=atomic_load(&mode)||
               (atomic_load(&mode)>0&&now()>=atomic_load(&deadline))){
                atomic_fetch_add(&leaseEpoch,1);
                oct_native_authorize(&journal,false);
                oct_native_snapshot prior;oct_native_get_snapshot(&journal,&prior);
                leaseSequence=prior.sequence;leaseCommands=prior.commands;leaseResponses=prior.paired_responses;leaseProviderEvents=providerEvents;
            }
            leaseToken=current;
            pthread_mutex_unlock(&lock);
            atomic_store(&deadline,until);atomic_store(&mode,wanted);
            if(wanted&&!attemptedHooks){attemptedHooks=true;NSString *reason=nil;BOOL ok=install(&reason);hookState=ok?@"Installed":reason;atomic_store(&ready,ok);}
            if(wanted&&atomic_load(&ready)&&!attemptedProvider){
                NSString *problem=nil;
                BOOL valid=OCTProviderValidate(&problem);
                providerState=valid?@"ABI matched; awaiting observer-only storage requests":(problem?:@"Provider ABI mismatch");
                if(wanted==2&&valid&&atomic_load(&observedStorage)==3){
                    attemptedProvider=true;BOOL ok=OCTProviderInstall(&problem);
                    providerState=ok?@"Installed (synthetic, experimental)":(problem?:@"Provider installation failed");
                    atomic_store(&providerReady,ok);
                }
            }
            pthread_mutex_lock(&lock);oct_native_authorize(&journal,authorized());pthread_mutex_unlock(&lock);
            publish();
        }});
        dispatch_resume(timer);
    }
}

#ifdef OCT_NATIVE_TEST
BOOL OCTObserverTestInstall(NSString **reason){
    oct_native_init(&journal,1);BOOL ok=install(reason);atomic_store(&ready,ok);return ok;
}
void OCTObserverTestAuthorize(unsigned wanted,uint64_t until){
    atomic_fetch_add(&leaseEpoch,1);atomic_store(&deadline,until);atomic_store(&mode,wanted);
    pthread_mutex_lock(&lock);oct_native_authorize(&journal,authorized());pthread_mutex_unlock(&lock);
}
void OCTObserverTestSnapshot(oct_native_snapshot *out){pthread_mutex_lock(&lock);oct_native_get_snapshot(&journal,out);pthread_mutex_unlock(&lock);}
uint64_t OCTObserverTestNow(void){return now();}
unsigned OCTObserverTestStorageFlags(void){return atomic_load(&observedStorage);}
#endif

#import "OctopusRuntime.h"
#import <mach-o/dyld.h>
#import <CommonCrypto/CommonDigest.h>
#import <dispatch/dispatch.h>
#include "../Core/oct_device.h"
#include "../Core/oct_link.h"
#include "../Core/oct_native.h"
#include "../Core/oct_native_io.h"
#include <stdatomic.h>
#include <pthread.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <os/log.h>

/*
 * Startup evidence that does not depend on the status file. On the v0.4.20 device run the status
 * file never appeared, and nothing could say whether this dylib was loaded at all, rejected the
 * process, or ran and was refused by ptpd's "ptpd" seatbelt profile when it touched the app's
 * folder. The same uid does not imply the same sandbox rights.
 *
 * Two channels, neither of which needs the filesystem:
 *  - os_log, under subsystem com.parv.octopus.native, readable from Windows with idevicesyslog.
 *  - a 64-bit Darwin notification state (OCT_NATIVE_SIGNAL_NAME), which the app reads and shows
 *    in Run Full Diagnostics: how far startup got, which identity check failed, and the exact stage
 *    and errno of the last lease read and status write.
 * Both report stage names and errno numbers only: no paths beyond our own, no lease token, no data.
 * Updated only on the constructor thread before the timer starts, then only on the timer's serial
 * queue, so the struct needs no lock.
 */
static oct_native_signal startup={.lease_stage=OCT_NATIVE_IO_NOT_ATTEMPTED,
                                  .status_stage=OCT_NATIVE_IO_NOT_ATTEMPTED};
static oct_native_detail startupDetail={.image_index=255};

/*
 * The process's own loaded images, behind the interface oct_device_select_image takes, so the
 * device uses dyld and the tests use a list they build. ptpd carries Dopamine's systemhook
 * bootstrap and, under ElleKit, this dylib, so the executable is not assumed to come first: the
 * 9 November run rejected a ptpd whose bytes are byte-identical to the reference, which is what
 * reading the wrong image's header would look like.
 */
static uint32_t dyldCount(void *context){(void)context;return _dyld_image_count();}
static const char *dyldName(void *context,uint32_t index){(void)context;return _dyld_get_image_name(index);}
static const void *dyldHeader(void *context,uint32_t index){(void)context;return _dyld_get_image_header(index);}
static os_log_t startupLog;
static os_log_t startup_log(void){return startupLog?startupLog:OS_LOG_DEFAULT;}
static void raise_phase(uint8_t phase){if(phase>startup.phase)startup.phase=phase;}
static uint8_t clamp_errno(int error){return error<0?0:(error>255?255:(uint8_t)error);}
static void post_startup(void){
    startup.pid=(uint16_t)(getpid()&0xFFFF);
    oct_status posted=oct_native_signal_publish(&startup);
    oct_native_detail_publish(&startupDetail);
    static bool reportedPostFailure;
    if(posted!=OCT_OK&&!reportedPostFailure){reportedPostFailure=true;
        os_log_error(startup_log(),"startup signal could not be posted: %{public}s",oct_status_name(posted));}
}

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static oct_native_journal journal;
static _Atomic(uint64_t) deadline;
static _Atomic(unsigned) mode;
static _Atomic(bool) ready;
static _Atomic(bool) providerReady;
/* Whether the provider *could* be installed: its ABI checks passed. Distinct from providerReady,
 * which means it *is* installed. The app's synthetic-storage toggle waits on eligibility, and
 * installation needs the toggle, so reporting "installed" as eligibility deadlocked the two: the
 * toggle could never become tappable, which is exactly what the 10 November run showed. */
static _Atomic(bool) providerEligible;
static _Atomic(unsigned) observedStorage;
static _Atomic(uint64_t) leaseEpoch;
static __thread uint64_t usbScope;
static __thread uint16_t currentOperation;
static __thread uint32_t currentTransaction;
static IMP originalBulk,originalRequest,originalResponse,originalData,originalConnected,originalStop;
static IMP requestOp,requestTID,responseCode,responseTID,dataTID,transportConnected;
static IMP requestParameterCount,requestParameter;
static void *transportPointer;
static uint64_t transportToken;
static NSString *compatibility=@"Not checked", *hookState=@"Not installed", *providerState=@"Not installed";
static NSString *leaseToken=@"",*kernelBuild=@"unknown",*lastProviderEvent=@"none";
static uint64_t providerEvents;
static NSMutableArray *providerJournal;
static NSMutableArray *requestJournal;
static uint64_t leaseSequence,leaseCommands,leaseResponses,leaseProviderEvents;
static bool attemptedHooks,attemptedProvider;
static dispatch_source_t timer;
static uint64_t now(void){int64_t n=oct_link_now_ms();return n>0?(uint64_t)n:0;}
static bool authorized(void){uint64_t current=now();return current>0&&atomic_load(&ready)&&atomic_load(&mode)>0&&current<atomic_load(&deadline);}
/* Exposure needs a current lease, the user's explicit mode-2 grant, and installed provider hooks.
 * It used to also require having seen a GetStorageIDs and a GetStorageInfo answered, as evidence a
 * real host was talking. That interlock could not be satisfied: those are recorded from the
 * response path, which until this commit ran only in mode 1, and a host asks for storages before
 * hooks exist anyway. The flags are still recorded and reported; they no longer gate. */
bool OCTNativeProviderEnabled(void){return authorized()&&atomic_load(&mode)==2&&atomic_load(&providerReady);}
uint64_t OCTNativeAuthorizationEpoch(void){return atomic_load(&leaseEpoch);}
/*
 * Every provider hook already reports a named event here, so the per-hook summary is derived from
 * those names rather than by touching fourteen call sites. The names are compile-time literals in
 * OctopusProvider.m; an unrecognised one simply sets no bit.
 */
static oct_native_hook_report hookReport;
static void recordHook(const char *event){
    struct { const char *event; unsigned bit; } map[] = {
        {"provider-device-info",OCT_NATIVE_HOOK_DEVICE_INFO},
        {"provider-storage-enumeration",OCT_NATIVE_HOOK_STORAGE_IDS},
        {"provider-storage-info",OCT_NATIVE_HOOK_STORAGE_INFO},
        {"provider-object-info",OCT_NATIVE_HOOK_OBJECT_INFO},
        {"provider-object-enumeration",OCT_NATIVE_HOOK_HANDLES},
        {"provider-bulk-object-info",OCT_NATIVE_HOOK_BULK_INFO},
        {"provider-download-source",OCT_NATIVE_HOOK_DATA_SOURCE},
    };
    for(size_t i=0;i<sizeof map/sizeof map[0];i++){
        if(!strcmp(event,map[i].event)){hookReport.fired|=(uint16_t)map[i].bit;break;}
    }
    /* The four that decide whether a listing appears, counted rather than just flagged. */
    if(!strcmp(event,"provider-device-info")){
        hookReport.formats_added=true;
        if(hookReport.device_info<255)hookReport.device_info++;
    } else if(!strcmp(event,"provider-object-enumeration")){
        if(hookReport.handles<255)hookReport.handles++;
    } else if(!strcmp(event,"provider-object-info")||!strcmp(event,"provider-bulk-object-info")){
        if(hookReport.object_info<255)hookReport.object_info++;
    } else if(!strcmp(event,"provider-read")){
        if(hookReport.get_object<255)hookReport.get_object++;
    }
}

void OCTNativeProviderEvent(const char *event,uint32_t handle,int32_t result){
    pthread_mutex_lock(&lock);
    recordHook(event);
    providerEvents++;lastProviderEvent=[NSString stringWithFormat:@"%s handle=%u result=%d",event,handle,result];
    if(!providerJournal)providerJournal=[NSMutableArray array];
    if(providerJournal.count==64)[providerJournal removeObjectAtIndex:0];
    [providerJournal addObject:@{@"sequence":@(providerEvents),@"event":[NSString stringWithUTF8String:event],
        @"utc_ms":@((int64_t)([NSDate date].timeIntervalSince1970*1000)),@"authorization_epoch":@(atomic_load(&leaseEpoch)),
        @"usb_scope":@(usbScope),@"operation":@(currentOperation),@"transaction":@(currentTransaction),
        @"object_or_storage":@(handle),@"result":@(result)}];
    pthread_mutex_unlock(&lock);
}
static uint64_t tokenFor(id transport){
    void *p=(__bridge void*)transport;
    if(transportPointer!=p){transportPointer=p;transportToken++;}
    return transportToken;
}
static bool packetIs(id packet,const char *name){return packet&&object_getClass(packet)==objc_getClass(name);}
static bool bulk(id self,SEL cmd,uint64_t bytes,int32_t result){
    uint64_t previous=usbScope,scope=0;
    if(authorized()){
        // Read the verified native connection property only during a real bulk
        // callback. This also supports enabling after Apple's connection setup.
        bool isConnected=((bool(*)(id,SEL))transportConnected)(self,sel_registerName("connected"));
        pthread_mutex_lock(&lock);
        oct_native_connect(&journal,tokenFor(self),isConnected);
        scope=oct_native_receive_begin(&journal,tokenFor(self),result,bytes>UINT32_MAX?UINT32_MAX:(uint32_t)bytes);
        pthread_mutex_unlock(&lock);
    }
    usbScope=scope;
    @try{return ((bool(*)(id,SEL,uint64_t,int32_t))originalBulk)(self,cmd,bytes,result);}
    @finally{usbScope=previous;pthread_mutex_lock(&lock);oct_native_receive_end(&journal,scope);pthread_mutex_unlock(&lock);}
}
static id request(id self,SEL cmd,id packet){
    uint16_t previousOp=currentOperation;uint32_t previousTransaction=currentTransaction;
    currentOperation=0;currentTransaction=0;
    if(authorized()&&usbScope&&packetIs(packet,"PTPOperationRequestPacket")){
        uint16_t op=((uint16_t(*)(id,SEL))requestOp)(packet,sel_registerName("operationCode"));
        uint32_t tid=((uint32_t(*)(id,SEL))requestTID)(packet,sel_registerName("transactionID"));
        pthread_mutex_lock(&lock);
        if(oct_native_command(&journal,usbScope,op,tid)){
            currentOperation=op;currentTransaction=tid;
            int32_t count=((int32_t(*)(id,SEL))requestParameterCount)(packet,sel_registerName("numParameters"));
            NSMutableArray *parameters=[NSMutableArray array];
            // The supplied binary's parameterAtIndex: returns only indices 0..3.
            // Never report its out-of-range zero as a genuine fifth parameter.
            if(count>=0&&count<=5)for(uint32_t i=0;i<(uint32_t)count&&i<4;i++){
                uint32_t value=((uint32_t(*)(id,SEL,uint32_t))requestParameter)(packet,sel_registerName("parameterAtIndex:"),i);
                [parameters addObject:@(value)];
            }
            if(!requestJournal)requestJournal=[NSMutableArray array];
            if(requestJournal.count==64)[requestJournal removeObjectAtIndex:0];
            [requestJournal addObject:@{@"usb_scope":@(usbScope),@"operation":@(op),@"transaction":@(tid),
                @"utc_ms":@((int64_t)([NSDate date].timeIntervalSince1970*1000)),@"parameters":parameters,
                @"parameter_count":@(count),@"parameters_truncated":@(count<0||count>4)}];
        }
        pthread_mutex_unlock(&lock);
    }
    @try{return ((id(*)(id,SEL,id))originalRequest)(self,cmd,packet);}
    @finally{currentOperation=previousOp;currentTransaction=previousTransaction;}
}
static bool response(id self,SEL cmd,id packet){
    bool accepted=((bool(*)(id,SEL,id))originalResponse)(self,cmd,packet);
    if(authorized()&&packetIs(packet,"PTPOperationResponsePacket")){
        uint32_t tid=((uint32_t(*)(id,SEL))responseTID)(packet,sel_registerName("transactionID"));
        uint16_t rc=((uint16_t(*)(id,SEL))responseCode)(packet,sel_registerName("responseCode"));
        pthread_mutex_lock(&lock);
        /* Recorded in any authorized mode. Gating this on mode 1 meant that turning the
         * synthetic toggle on, which moves to mode 2, stopped these from ever being seen: the
         * 10 November run observed 121 commands and still reported no storage requests. */
        if(oct_native_response(&journal,tokenFor(self),tid,rc,accepted)&&rc==0x2001){
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
static void connected(id self,SEL cmd,bool value){
    ((void(*)(id,SEL,bool))originalConnected)(self,cmd,value);
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
        {"PTPOperationRequestPacket","numParameters","i16@0:8",&requestParameterCount},
        {"PTPOperationRequestPacket","parameterAtIndex:","I20@0:8I16",&requestParameter},
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
    if(strcmp(getprogname(),"ptpd")){startup.identity=OCT_NATIVE_IDENTITY_WRONG_PROCESS;*reason=@"Wrong process";return NO;}
    NSOperatingSystemVersion os=NSProcessInfo.processInfo.operatingSystemVersion;
    char build[64]={0};size_t size=sizeof build;
    if(sysctlbyname("kern.osversion",build,&size,NULL,0)==0)kernelBuild=[NSString stringWithUTF8String:build];
    if(os.majorVersion!=17||os.minorVersion!=0||os.patchVersion!=0){startup.identity=OCT_NATIVE_IDENTITY_WRONG_SYSTEM;*reason=@"Requires iOS 17.0.0";return NO;}
    oct_image_source images={dyldCount,dyldName,dyldHeader,NULL};
    uint32_t imageIndex=UINT32_MAX;oct_macho_identity found={0};
    oct_image_outcome outcome=oct_device_select_image(&images,"/usr/libexec/ptpd",&imageIndex,&found);
    startupDetail.image_count=(uint8_t)(_dyld_image_count()>255?255:_dyld_image_count());
    startupDetail.image_index=(uint8_t)(imageIndex>254?255:imageIndex);
    /* Six bytes of whatever UUID was read, so a wrong image names itself next run. */
    if(found.uuid[0]){
        for(unsigned i=0;i<6;i++){
            char pair[3]={found.uuid[i*2+(i>=4?1:0)],found.uuid[i*2+1+(i>=4?1:0)],0};
            startupDetail.uuid_prefix[i]=(uint8_t)strtoul(pair,NULL,16);
        }
    }
    if(outcome!=OCT_IMAGE_FOUND){
        switch(outcome){
        case OCT_IMAGE_NOT_PRESENT: startup.identity=OCT_NATIVE_IDENTITY_NO_IMAGE; break;
        case OCT_IMAGE_AMBIGUOUS: startup.identity=OCT_NATIVE_IDENTITY_AMBIGUOUS_IMAGE; break;
        case OCT_IMAGE_NO_HEADER: startup.identity=OCT_NATIVE_IDENTITY_NO_HEADER; break;
        case OCT_IMAGE_NOT_MACHO: case OCT_IMAGE_HEADER_TOO_LARGE:
            startup.identity=OCT_NATIVE_IDENTITY_BAD_HEADER; break;
        case OCT_IMAGE_MALFORMED: startup.identity=OCT_NATIVE_IDENTITY_MALFORMED_HEADER; break;
        case OCT_IMAGE_NOT_EXECUTABLE: startup.identity=OCT_NATIVE_IDENTITY_NOT_EXECUTABLE; break;
        default: startup.identity=OCT_NATIVE_IDENTITY_NO_IMAGE; break;
        }
        os_log_error(startup_log(),"ptpd image not usable: %{public}s (index %u of %u)",
                     oct_image_outcome_name(outcome),imageIndex,_dyld_image_count());
        *reason=[NSString stringWithFormat:@"ptpd image not usable: %s",oct_image_outcome_name(outcome)];
        return NO;
    }
    if(strcmp(found.uuid,"FFD54FC7-0508-3AB8-8D17-2F698B611E10")){
        startup.identity=OCT_NATIVE_IDENTITY_WRONG_UUID;
        os_log_error(startup_log(),"ptpd UUID is %{public}s, expected FFD54FC7-0508-3AB8-8D17-2F698B611E10",found.uuid);
        *reason=[NSString stringWithFormat:@"ptpd UUID is %s",found.uuid];return NO;
    }
    if(found.cpu_type!=0x100000c||found.cpu_subtype!=0x80000002){
        startup.identity=OCT_NATIVE_IDENTITY_WRONG_ARCHITECTURE;
        *reason=[NSString stringWithFormat:@"ptpd is cpu 0x%x/0x%x, expected arm64e",found.cpu_type,found.cpu_subtype];
        return NO;
    }
    NSError *error=nil;NSData *bytes=[NSData dataWithContentsOfFile:@"/usr/libexec/ptpd" options:NSDataReadingMappedIfSafe error:&error];
    if(!bytes||bytes.length>8*1024*1024){startup.identity=OCT_NATIVE_IDENTITY_UNREADABLE_FILE;*reason=@"Cannot read /usr/libexec/ptpd to check its digest";return NO;}
    uint8_t digest[CC_SHA256_DIGEST_LENGTH];CC_SHA256(bytes.bytes,(CC_LONG)bytes.length,digest);
    NSMutableString *hash=[NSMutableString string];for(unsigned i=0;i<sizeof digest;i++)[hash appendFormat:@"%02x",digest[i]];
    if(![hash isEqualToString:@"a1742358da3e257204a0f190211cda26a155dfe0dabb3c9fa572117460085dcb"]){startup.identity=OCT_NATIVE_IDENTITY_WRONG_DIGEST;*reason=@"ptpd hash mismatch";return NO;}
    startup.identity=OCT_NATIVE_IDENTITY_COMPATIBLE;*reason=@"Exact ptpd UUID, arm64e, SHA-256 and iOS 17.0 matched";return YES;
}
static void publish(void){
    oct_native_snapshot snapshot;NSString *event;uint64_t count;NSArray *providerRecords,*requestRecords;
    pthread_mutex_lock(&lock);oct_native_get_snapshot(&journal,&snapshot);event=lastProviderEvent;count=providerEvents;
    providerRecords=[providerJournal copy]?:@[];requestRecords=[requestJournal copy]?:@[];pthread_mutex_unlock(&lock);
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
        @"provider_eligible":@(atomic_load(&providerEligible)),@"observed_storage_flags":@(atomic_load(&observedStorage)),
        @"connected":@(snapshot.connected),@"commands":@(snapshot.commands),@"paired_responses":@(snapshot.paired_responses),
        @"lease_commands":@(snapshot.commands-leaseCommands),@"lease_paired_responses":@(snapshot.paired_responses-leaseResponses),
        @"failed_responses":@(snapshot.failed_responses),@"unmatched_responses":@(snapshot.unmatched_responses),
        @"data_submissions":@(snapshot.data_submissions),@"overwritten_events":@(snapshot.overwritten_events),
        @"pairing_disabled":@(snapshot.pairing_disabled),@"provider_events":@(count),@"lease_provider_events":@(count-leaseProviderEvents),
        @"last_provider_event":count>leaseProviderEvents?event:@"none in current authorization",@"events":events,
        @"provider_journal":providerRecords,
        @"request_journal":requestRecords,
        @"claim":@"Native method observations. Successful write result is not proof of host receipt or Explorer download."
    };
    NSData *json=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingSortedKeys error:nil];
    if(!json||json.length>65536)return;
    /*
     * The shared folder is refused inside ptpd's sandbox: the 9 November run measured EPERM when
     * opening it, from both ptpd processes, with the folder owned by the same uid ptpd runs as.
     * Retrying once a second for the life of the process only fills the system log with sandbox
     * denials, so a refusal is recorded once and then retried rarely, in case the situation
     * changes (the app recreating the folder, a different build of the companion). The startup
     * signal carries the stage and errno regardless, so nothing is lost by not retrying.
     */
    static unsigned refusals;static uint64_t nextAttemptMs;
    if(refusals&&now()<nextAttemptMs)return;
    oct_native_io_failure failure;
    oct_native_io_write_detailed(OCT_NATIVE_DIRECTORY,"status",json.bytes,json.length,&failure);
    bool refused=failure.stage!=OCT_NATIVE_IO_OK&&(failure.error==EPERM||failure.error==EACCES);
    if(refused){
        if(!refusals)os_log_error(startup_log(),"the shared folder is refused in this process; backing off");
        if(refusals<16)refusals++;
        /* 30 s, then a minute, up to roughly eight minutes. */
        nextAttemptMs=now()+(uint64_t)30000*(refusals<5?(1u<<(refusals-1)):16u);
    } else {
        refusals=0;nextAttemptMs=0;
    }
    if(failure.stage!=startup.status_stage||clamp_errno(failure.error)!=startup.status_error){
        if(failure.stage==OCT_NATIVE_IO_OK)os_log(startup_log(),"status written");
        else os_log_error(startup_log(),"status write failed at %{public}s, errno %d",oct_native_io_stage_name(failure.stage),failure.error);
    }
    startup.status_stage=(uint8_t)failure.stage;startup.status_error=clamp_errno(failure.error);
    if(failure.stage==OCT_NATIVE_IO_OK)raise_phase(OCT_NATIVE_PHASE_PUBLISHED);
}
/*
 * One authorization pass: read the lease, install the observer hooks and the provider, publish.
 * Run once synchronously from the constructor and then every second from the timer.
 *
 * Synchronously first because the timer's handler runs on its own queue, after the constructor
 * returns, concurrently with ptpd's own startup. A host asks GetDeviceInfo as the first command of
 * a session, and that answer carries the list of object formats the host will accept; Windows hid
 * every text object on 10 November because the hook was not yet in place to add format 0x3004 to
 * it. Doing a pass before dyld returns control puts the hooks in before ptpd can open its USB
 * session at all, which is the only ordering that beats the first command rather than racing it.
 */
static void pollOnce(BOOL match)
{
    @autoreleasepool {
            char token[33]={0};uint64_t until=0;unsigned wanted=0;
            if(match){
                /*
                 * Authorization arrives by notification, not by file: the shared folder is refused
                 * in this process. Whatever the word asks for, only observation is granted, because
                 * any process on the device can post to this name; see oct_native_lease_effective.
                 * The file is still read when it is readable, so a build on a phone where the
                 * folder works keeps the stronger channel, and a lease from either source must be
                 * current for hooks to stay installed.
                 */
                oct_native_lease_word granted;
                oct_native_lease_mode effective=OCT_NATIVE_LEASE_NONE;
                oct_status leaseStatus=oct_native_lease_read(&granted);
                if(leaseStatus==OCT_OK){
                    effective=oct_native_lease_effective(&granted,now());
                    if(granted.mode==OCT_NATIVE_LEASE_EXPOSE_FILESYSTEM&&effective==OCT_NATIVE_LEASE_NONE){
                        static bool reportedRefusal;
                        if(!reportedRefusal){reportedRefusal=true;
                            os_log_error(startup_log(),"real-file exposure was requested over the notification channel and refused; it grants nothing");}
                    }
                }
                startup.lease_stage=(uint8_t)(leaseStatus==OCT_OK?OCT_NATIVE_IO_OK:
                                              (leaseStatus==OCT_E_STATE?OCT_NATIVE_IO_CONTENT:OCT_NATIVE_IO_READ));
                startup.lease_error=0;
                /* Mode 1 observes; mode 2 additionally serves the synthetic tree, which is what
                 * OCTNativeProviderEnabled requires. Real files are never reachable from here. */
                /* Mode 1 observes; mode 2 additionally serves a storage. Which store that is comes
                 * from the scope the grant names: the synthetic tree, or a read-only view of one
                 * of the fixed roots in oct_native_lease_root. */
                wanted=effective==OCT_NATIVE_LEASE_OBSERVE?1u:
                       (effective==OCT_NATIVE_LEASE_NONE?0u:2u);
                if(wanted==2){
                    NSString *scopeProblem=nil;
                    if(!OCTProviderSetScope(effective,&scopeProblem)&&scopeProblem){
                        static NSString *lastScopeProblem;
                        if(![scopeProblem isEqualToString:lastScopeProblem]){
                            lastScopeProblem=scopeProblem;
                            os_log_error(startup_log(),"scope not served: %{public}s",scopeProblem.UTF8String);
                        }
                    }
                }
                if(wanted){
                    until=granted.deadline_ms;
                    snprintf(token,sizeof token,"notify-%u",(unsigned)granted.generation);
                    raise_phase(OCT_NATIVE_PHASE_LEASE);
                }
            }
            NSString *current=[NSString stringWithUTF8String:token];
            pthread_mutex_lock(&lock);
            if(![current isEqualToString:leaseToken]||wanted!=atomic_load(&mode)||
               (atomic_load(&mode)>0&&now()>=atomic_load(&deadline))){
                atomic_fetch_add(&leaseEpoch,1);
                oct_native_authorize(&journal,false);
                oct_native_snapshot prior;oct_native_get_snapshot(&journal,&prior);
                leaseSequence=prior.sequence;leaseCommands=prior.commands;leaseResponses=prior.paired_responses;leaseProviderEvents=providerEvents;
                [providerJournal removeAllObjects];
                [requestJournal removeAllObjects];
            }
            leaseToken=current;
            pthread_mutex_unlock(&lock);
            atomic_store(&deadline,until);atomic_store(&mode,wanted);
            if(wanted&&!attemptedHooks){attemptedHooks=true;NSString *reason=nil;BOOL ok=install(&reason);hookState=ok?@"Installed":reason;atomic_store(&ready,ok);
                startup.hooks=ok?OCT_NATIVE_HOOKS_INSTALLED:OCT_NATIVE_HOOKS_REFUSED;if(ok)raise_phase(OCT_NATIVE_PHASE_HOOKED);
                os_log(startup_log(),"observer hooks %{public}s",ok?"installed":"refused");}
            /* Not gated on the observer hooks or on a lease: the provider's hooks forward to Apple
         * until OCTNativeProviderEnabled() says otherwise, and that still requires installed
         * observer hooks, a current lease and the user's mode-2 grant. Installing them the moment
         * identity is verified is the earliest possible point, which is what GetDeviceInfo needs. */
        if(match&&!attemptedProvider){
                NSString *problem=nil;
                BOOL valid=OCTProviderValidate(&problem);
                atomic_store(&providerEligible,valid);
                providerState=valid?@"ABI matched; awaiting observer-only storage requests":(problem?:@"Provider ABI mismatch");
                /*
                 * Installed as soon as the ABI validates, not when exposure is first wanted. Every
                 * provider hook asks OCTNativeProviderEnabled() on each call and forwards to
                 * Apple's own implementation when it says no, so installing early changes nothing
                 * until the user authorizes exposure.
                 *
                 * Waiting cost the 10 November run its result. A host asks for storages once, at
                 * the start of a session, and ptpd is started by launchd when the cable goes in,
                 * so by the time a lease has been read and hooks installed the enumeration has
                 * already happened. Installing on the first poll means the hook is in place before
                 * the next GetStorageIDs, which is what makes a reconnect show the volume.
                 */
                if(valid){
                    attemptedProvider=true;BOOL ok=OCTProviderInstall(&problem);
                    providerState=ok?@"Installed (synthetic, experimental)":(problem?:@"Provider installation failed");
                    atomic_store(&providerReady,ok);
                }
            }
            pthread_mutex_lock(&lock);oct_native_authorize(&journal,authorized());pthread_mutex_unlock(&lock);
            publish();
            /* The counters also go out by notification, because the folder publish() writes into
             * is refused in this process. This is what the app reads to decide whether the
             * synthetic-storage toggle may be offered. */
            {
                oct_native_snapshot live;oct_native_get_snapshot(&journal,&live);
                oct_native_runtime runtime={0};
                runtime.mode=(uint8_t)atomic_load(&mode);
                if(atomic_load(&ready))runtime.flags|=OCT_NATIVE_RUNTIME_HOOKED;
                /* Eligible means the provider's own checks passed, not that it is already
                 * serving; the app needs that to offer the toggle at all. */
                if(atomic_load(&providerEligible))runtime.flags|=OCT_NATIVE_RUNTIME_ELIGIBLE;
                if(atomic_load(&providerReady))runtime.flags|=OCT_NATIVE_RUNTIME_SERVING;
                unsigned seen=atomic_load(&observedStorage);
                if(seen&1)runtime.flags|=OCT_NATIVE_RUNTIME_SAW_IDS;
                if(seen&2)runtime.flags|=OCT_NATIVE_RUNTIME_SAW_INFO;
                /* Counts saturate rather than wrap, so a long session cannot read as a quiet one. */
                runtime.commands=(uint16_t)(live.commands>0xFFFFu?0xFFFFu:live.commands);
                runtime.responses=(uint16_t)(live.paired_responses>0xFFFFu?0xFFFFu:live.paired_responses);
                runtime.provider_events=(uint8_t)(providerEvents>0xFFu?0xFFu:providerEvents);
                oct_native_runtime_publish(&runtime);
                pthread_mutex_lock(&lock);
                oct_native_hook_report hooks=hookReport;
                pthread_mutex_unlock(&lock);
                oct_native_hooks_publish(&hooks);
            }
            post_startup();
    }
}

__attribute__((constructor)) static void startObserver(void){
    @autoreleasepool {
        if(strcmp(getprogname(),"ptpd"))return;
        startupLog=os_log_create("com.parv.octopus.native","startup");
        /* The first thing done in ptpd, before anything that can fail: proof the dylib was loaded. */
        os_log(startup_log(),"constructor entered in ptpd pid %d uid %d",(int)getpid(),(int)geteuid());
        raise_phase(OCT_NATIVE_PHASE_CONSTRUCTOR);post_startup();
        oct_native_init(&journal,((uint64_t)arc4random()<<32)|arc4random()|1);
        NSString *reason=nil;BOOL match=compatible(&reason);compatibility=reason?:@"Identity check failed";
        raise_phase(OCT_NATIVE_PHASE_IDENTITY);
        os_log(startup_log(),"identity %{public}s: %{public}s",match?"compatible":"rejected",compatibility.UTF8String);
        post_startup();
        dispatch_queue_t queue=dispatch_queue_create("com.parv.octopus.native",DISPATCH_QUEUE_SERIAL);
        timer=dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,queue);
        dispatch_source_set_timer(timer,dispatch_time(DISPATCH_TIME_NOW,0),NSEC_PER_SEC,NSEC_PER_SEC/10);
        dispatch_source_set_event_handler(timer,^{pollOnce(match);});
        /* One pass now, on this thread, before ptpd can open a USB session. */
        pollOnce(match);
        /* Before dispatch_resume: once resumed, the handler runs on its own queue and owns `startup`. */
        raise_phase(OCT_NATIVE_PHASE_TIMER);post_startup();
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

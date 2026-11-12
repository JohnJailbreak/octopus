/* LOCAL test of real hook wrappers around precisely typed native stand-ins.
 * This does not exercise IOKit, USB endpoints, Photos, injection or File Explorer. */
#import <Foundation/Foundation.h>
#import "../OctopusRuntime.h"
#include "../../Core/oct_native.h"
#include <assert.h>
#include <stdio.h>

extern BOOL OCTObserverTestInstall(NSString **reason);
extern void OCTObserverTestAuthorize(unsigned wanted,uint64_t until);
extern void OCTObserverTestSnapshot(oct_native_snapshot *out);
extern uint64_t OCTObserverTestNow(void);
extern unsigned OCTObserverTestStorageFlags(void);

BOOL OCTProviderInstall(oct_native_lease_mode scope, NSString **reason) {
    (void)scope;   /* the observer build does not link the provider; see OCTProviderSetScope below */
    if(reason)*reason=@"Observer-only fixture";
    return YES;
}
BOOL OCTProviderValidate(NSString **reason) { if(reason)*reason=@"Observer-only fixture";return YES; }
/* The observer selects a scope once a lease grants exposure. This build does not link the provider,
 * so this stands in for it, as the two stubs above do; the scope is accepted and not acted on. */
bool OCTProviderNeedsAnnounce(void) { return false; }   /* the provider is not linked here */
BOOL OCTProviderSetScope(oct_native_lease_mode scope, NSString **reason) {
    (void)scope;if(reason)*reason=nil;return YES;
}

@interface PTPOperationRequestPacket : NSObject
@property(nonatomic) uint16_t operationCode;
@property(nonatomic) uint32_t transactionID;
- (int32_t)numParameters;
- (uint32_t)parameterAtIndex:(uint32_t)index;
@end
@implementation PTPOperationRequestPacket
- (int32_t)numParameters { return 1; }
- (uint32_t)parameterAtIndex:(uint32_t)index { return index==0?0x10001:0; }
@end
@interface PTPOperationResponsePacket : NSObject
@property(nonatomic) uint16_t responseCode;
@property(nonatomic) uint32_t transactionID;
@end
@implementation PTPOperationResponsePacket @end
@interface PTPDataPacket : NSObject
@property(nonatomic) uint32_t transactionID;
@end
@implementation PTPDataPacket @end

@interface PTPResponder : NSObject
@property(nonatomic) NSUInteger calls;
@property(nonatomic,strong) id answer;
- (id)handleRequest:(id)request;
@end
@implementation PTPResponder
- (id)handleRequest:(id)request { (void)request;self.calls++;return self.answer; }
@end

@interface PTPDeviceUSBTransport : NSObject
@property(nonatomic) bool connected;
@property(nonatomic) bool acceptResponse;
@property(nonatomic) bool throwOnBulk;
@property(nonatomic) NSUInteger bulkCalls;
@property(nonatomic) NSUInteger dataCalls;
@property(nonatomic) NSUInteger responseCalls;
@property(nonatomic,strong) PTPResponder *responder;
@property(nonatomic,strong) PTPOperationRequestPacket *packet;
- (bool)handleBulkData:(uint64_t)bytes result:(int32_t)result;
- (bool)sendResponse:(id)packet;
- (void)sendData:(id)packet;
- (void)stop;
@end
@implementation PTPDeviceUSBTransport
- (bool)handleBulkData:(uint64_t)bytes result:(int32_t)result {
    (void)bytes;(void)result;self.bulkCalls++;
    if(self.throwOnBulk)@throw [NSException exceptionWithName:@"Fixture" reason:@"scope cleanup" userInfo:nil];
    if(self.packet){id answer=[self.responder handleRequest:self.packet];assert(answer==self.responder.answer);}
    return self.connected;
}
- (bool)sendResponse:(id)packet { (void)packet;self.responseCalls++;return self.acceptResponse; }
- (void)sendData:(id)packet { (void)packet;self.dataCalls++; }
- (void)stop { self.connected=false; }
@end

static oct_native_snapshot snapshot(void){oct_native_snapshot value;OCTObserverTestSnapshot(&value);return value;}
int main(void){@autoreleasepool{
    NSString *reason=nil;assert(OCTObserverTestInstall(&reason));
    PTPDeviceUSBTransport *usb=[PTPDeviceUSBTransport new];usb.responder=[PTPResponder new];usb.responder.answer=@NO;
    usb.acceptResponse=true;usb.connected=true;
    PTPOperationRequestPacket *command=[PTPOperationRequestPacket new];command.operationCode=0x1004;command.transactionID=1;usb.packet=command;
    /* No authorization: originals continue, no request recorded. */
    assert([usb handleBulkData:12 result:0]);assert(usb.bulkCalls==1&&usb.responder.calls==1);assert(snapshot().commands==0);
    OCTObserverTestAuthorize(1,OCTObserverTestNow()+15000);
    /* Hook installed after connection: bulk wrapper samples connected getter. */
    assert([usb handleBulkData:12 result:0]);assert(snapshot().commands==1&&snapshot().connected);
    PTPOperationResponsePacket *response=[PTPOperationResponsePacket new];response.transactionID=1;response.responseCode=0x2001;
    assert([usb sendResponse:response]);assert(snapshot().paired_responses==1);
    assert(OCTObserverTestStorageFlags()==1);
    /* An arbitrary native method invocation cannot create USB provenance. */
    command.transactionID=2;[usb.responder handleRequest:command];assert(snapshot().commands==1);
    response.transactionID=2;assert([usb sendResponse:response]);assert(snapshot().paired_responses==1);
    command.transactionID=3;assert([usb handleBulkData:12 result:0]);
    PTPDataPacket *data=[PTPDataPacket new];data.transactionID=3;[usb sendData:data];
    assert(usb.dataCalls==1&&snapshot().data_submissions==1&&snapshot().paired_responses==1);
    response.transactionID=3;usb.acceptResponse=false;assert(![usb sendResponse:response]);
    assert(snapshot().failed_responses==1&&snapshot().paired_responses==1);usb.acceptResponse=true;
    /* Failed read provenance must not authorize a command even if a fixture
     * deliberately invokes its delegate anyway. */
    command.transactionID=4;[usb handleBulkData:12 result:-1];assert(snapshot().commands==2);
    usb.throwOnBulk=true;@try{[usb handleBulkData:12 result:0];assert(false);}@catch(NSException *e){assert([e.name isEqualToString:@"Fixture"]);}
    usb.throwOnBulk=false;
    [usb.responder handleRequest:command];assert(snapshot().commands==2); /* TLS restored */
    command.transactionID=5;[usb handleBulkData:12 result:0];assert(snapshot().commands==3);
    [usb stop];assert(!snapshot().connected);
    response.transactionID=5;[usb sendResponse:response];assert(snapshot().paired_responses==1);
    usb.connected=true;command.transactionID=6;[usb handleBulkData:12 result:0];assert(snapshot().commands==4);
    OCTObserverTestAuthorize(0,0);response.transactionID=6;[usb sendResponse:response];
    assert(snapshot().paired_responses==1);
    command.transactionID=7;[usb handleBulkData:12 result:0];assert(snapshot().commands==4);
    assert(usb.responseCalls==5&&usb.bulkCalls>=8);
    OCTObserverTestAuthorize(1,OCTObserverTestNow()-1);
    command.transactionID=8;[usb handleBulkData:12 result:0];assert(snapshot().commands==4);
    OCTObserverTestAuthorize(1,OCTObserverTestNow()+15000);
    command.operationCode=0x1005;command.transactionID=9;[usb handleBulkData:16 result:0];
    response.transactionID=9;[usb sendResponse:response];
    assert(OCTObserverTestStorageFlags()==3&&snapshot().paired_responses==2);
    /* Recorded in mode 2 as well. Gating this on mode 1 made the flags unreachable the moment a
     * user authorized exposure, which is exactly what the 10 November device run hit. */
    OCTObserverTestAuthorize(2,OCTObserverTestNow()+15000);
    command.operationCode=0x1004;command.transactionID=10;[usb handleBulkData:16 result:0];
    response.transactionID=10;[usb sendResponse:response];
    assert(OCTObserverTestStorageFlags()==3);
    puts("PASS observer wrappers: exact signatures, original forwarding, scoped provenance, async accounting, errors, disconnect, revoke (LOCAL boundary test)");
}return 0;}

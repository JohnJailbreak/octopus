#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <stdbool.h>
#include <stdint.h>
#include "../Core/oct_native_io.h"
BOOL OCTRuntimeCheck(const char *className, const char *selector, const char *encoding, NSString **reason);
BOOL OCTRuntimeHook(const char *className, const char *selector, const char *encoding, IMP replacement, IMP *original, NSString **reason);
bool OCTNativeProviderEnabled(void);
uint64_t OCTNativeAuthorizationEpoch(void);
void OCTNativeProviderEvent(const char *event, uint32_t handle, int32_t result);
/* `scope` is the root to serve from the first request; see the definition. */
BOOL OCTProviderInstall(oct_native_lease_mode scope, NSString **reason);
BOOL OCTProviderValidate(NSString **reason);
/* Declared here with the other provider entry points because this is the header the observer
 * imports; declaring it only in OctopusProvider.h left the observer calling an undeclared function.
 * The scope type comes from oct_native_io.h, included below. */
BOOL OCTProviderSetScope(oct_native_lease_mode scope, NSString **reason);
/* Announce the storage again if a host has not read the list since a store opened. The timer calls
 * it every poll, because an event sent before a session is up goes nowhere and there is no way to
 * know from inside when it comes up. A host reading the list stops it. */
void OCTProviderAnnounceIfNeeded(void);
/* Whether the grant allows writes, for the first DeviceInfo of a session to offer them before the
 * store has switched to the granted root. An atomic store only: safe before install. */
void OCTProviderSetWritesGranted(bool granted);
/* The two halves of an upload, taken over from Apple's responder, which refuses both before any
 * storage is consulted. Each returns whether it handled the request; when it did not, the caller
 * must fall through to Apple's implementation. See docs/usb-research/implementation-status.md. */
BOOL OCTProviderHandleRequest(id packet, id *answer);
BOOL OCTProviderHandleData(id packet);
/* Tell the host its storage list changed, so it reads it again without the cable being pulled.
 * Defined in the observer, which holds the transport; does nothing when no session is up. */
bool OCTNativeAnnounceStorage(uint32_t storage);
/* Any PTP event with one parameter, through the transport the host is on. Does nothing without a
 * session. Defined in the observer. */
bool OCTNativeSendEvent(uint16_t code, uint32_t parameter);
/* Answer a host's request through the transport, as Apple's responder does. `parameters` is how
 * many of the three are meaningful. Defined in the observer, which holds the transport. */
bool OCTNativeSendResponse(uint16_t code, uint32_t transaction, unsigned parameters,
                           uint32_t one, uint32_t two, uint32_t three);

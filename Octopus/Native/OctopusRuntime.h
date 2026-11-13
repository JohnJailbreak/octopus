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
/* Tell the host its storage list changed, so it reads it again without the cable being pulled.
 * Defined in the observer, which holds the transport; does nothing when no session is up. */
bool OCTNativeAnnounceStorage(uint32_t storage);
/* Any PTP event with one parameter, through the transport the host is on. Does nothing without a
 * session. Defined in the observer. */
bool OCTNativeSendEvent(uint16_t code, uint32_t parameter);

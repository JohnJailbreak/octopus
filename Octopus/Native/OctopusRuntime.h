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

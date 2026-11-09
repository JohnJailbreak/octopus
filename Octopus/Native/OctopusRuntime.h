#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <stdbool.h>
#include <stdint.h>
BOOL OCTRuntimeCheck(const char *className, const char *selector, const char *encoding, NSString **reason);
BOOL OCTRuntimeHook(const char *className, const char *selector, const char *encoding, IMP replacement, IMP *original, NSString **reason);
bool OCTNativeProviderEnabled(void);
uint64_t OCTNativeAuthorizationEpoch(void);
void OCTNativeProviderEvent(const char *event, uint32_t handle, int32_t result);
BOOL OCTProviderInstall(NSString **reason);
BOOL OCTProviderValidate(NSString **reason);

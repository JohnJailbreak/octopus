#import "OctopusRuntime.h"

/* Synthetic-only, read-only provider overlay. Caller verifies executable/OS
 * identity first. No transport claim, raw offsets, or Apple storage insertion. */
#define OCT_NATIVE_STORAGE_ID 0x4f430001u
#define OCT_NATIVE_FIRST_HANDLE 0x7f000001u
BOOL OCTProviderValidate(NSString **reason);
BOOL OCTProviderInstall(NSString **reason);

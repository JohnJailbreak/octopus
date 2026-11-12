#import "OctopusRuntime.h"
#include "../Core/oct_native_io.h"

/* Synthetic-only, read-only provider overlay. Caller verifies executable/OS
 * identity first. No transport claim, raw offsets, or Apple storage insertion. */
#define OCT_NATIVE_STORAGE_ID 0x4f430001u
#define OCT_NATIVE_FIRST_HANDLE 0x7f000001u
BOOL OCTProviderValidate(NSString **reason);
/* Switches the served store between the synthetic tree and a read-only view of one of the fixed
 * roots in oct_native_lease_root. Safe to call repeatedly; a root that cannot be opened falls back
 * to the synthetic tree so a host keeps seeing a working storage. */
BOOL OCTProviderSetScope(oct_native_lease_mode scope, NSString **reason);
BOOL OCTProviderInstall(NSString **reason);

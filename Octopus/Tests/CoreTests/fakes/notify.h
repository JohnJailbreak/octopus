/* Stand-in for Apple's <notify.h>: the same prototypes, so oct_native_io.c's Darwin branch builds
 * and runs in the portable tests. Not used for any device or app build. */
#include <stdint.h>
#define NOTIFY_STATUS_OK 0
uint32_t notify_register_check(const char *name, int *out_token);
uint32_t notify_set_state(int token, uint64_t state);
uint32_t notify_get_state(int token, uint64_t *state);
uint32_t notify_post(const char *name);
uint32_t notify_cancel(int token);
uint32_t notify_check(int token, int *check);

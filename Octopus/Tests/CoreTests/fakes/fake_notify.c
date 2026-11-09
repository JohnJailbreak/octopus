/* In-memory stand-in for notifyd: one name, one 64-bit state, counted posts. */
#include "notify.h"
#include <string.h>
static uint64_t state;
static int posts;
static int registered;
uint32_t notify_register_check(const char *name, int *token)
{
    if (strcmp(name, "com.parv.octopus.native.signal") != 0) return 1;
    *token = ++registered;
    return NOTIFY_STATUS_OK;
}
uint32_t notify_set_state(int token, uint64_t value) { (void)token; state = value; return NOTIFY_STATUS_OK; }
uint32_t notify_get_state(int token, uint64_t *value) { (void)token; *value = state; return NOTIFY_STATUS_OK; }
uint32_t notify_post(const char *name) { (void)name; posts++; return NOTIFY_STATUS_OK; }
uint32_t notify_cancel(int token) { (void)token; return NOTIFY_STATUS_OK; }
int fake_notify_posts(void);
int fake_notify_posts(void) { return posts; }

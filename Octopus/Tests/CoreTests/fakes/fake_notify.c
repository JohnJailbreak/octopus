/* In-memory stand-in for notifyd. One 64-bit state per name, counted posts per name, and
 * notify_check reporting a post since that token last checked, which is how the app tells a live
 * companion from a value left behind by one that has exited.
 *
 * Names outside com.parv.octopus.native are refused, as before. */
#include "notify.h"
#include <string.h>

#define FAKE_NAMES  8
#define FAKE_TOKENS 64
static struct { char name[64]; uint64_t state; int posts; } names[FAKE_NAMES];
static int name_count;
static struct { int name; int seen; } tokens[FAKE_TOKENS + 1];
static int registered;
static int posts_total;

static int lookup(const char *name)
{
    for (int i = 0; i < name_count; i++) if (!strcmp(names[i].name, name)) return i;
    if (name_count == FAKE_NAMES || strlen(name) >= sizeof names[0].name) return -1;
    strcpy(names[name_count].name, name);
    return name_count++;
}

uint32_t notify_register_check(const char *name, int *token)
{
    if (strncmp(name, "com.parv.octopus.native.", 24) != 0) return 1;
    int at = lookup(name);
    if (at < 0 || registered == FAKE_TOKENS) return 1;
    *token = ++registered;
    tokens[*token].name = at;
    /* A fresh registration has seen nothing, so its first check reports any earlier post. */
    tokens[*token].seen = 0;
    return NOTIFY_STATUS_OK;
}
static int valid(int token) { return token > 0 && token <= registered; }
uint32_t notify_set_state(int token, uint64_t value)
{
    if (!valid(token)) return 1;
    names[tokens[token].name].state = value;
    return NOTIFY_STATUS_OK;
}
uint32_t notify_get_state(int token, uint64_t *value)
{
    if (!valid(token)) return 1;
    *value = names[tokens[token].name].state;
    return NOTIFY_STATUS_OK;
}
uint32_t notify_post(const char *name)
{
    int at = lookup(name);
    if (at < 0) return 1;
    names[at].posts++;
    posts_total++;
    return NOTIFY_STATUS_OK;
}
uint32_t notify_check(int token, int *check)
{
    if (!valid(token) || !check) return 1;
    int now = names[tokens[token].name].posts;
    *check = now != tokens[token].seen;
    tokens[token].seen = now;
    return NOTIFY_STATUS_OK;
}
uint32_t notify_cancel(int token) { (void)token; return NOTIFY_STATUS_OK; }
int fake_notify_posts(void);
int fake_notify_posts(void) { return posts_total; }

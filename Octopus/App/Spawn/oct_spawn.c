/* oct_spawn.c - starts Octopus's root helper and collects what it prints. See oct_spawn.h. */

#define _GNU_SOURCE /* RTLD_DEFAULT on glibc, for the host tests */
#include "oct_spawn.h"

#include <dlfcn.h>
#include <errno.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* TrollStore's persona spawn uses private libsystem calls. They're looked up when needed rather than linked, so the
 * app also builds and runs where they don't exist: the iOS Simulator, which runs the tests, and the host tests. */
#define OCT_PERSONA_ID 99
#define OCT_PERSONA_FLAGS_OVERRIDE 1u
typedef int (*oct_set_persona_fn)(const posix_spawnattr_t *, uid_t, uint32_t);
typedef int (*oct_set_persona_uid_fn)(const posix_spawnattr_t *, uid_t);
typedef int (*oct_set_persona_gid_fn)(const posix_spawnattr_t *, gid_t);

__attribute__((format(printf, 1, 2)))
static char *oct_message(const char *format, ...) {
    va_list args;
    va_start(args, format);
    char *text = malloc(512);
    if (text) vsnprintf(text, 512, format, args);
    va_end(args);
    return text;
}

static int oct_use_root_persona(posix_spawnattr_t *attr) {
    void *persona = dlsym(RTLD_DEFAULT, "posix_spawnattr_set_persona_np");
    void *uid = dlsym(RTLD_DEFAULT, "posix_spawnattr_set_persona_uid_np");
    void *gid = dlsym(RTLD_DEFAULT, "posix_spawnattr_set_persona_gid_np");
    if (!persona || !uid || !gid) return 0;
    oct_set_persona_fn set_persona;
    oct_set_persona_uid_fn set_uid;
    oct_set_persona_gid_fn set_gid;
    memcpy(&set_persona, &persona, sizeof set_persona);
    memcpy(&set_uid, &uid, sizeof set_uid);
    memcpy(&set_gid, &gid, sizeof set_gid);
    return set_persona(attr, OCT_PERSONA_ID, OCT_PERSONA_FLAGS_OVERRIDE) == 0 && set_uid(attr, 0) == 0 &&
           set_gid(attr, 0) == 0;
}

int oct_spawn(const char *path, const char *const argv[], int as_root, char **output) {
    *output = NULL;
    if (!path || !argv || !argv[0]) {
        *output = oct_message("ERR\tNothing to start");
        return OCT_SPAWN_NOT_STARTED;
    }

    posix_spawnattr_t attr;
    int rc = posix_spawnattr_init(&attr);
    if (rc != 0) {
        *output = oct_message("ERR\tCouldn't prepare the update helper: %s", strerror(rc));
        return OCT_SPAWN_NOT_STARTED;
    }
    if (as_root && !oct_use_root_persona(&attr)) {
        posix_spawnattr_destroy(&attr);
        *output = oct_message("ERR\tThis iPhone can't start the update helper as root. Install Octopus with TrollStore.");
        return OCT_SPAWN_NOT_STARTED;
    }

    int fds[2];
    if (pipe(fds) != 0) {
        int error = errno;
        posix_spawnattr_destroy(&attr);
        *output = oct_message("ERR\tCouldn't create a pipe: %s", strerror(error));
        return OCT_SPAWN_NOT_STARTED;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    if (fds[1] > STDERR_FILENO) posix_spawn_file_actions_addclose(&actions, fds[1]);

    /* The helper needs nothing from the app's environment. */
    char *const envp[] = {"PATH=/usr/bin:/bin:/usr/sbin:/sbin", NULL};
    pid_t pid = 0;
    rc = posix_spawn(&pid, path, &actions, &attr, (char *const *)argv, envp);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    close(fds[1]);
    if (rc != 0) {
        close(fds[0]);
        *output = oct_message("ERR\tCouldn't start the update helper: %s", strerror(rc));
        return OCT_SPAWN_NOT_STARTED;
    }

    size_t capacity = 4096, length = 0;
    char *text = malloc(capacity);
    char chunk[4096];
    for (;;) {
        ssize_t got = read(fds[0], chunk, sizeof chunk);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        size_t count = (size_t)got;
        if (count > sizeof chunk) count = sizeof chunk; /* read() never returns more; stated for the analyzer */
        if (count > OCT_SPAWN_MAX_OUTPUT - length) count = OCT_SPAWN_MAX_OUTPUT - length;
        if (text && count > 0 && length + count + 1 > capacity) {
            while (length + count + 1 > capacity) capacity *= 2;
            char *grown = realloc(text, capacity);
            if (!grown) {
                free(text);
                text = NULL;
            } else {
                text = grown;
            }
        }
        if (text && count > 0) {
            memcpy(text + length, chunk, count);
            length += count;
        }
    }
    close(fds[0]);
    if (text) text[length] = '\0';
    *output = text;

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return OCT_SPAWN_KILLED;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return OCT_SPAWN_KILLED;
}

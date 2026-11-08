// octopushelper - Octopus's root helper. It only installs updates.
//
// Octopus starts it as root (TrollStore's persona spawn, Octopus/App/Spawn/oct_spawn.c) and reads its output: one line
// per event, its fields separated by a tab:
//
//   LOG <text>
//   ERR <text>
//
// Usage:
//   octopushelper install-update --tipa <path>
//   octopushelper version
//
// install-update hands a downloaded Octopus.tipa to TrollStore's installer, in its own session so the install finishes
// even though it closes Octopus, and reports how it went, so Octopus can fall back to TrollStore's install link. It is
// Speaka's and Caz's helper with only that part kept: no other files are touched, and nothing else runs as root.
//
// Exit codes: 0 done, 2 usage, 3 not root, 4 the update file is missing or outside Octopus's download folders,
// 5 TrollStore's installer wasn't found, 6 it couldn't be started, 7 it failed.
//
// Built by tools/package.sh. Test builds (-DOCT_HELPER_TEST, Octopus/Tests/HelperTests) start nothing: they put every
// absolute path under $OCTOPUS_ROOT and print what they would run.

#define _DEFAULT_SOURCE
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__) && !defined(OCT_HELPER_TEST)
#include <CoreFoundation/CoreFoundation.h>
#endif

#define OCT_HELPER_VERSION "1.0"
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define OCT_TROLLSTORE_ID "com.opa334.TrollStore"
#define OCT_TROLLSTORE_HELPER "TrollStore.app/trollstorehelper"
#define OCT_BUNDLE_CONTAINERS "/private/var/containers/Bundle/Application"
#define OCT_CONTAINER_METADATA ".com.apple.mobile_container_manager.metadata.plist"

// Where Octopus saves a downloaded update, in the order it tries them (Updater.downloadFolders). A file is accepted
// only directly inside one of these, after resolving links in its path.
static const char *const k_update_dirs[] = {
    "/var/mobile/Library/Caches/Octopus",
    "/var/tmp/Octopus",
};

extern char **environ;

static char g_root[PATH_MAX] = ""; // test builds only: prefix for every absolute path

// ---------------------------------------------------------------- output and paths

__attribute__((format(printf, 1, 2)))
static void emit(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    putchar('\n');
    fflush(stdout);
}

static int ends_with_ci(const char *text, const char *suffix) {
    size_t a = strlen(text), b = strlen(suffix);
    return a >= b && strcasecmp(text + a - b, suffix) == 0;
}

// Builds a path. On overflow it returns 0 and an empty string, which every caller treats as "doesn't exist".
__attribute__((format(printf, 2, 3)))
static int pathf(char *out, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int n = vsnprintf(out, PATH_MAX, format, args);
    va_end(args);
    if (n < 0 || n >= PATH_MAX) {
        out[0] = '\0';
        return 0;
    }
    return 1;
}

// An absolute device path, under the test root in test builds.
static int device_path(char *out, const char *absolute) { return pathf(out, "%s%s", g_root, absolute); }

static int is_dir(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int is_regular(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0 && S_ISREG(st.st_mode);
}

// The downloaded app: a .tipa or .ipa that is a regular file, not a link, directly inside a download folder.
static int update_file_ok(const char *path) {
    if (!path || !path[0]) return 0;
    if (!ends_with_ci(path, ".tipa") && !ends_with_ci(path, ".ipa")) return 0;
    if (!is_regular(path)) return 0;
    char real[PATH_MAX];
    if (!realpath(path, real)) return 0;
    for (size_t i = 0; i < sizeof k_update_dirs / sizeof k_update_dirs[0]; i++) {
        char dir[PATH_MAX], real_dir[PATH_MAX];
        // The folder itself must be a real directory, not a link to somewhere else.
        if (!device_path(dir, k_update_dirs[i]) || !is_dir(dir) || !realpath(dir, real_dir)) continue;
        size_t n = strlen(real_dir);
        if (strncmp(real, real_dir, n) == 0 && real[n] == '/' && strchr(real + n + 1, '/') == NULL) return 1;
    }
    return 0;
}

// ---------------------------------------------------------------- finding TrollStore (as in Caz's and Speaka's helper)

#if defined(__APPLE__) && !defined(OCT_HELPER_TEST)
// The bundle id in an app container's metadata (a binary plist, MCMMetadataIdentifier).
static int read_container_id(const char *dir, char *out, size_t length) {
    char path[PATH_MAX];
    if (!pathf(path, "%s/%s", dir, OCT_CONTAINER_METADATA)) return 0;
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 0;
    }
    long size = ftell(file);
    rewind(file);
    if (size <= 0 || size > (1 << 20)) {
        fclose(file);
        return 0;
    }
    UInt8 *bytes = malloc((size_t)size);
    if (!bytes) {
        fclose(file);
        return 0;
    }
    size_t got = fread(bytes, 1, (size_t)size, file);
    fclose(file);
    CFDataRef data = CFDataCreateWithBytesNoCopy(NULL, bytes, (CFIndex)got, kCFAllocatorMalloc);
    if (!data) {
        free(bytes);
        return 0;
    }
    CFPropertyListRef plist = CFPropertyListCreateWithData(NULL, data, kCFPropertyListImmutable, NULL, NULL);
    CFRelease(data);
    int ok = 0;
    if (plist && CFGetTypeID(plist) == CFDictionaryGetTypeID()) {
        CFTypeRef value = CFDictionaryGetValue((CFDictionaryRef)plist, CFSTR("MCMMetadataIdentifier"));
        if (value && CFGetTypeID(value) == CFStringGetTypeID())
            ok = CFStringGetCString((CFStringRef)value, out, (CFIndex)length, kCFStringEncodingUTF8) ? 1 : 0;
    }
    if (plist) CFRelease(plist);
    return ok;
}
#else
// Test builds: the fake metadata file holds the bundle id as text.
static int read_container_id(const char *dir, char *out, size_t length) {
    char path[PATH_MAX];
    if (!pathf(path, "%s/%s", dir, OCT_CONTAINER_METADATA)) return 0;
    FILE *file = fopen(path, "r");
    if (!file) return 0;
    if (!fgets(out, (int)length, file)) {
        fclose(file);
        return 0;
    }
    fclose(file);
    out[strcspn(out, "\r\n")] = '\0';
    return out[0] != '\0';
}
#endif

// The installer inside TrollStore.app, found through the bundle container whose metadata names TrollStore.
static int find_trollstore_helper(char *out) {
    char base[PATH_MAX];
    out[0] = '\0';
    if (!device_path(base, OCT_BUNDLE_CONTAINERS)) return 0;
    DIR *containers = opendir(base);
    if (!containers) return 0;
    struct dirent *entry;
    while ((entry = readdir(containers)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char dir[PATH_MAX], id[256] = "";
        if (!pathf(dir, "%s/%s", base, entry->d_name) || !is_dir(dir)) continue;
        if (!read_container_id(dir, id, sizeof id) || strcmp(id, OCT_TROLLSTORE_ID) != 0) continue;
        if (pathf(out, "%s/%s", dir, OCT_TROLLSTORE_HELPER) && is_regular(out)) break;
        out[0] = '\0';
    }
    closedir(containers);
    return out[0] != '\0';
}

// ---------------------------------------------------------------- running TrollStore's installer

// Runs a program in its own session and waits for it; its output goes where this helper's goes. -1 if it couldn't
// start (errno says why).
static int run_and_wait(const char *path, char *const argv[]) {
#ifdef OCT_HELPER_TEST
    emit("LOG\t(test) would run %s in its own session", path);
    for (int i = 1; argv[i]; i++) emit("LOG\t(test)   %s", argv[i]);
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    const char *nostart = getenv("OCTOPUS_TEST_NOSTART"); // the basename of a program that can't start
    if (nostart && strcmp(name, nostart) == 0) {
        errno = ENOENT;
        return -1;
    }
    const char *fail = getenv("OCTOPUS_TEST_FAIL"); // the basename of a program that fails
    return fail && strcmp(name, fail) == 0 ? 1 : 0;
#else
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
#ifdef POSIX_SPAWN_SETSID
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
#endif
    pid_t pid;
    int rc = posix_spawn(&pid, path, NULL, &attr, argv, environ);
    posix_spawnattr_destroy(&attr);
    if (rc != 0) {
        errno = rc;
        return -1;
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

static int install_update(const char *tipa) {
    if (!update_file_ok(tipa)) {
        emit("ERR\tThe downloaded update wasn't found");
        return 4;
    }
    char helper[PATH_MAX];
    if (!find_trollstore_helper(helper)) {
        emit("ERR\tTrollStore's installer wasn't found. Is TrollStore installed?");
        return 5;
    }
    emit("LOG\tInstalling the update with TrollStore");
    char *argv[] = {"trollstorehelper", "install", (char *)tipa, NULL};
    int code = run_and_wait(helper, argv);
    int error = errno;
    unlink(tipa); // the download is used once, whatever happened
    if (code == -1) {
        emit("ERR\tCouldn't start TrollStore's installer: %s", strerror(error));
        return 6;
    }
    if (code != 0) {
        emit("ERR\tTrollStore's installer returned %d", code);
        return 7;
    }
    emit("LOG\tUpdate installed");
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN); // Octopus may close before the last line arrives
    umask(022);
#ifdef OCT_HELPER_TEST
    const char *root = getenv("OCTOPUS_ROOT");
    if (root) snprintf(g_root, sizeof g_root, "%s", root);
#else
    if (geteuid() != 0) {
        emit("ERR\tThe update helper isn't running as root. Install Octopus with TrollStore.");
        return 3;
    }
#endif
    if (argc < 2) {
        emit("ERR\tusage: octopushelper install-update --tipa <path> | version");
        return 2;
    }
    const char *tipa = NULL;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--tipa") == 0 && i + 1 < argc) {
            tipa = argv[++i];
        } else {
            emit("ERR\tUnknown option %s", argv[i]);
            return 2;
        }
    }
    if (strcmp(argv[1], "version") == 0) {
        emit("LOG\toctopushelper %s", OCT_HELPER_VERSION);
        return 0;
    }
    if (strcmp(argv[1], "install-update") == 0) {
        if (!tipa) {
            emit("ERR\tNothing to install");
            return 2;
        }
        return install_update(tipa);
    }
    emit("ERR\tUnknown command %s", argv[1]);
    return 2;
}

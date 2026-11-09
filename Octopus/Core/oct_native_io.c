#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#else
#define _POSIX_C_SOURCE 200809L
#endif
#include "oct_native_io.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static bool valid_name(const char *name) { return name && (!strcmp(name,"lease") || !strcmp(name,"status")); }
/* Records the stage and the errno of the call that failed. Callers capture errno immediately
 * after the failing call and before any cleanup, which may overwrite it. */
static void fail_at(oct_native_io_failure *failure, oct_native_io_stage stage, int error)
{
    if (failure) { failure->stage = stage; failure->error = error; }
}

static int open_directory(const char *path, bool create, oct_native_io_failure *failure)
{
    if (!path) { fail_at(failure, OCT_NATIVE_IO_ARGUMENT, EINVAL); errno = EINVAL; return -1; }
    if (create && mkdir(path, 0700) < 0 && errno != EEXIST) {
        int error = errno; fail_at(failure, OCT_NATIVE_IO_CREATE_DIRECTORY, error); errno = error; return -1;
    }
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) { int error = errno; fail_at(failure, OCT_NATIVE_IO_OPEN_DIRECTORY, error); errno = error; return -1; }
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0077)) {
        /* A policy rejection, not a refusal by the kernel: kept apart from OPEN_DIRECTORY. */
        close(fd); fail_at(failure, OCT_NATIVE_IO_VALIDATE_DIRECTORY, EPERM); errno = EPERM; return -1;
    }
    return fd;
}
oct_status oct_native_io_read_detailed(const char *directory, const char *name, void *bytes, size_t capacity,
                                       size_t *length, oct_native_io_failure *failure)
{
    fail_at(failure, OCT_NATIVE_IO_OK, 0);
    if (length) *length = 0;
    if (!valid_name(name) || !bytes || !length || capacity > 65536) {
        fail_at(failure, OCT_NATIVE_IO_ARGUMENT, EINVAL); errno = EINVAL;
        return OCT_E_INVALID_ARGUMENT;
    }
    int dir = open_directory(directory, false, failure);
    if (dir < 0) return OCT_E_IO;
    int fd = openat(dir, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    int saved = errno;
    close(dir);
    if (fd < 0) { fail_at(failure, OCT_NATIVE_IO_OPEN_FILE, saved); errno = saved; return OCT_E_IO; }
    struct stat st;
    oct_status result = OCT_E_IO;
    if (fstat(fd, &st) < 0) {
        saved = errno; fail_at(failure, OCT_NATIVE_IO_VALIDATE_FILE, saved);
    } else if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0077) || st.st_nlink != 1) {
        saved = EPERM; fail_at(failure, OCT_NATIVE_IO_VALIDATE_FILE, saved);
    } else if (st.st_size < 0 || (uint64_t)st.st_size > capacity) {
        saved = EOVERFLOW; fail_at(failure, OCT_NATIVE_IO_VALIDATE_FILE, saved);
    } else {
        size_t pos = 0;
        result = OCT_OK;
        while (pos < (size_t)st.st_size) {
            ssize_t n = read(fd, (char *)bytes + pos, (size_t)st.st_size - pos);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { saved = n ? errno : EIO; fail_at(failure, OCT_NATIVE_IO_READ, saved); result = OCT_E_IO; break; }
            pos += (size_t)n;
        }
        /* Concurrent growth is rejected; writers normally publish by rename. */
        if (result == OCT_OK) {
            char extra;
            ssize_t n;
            do { n = read(fd, &extra, 1); } while (n < 0 && errno == EINTR);
            if (n != 0) { saved = n > 0 ? EOVERFLOW : errno; fail_at(failure, OCT_NATIVE_IO_READ, saved); result = OCT_E_IO; }
        }
        if (result == OCT_OK) *length = pos;
    }
    close(fd);
    errno = result == OCT_OK ? 0 : saved;
    return result;
}

oct_status oct_native_io_read(const char *directory, const char *name, void *bytes, size_t capacity, size_t *length)
{
    return oct_native_io_read_detailed(directory, name, bytes, capacity, length, NULL);
}
oct_status oct_native_io_write_detailed(const char *directory, const char *name, const void *bytes, size_t length,
                                        oct_native_io_failure *failure)
{
    fail_at(failure, OCT_NATIVE_IO_OK, 0);
    if (!valid_name(name) || (!bytes && length) || length > 65536) {
        fail_at(failure, OCT_NATIVE_IO_ARGUMENT, EINVAL); errno = EINVAL; return OCT_E_INVALID_ARGUMENT;
    }
    int dir = open_directory(directory, true, failure);
    if (dir < 0) return OCT_E_IO;
    char temporary[64];
    snprintf(temporary, sizeof temporary, ".%s.%ld.tmp", name, (long)getpid());
    int fd = openat(dir, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) { int error = errno; close(dir); fail_at(failure, OCT_NATIVE_IO_OPEN_FILE, error); errno = error; return OCT_E_IO; }
    /* Each failure records its own errno at the point it happens. The old code read errno only
     * after close(), so a failed write() or fsync() could be reported with close()'s errno. */
    int error = 0;
    oct_native_io_stage stage = OCT_NATIVE_IO_OK;
    size_t pos = 0;
    while (pos < length) {
        ssize_t n = write(fd, (const char *)bytes + pos, length - pos);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { error = n ? errno : EIO; stage = OCT_NATIVE_IO_WRITE; break; }
        pos += (size_t)n;
    }
    if (stage == OCT_NATIVE_IO_OK && fsync(fd) < 0) { error = errno; stage = OCT_NATIVE_IO_FSYNC; }
    if (close(fd) < 0 && stage == OCT_NATIVE_IO_OK) { error = errno; stage = OCT_NATIVE_IO_CLOSE; }
    if (stage == OCT_NATIVE_IO_OK && renameat(dir, temporary, dir, name) < 0) { error = errno; stage = OCT_NATIVE_IO_RENAME; }
    if (stage != OCT_NATIVE_IO_OK) unlinkat(dir, temporary, 0);
    close(dir);
    fail_at(failure, stage, error);
    errno = error;
    return stage == OCT_NATIVE_IO_OK ? OCT_OK : OCT_E_IO;
}

oct_status oct_native_io_write(const char *directory, const char *name, const void *bytes, size_t length)
{
    return oct_native_io_write_detailed(directory, name, bytes, length, NULL);
}
oct_status oct_native_io_remove_lease(const char *directory)
{
    int dir=open_directory(directory, false, NULL);if(dir<0)return errno==ENOENT?OCT_OK:OCT_E_IO;
    int rc=unlinkat(dir,"lease",0);int saved=errno;close(dir);errno=saved;
    return rc==0||errno==ENOENT?OCT_OK:OCT_E_IO;
}
static bool valid_token(const char *token)
{
    if(!token||strlen(token)!=32)return false;
    for(size_t i=0;i<32;i++)if(!((token[i]>='0'&&token[i]<='9')||(token[i]>='a'&&token[i]<='f')))return false;
    return true;
}
oct_status oct_native_write_lease(const char *directory,const char *token,unsigned mode,uint64_t now_ms)
{
    if(!valid_token(token)||mode<1||mode>2||now_ms>UINT64_MAX-OCT_NATIVE_LEASE_MS)return OCT_E_INVALID_ARGUMENT;
    char bytes[96];int n=snprintf(bytes,sizeof bytes,"OCTN1 %" PRIu64 " %u %s\n",now_ms+OCT_NATIVE_LEASE_MS,mode,token);
    if(n<0||(size_t)n>=sizeof bytes)return OCT_E_IO;
    return oct_native_io_write(directory,"lease",bytes,(size_t)n);
}
/* Exactly canonical decimal, with explicit overflow checks. scanf's unsigned
 * conversions accept signs and have undefined behavior on unrepresentable input. */
static bool decimal(const char **cursor, const char *end, uint64_t *value)
{
    const char *p = *cursor;
    uint64_t result = 0;
    if (p == end || *p < '0' || *p > '9') return false;
    if (*p == '0' && p + 1 < end && p[1] >= '0' && p[1] <= '9') return false;
    while (p < end && *p >= '0' && *p <= '9') {
        unsigned digit = (unsigned)(*p - '0');
        if (result > (UINT64_MAX - digit) / 10) return false;
        result = result * 10 + digit;
        p++;
    }
    *cursor = p;
    *value = result;
    return true;
}

unsigned oct_native_parse_lease(const void *bytes, size_t length, uint64_t now_ms, uint64_t *deadline, char token[33])
{
    if (deadline) *deadline = 0;
    if (token) token[0] = 0;
    if (!deadline || !token || !bytes || length > 96) { errno = EINVAL; return 0; }
    char parsed[33];
    uint64_t expires = 0, mode = 0;
    const char *p = bytes, *end = p + length;
    if (length < 6 || memcmp(p, "OCTN1 ", 6)) { errno = EINVAL; return 0; }
    p += 6;
    if (!decimal(&p, end, &expires) || p == end || *p++ != ' ' ||
        !decimal(&p, end, &mode) || p == end || *p++ != ' ' ||
        end - p != 33 || p[32] != '\n') { errno = EINVAL; return 0; }
    memcpy(parsed, p, 32);
    parsed[32] = 0;
    if (!valid_token(parsed) || mode < 1 || mode > 2 || expires <= now_ms ||
        expires - now_ms > OCT_NATIVE_LEASE_MS) { errno = EINVAL; return 0; }
    *deadline = expires;
    memcpy(token, parsed, 33);
    return (unsigned)mode;
}

unsigned oct_native_read_lease_detailed(const char *directory, uint64_t now_ms, uint64_t *deadline, char token[33],
                                        oct_native_io_failure *failure)
{
    fail_at(failure, OCT_NATIVE_IO_OK, 0);
    if (deadline) *deadline = 0;
    if (token) token[0] = 0;
    if (!deadline || !token) { fail_at(failure, OCT_NATIVE_IO_ARGUMENT, EINVAL); errno = EINVAL; return 0; }
    char bytes[96];
    size_t length = 0;
    if (oct_native_io_read_detailed(directory, "lease", bytes, sizeof bytes, &length, failure) != OCT_OK) return 0;
    unsigned mode = oct_native_parse_lease(bytes, length, now_ms, deadline, token);
    /* Readable but rejected: expired, malformed, or an unknown mode. */
    if (!mode) fail_at(failure, OCT_NATIVE_IO_CONTENT, 0);
    return mode;
}

unsigned oct_native_read_lease(const char *directory, uint64_t now_ms, uint64_t *deadline, char token[33])
{
    return oct_native_read_lease_detailed(directory, now_ms, deadline, token, NULL);
}

/* ---- Stage names and the startup signal ------------------------------------------------------ */

const char *oct_native_io_stage_name(oct_native_io_stage stage)
{
    switch (stage) {
    case OCT_NATIVE_IO_OK: return "ok";
    case OCT_NATIVE_IO_ARGUMENT: return "argument";
    case OCT_NATIVE_IO_CREATE_DIRECTORY: return "create-directory";
    case OCT_NATIVE_IO_OPEN_DIRECTORY: return "open-directory";
    case OCT_NATIVE_IO_VALIDATE_DIRECTORY: return "validate-directory";
    case OCT_NATIVE_IO_OPEN_FILE: return "open-file";
    case OCT_NATIVE_IO_VALIDATE_FILE: return "validate-file";
    case OCT_NATIVE_IO_READ: return "read";
    case OCT_NATIVE_IO_WRITE: return "write";
    case OCT_NATIVE_IO_FSYNC: return "fsync";
    case OCT_NATIVE_IO_CLOSE: return "close";
    case OCT_NATIVE_IO_RENAME: return "rename";
    case OCT_NATIVE_IO_CONTENT: return "content";
    }
    return "unknown";
}

static uint8_t signal_check(uint64_t word)
{
    uint8_t check = 0;
    for (unsigned i = 0; i < 7; i++) check ^= (uint8_t)(word >> (8 * i));
    return check;
}

uint64_t oct_native_signal_pack(const oct_native_signal *s)
{
    if (!s) return 0;
    uint64_t word = (uint64_t)(OCT_NATIVE_SIGNAL_VERSION & 0xFu)
                  | (uint64_t)(s->phase & 0xFu) << 4
                  | (uint64_t)(s->identity & 0xFu) << 8
                  | (uint64_t)(s->hooks & 0xFu) << 12
                  | (uint64_t)(s->lease_stage & 0xFu) << 16
                  | (uint64_t)(s->status_stage & 0xFu) << 20
                  | (uint64_t)s->lease_error << 24
                  | (uint64_t)s->status_error << 32
                  | (uint64_t)s->pid << 40;
    return word | (uint64_t)signal_check(word) << 56;
}

bool oct_native_signal_unpack(uint64_t word, oct_native_signal *out)
{
    if (!out) return false;
    memset(out, 0, sizeof *out);
    if (!word || (word & 0xFu) != OCT_NATIVE_SIGNAL_VERSION) return false;
    if ((uint8_t)(word >> 56) != signal_check(word & 0x00FFFFFFFFFFFFFFull)) return false;
    oct_native_signal s = {
        .phase = (uint8_t)((word >> 4) & 0xFu),
        .identity = (uint8_t)((word >> 8) & 0xFu),
        .hooks = (uint8_t)((word >> 12) & 0xFu),
        .lease_stage = (uint8_t)((word >> 16) & 0xFu),
        .status_stage = (uint8_t)((word >> 20) & 0xFu),
        .lease_error = (uint8_t)(word >> 24),
        .status_error = (uint8_t)(word >> 32),
        .pid = (uint16_t)(word >> 40),
    };
    if (s.phase > OCT_NATIVE_PHASE_HOOKED || s.identity > OCT_NATIVE_IDENTITY_WRONG_DIGEST ||
        s.hooks > OCT_NATIVE_HOOKS_REFUSED || s.lease_stage > OCT_NATIVE_IO_CONTENT ||
        s.status_stage > OCT_NATIVE_IO_CONTENT) {
        return false;
    }
    *out = s;
    return true;
}

bool oct_native_signal_from_pid(const oct_native_signal *s, int32_t pid)
{
    return s && pid > 0 && s->pid == (uint16_t)(pid & 0xFFFF);
}

static bool sandbox_like(uint8_t stage, uint8_t error)
{
    /* open() or mkdir() refused outright. EPERM is what a seatbelt denial surfaces as; a policy
     * rejection of our own is VALIDATE_DIRECTORY, so it is never confused with this. */
    return (stage == OCT_NATIVE_IO_OPEN_DIRECTORY || stage == OCT_NATIVE_IO_CREATE_DIRECTORY ||
            stage == OCT_NATIVE_IO_OPEN_FILE) && (error == EPERM || error == EACCES);
}

oct_native_verdict oct_native_signal_describe(const oct_native_signal *s, bool present, int32_t observed_pid,
                                              char *text, size_t capacity)
{
    char scratch[8];
    if (!text || !capacity) { text = scratch; capacity = sizeof scratch; }
    if (!present || !s || s->phase == OCT_NATIVE_PHASE_NONE) {
        snprintf(text, capacity, "No startup signal from the companion. Its constructor never posted in ptpd: "
                 "it was not loaded (injection, dyld or code-signature failure) or notifications were refused.");
        return OCT_NATIVE_VERDICT_NO_SIGNAL;
    }
    if (observed_pid > 0 && !oct_native_signal_from_pid(s, observed_pid)) {
        snprintf(text, capacity, "The last startup signal came from an earlier ptpd, not the one running now "
                 "(pid %ld). Reconnect USB so ptpd restarts with the companion.", (long)observed_pid);
        return OCT_NATIVE_VERDICT_STALE;
    }
    if (s->identity != OCT_NATIVE_IDENTITY_COMPATIBLE) {
        static const char *const why[] = { "not yet checked", "compatible", "the process is not ptpd",
            "the system is not iOS 17.0.0", "ptpd's UUID differs", "ptpd is not arm64e", "ptpd's SHA-256 differs" };
        snprintf(text, capacity, "The companion was loaded into ptpd but stopped itself: %s. Nothing was hooked.",
                 s->identity < sizeof why / sizeof why[0] ? why[s->identity] : "unknown");
        return OCT_NATIVE_VERDICT_IDENTITY;
    }
    if (sandbox_like(s->lease_stage, s->lease_error) || sandbox_like(s->status_stage, s->status_error)) {
        bool lease = sandbox_like(s->lease_stage, s->lease_error);
        snprintf(text, capacity, "The companion is loaded and compatible, but ptpd's sandbox refused the shared "
                 "folder: %s failed at %s with errno %u. The same uid does not grant the same sandbox rights.",
                 lease ? "reading the lease" : "writing status",
                 oct_native_io_stage_name((oct_native_io_stage)(lease ? s->lease_stage : s->status_stage)),
                 (unsigned)(lease ? s->lease_error : s->status_error));
        return OCT_NATIVE_VERDICT_SANDBOX;
    }
    if (s->phase < OCT_NATIVE_PHASE_LEASE) {
        snprintf(text, capacity, "The companion is loaded and compatible, and waiting for authorization: the last "
                 "lease read ended at %s (errno %u). Keep Octopus open with USB Access enabled.",
                 oct_native_io_stage_name((oct_native_io_stage)s->lease_stage), (unsigned)s->lease_error);
        return OCT_NATIVE_VERDICT_NO_LEASE;
    }
    if (s->status_stage != OCT_NATIVE_IO_OK) {
        snprintf(text, capacity, "A lease was read, but writing status failed at %s with errno %u.",
                 oct_native_io_stage_name((oct_native_io_stage)s->status_stage), (unsigned)s->status_error);
        return OCT_NATIVE_VERDICT_STATUS_FAILED;
    }
    if (s->hooks == OCT_NATIVE_HOOKS_REFUSED) {
        snprintf(text, capacity, "Status is being written, but an encoding or provenance check refused the observer "
                 "hooks, so nothing in ptpd was changed.");
        return OCT_NATIVE_VERDICT_HOOKS_REFUSED;
    }
    snprintf(text, capacity, "The companion is running in ptpd: status written%s.",
             s->hooks == OCT_NATIVE_HOOKS_INSTALLED ? " and observer hooks installed" : ", hooks not yet wanted");
    return OCT_NATIVE_VERDICT_RUNNING;
}

/* OCT_NATIVE_FAKE_NOTIFY lets the portable tests build this branch against a stand-in notify.h
 * with Apple's prototypes, so the code that runs on the device is exercised, not just the stub. */
#if defined(__APPLE__) || defined(OCT_NATIVE_FAKE_NOTIFY)
#include <notify.h>
#include <pthread.h>

static int signal_token = -1;
static pthread_once_t signal_once = PTHREAD_ONCE_INIT;
static void signal_register(void)
{
    int token = -1;
    if (notify_register_check(OCT_NATIVE_SIGNAL_NAME, &token) == NOTIFY_STATUS_OK) signal_token = token;
}

oct_status oct_native_signal_publish(const oct_native_signal *s)
{
    if (!s) return OCT_E_INVALID_ARGUMENT;
    /* The token is kept for the life of the process: notifyd keeps a name's state while someone
     * holds a registration, so ptpd's open token is what keeps the signal readable. */
    pthread_once(&signal_once, signal_register);
    if (signal_token < 0) return OCT_E_IO;
    if (notify_set_state(signal_token, oct_native_signal_pack(s)) != NOTIFY_STATUS_OK) return OCT_E_IO;
    return notify_post(OCT_NATIVE_SIGNAL_NAME) == NOTIFY_STATUS_OK ? OCT_OK : OCT_E_IO;
}

oct_status oct_native_signal_read(oct_native_signal *out)
{
    if (!out) return OCT_E_INVALID_ARGUMENT;
    memset(out, 0, sizeof *out);
    int token = -1;
    if (notify_register_check(OCT_NATIVE_SIGNAL_NAME, &token) != NOTIFY_STATUS_OK) return OCT_E_IO;
    uint64_t word = 0;
    uint32_t status = notify_get_state(token, &word);
    notify_cancel(token);
    if (status != NOTIFY_STATUS_OK) return OCT_E_IO;
    return oct_native_signal_unpack(word, out) ? OCT_OK : OCT_E_STATE;
}
#else
oct_status oct_native_signal_publish(const oct_native_signal *s) { (void)s; return OCT_E_UNSUPPORTED; }
oct_status oct_native_signal_read(oct_native_signal *out)
{
    if (out) memset(out, 0, sizeof *out);
    return OCT_E_UNSUPPORTED;
}
#endif

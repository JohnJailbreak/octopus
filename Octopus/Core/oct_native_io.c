#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#else
#define _POSIX_C_SOURCE 200809L
#endif
#include "oct_native_io.h"
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
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
    case OCT_NATIVE_IO_NOT_ATTEMPTED: return "not-attempted";
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
    uint64_t word = (uint64_t)(OCT_NATIVE_SIGNAL_VERSION_2 & 0xFu)
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
    uint32_t version = (uint32_t)(word & 0xFu);
    /* Version 1 is what 0.4.24 posted and is still read, so a signal left in notifyd by the
     * previous build decodes rather than being discarded as corrupt. */
    if (!word || (version != OCT_NATIVE_SIGNAL_VERSION && version != OCT_NATIVE_SIGNAL_VERSION_2)) {
        return false;
    }
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
    /* Each version is held to the range it could legally carry, so a value only version 2 can
     * mean is rejected in a version 1 word rather than silently accepted. */
    uint8_t identity_limit = version == OCT_NATIVE_SIGNAL_VERSION
                                 ? (uint8_t)OCT_NATIVE_IDENTITY_WRONG_DIGEST
                                 : (uint8_t)OCT_NATIVE_IDENTITY_LAST;
    uint8_t stage_limit = version == OCT_NATIVE_SIGNAL_VERSION
                              ? (uint8_t)OCT_NATIVE_IO_CONTENT
                              : (uint8_t)OCT_NATIVE_IO_NOT_ATTEMPTED;
    if (s.phase > OCT_NATIVE_PHASE_HOOKED || s.identity > identity_limit ||
        s.hooks > OCT_NATIVE_HOOKS_REFUSED || s.lease_stage > stage_limit ||
        s.status_stage > stage_limit) {
        return false;
    }
    *out = s;
    return true;
}

bool oct_native_signal_from_pid(const oct_native_signal *s, int32_t pid)
{
    return s && pid > 0 && s->pid == (uint16_t)(pid & 0xFFFF);
}

uint64_t oct_native_detail_pack(const oct_native_detail *detail)
{
    if (!detail) return 0;
    uint64_t word = 0;
    for (unsigned i = 0; i < 6; i++) word |= (uint64_t)detail->uuid_prefix[i] << (8 * i);
    word |= (uint64_t)detail->image_index << 48;
    word |= (uint64_t)detail->image_count << 56;
    return word;
}

bool oct_native_detail_unpack(uint64_t word, oct_native_detail *out)
{
    if (!out) return false;
    memset(out, 0, sizeof *out);
    if (!word) return false; /* nothing posted */
    for (unsigned i = 0; i < 6; i++) out->uuid_prefix[i] = (uint8_t)(word >> (8 * i));
    out->image_index = (uint8_t)(word >> 48);
    out->image_count = (uint8_t)(word >> 56);
    return true;
}

/* ---- Authorization carried by notification -------------------------------------------------
 * Layout: version:4 mode:4 deadline:40 generation:8 check:8. The check byte rejects a corrupted or
 * foreign word; it is not a signature. See the header for what this channel can and cannot carry. */
static uint8_t lease_check(uint64_t word)
{
    uint8_t check = 0;
    for (unsigned i = 0; i < 7; i++) check ^= (uint8_t)(word >> (8 * i));
    return check;
}

uint64_t oct_native_lease_pack(const oct_native_lease_word *lease)
{
    if (!lease) return 0;
    uint64_t word = (uint64_t)(OCT_NATIVE_LEASE_VERSION & 0xFu)
                  | (uint64_t)(lease->mode & 0xFu) << 4
                  | (lease->deadline_ms & 0xFFFFFFFFFFull) << 8
                  | (uint64_t)(lease->generation & 0xFFu) << 48;
    return word | (uint64_t)lease_check(word) << 56;
}

bool oct_native_lease_unpack(uint64_t word, oct_native_lease_word *out)
{
    if (!out) return false;
    memset(out, 0, sizeof *out);
    if (!word || (word & 0xFu) != OCT_NATIVE_LEASE_VERSION) return false;
    if ((uint8_t)(word >> 56) != lease_check(word & 0x00FFFFFFFFFFFFFFull)) return false;
    uint8_t mode = (uint8_t)((word >> 4) & 0xFu);
    if (mode > OCT_NATIVE_LEASE_MODE_LAST) return false;
    out->mode = mode;
    out->deadline_ms = (word >> 8) & 0xFFFFFFFFFFull;
    out->generation = (uint16_t)((word >> 48) & 0xFFu);
    return true;
}

bool oct_native_transport_fresh(uint64_t now_ms, uint64_t last_heard_ms, uint64_t window_ms)
{
    if (last_heard_ms == 0 || now_ms < last_heard_ms) return false;
    return now_ms - last_heard_ms <= window_ms;
}

oct_native_transport_stage oct_native_transport_stage_of(const oct_native_runtime *runtime, bool fresh)
{
    if (!runtime || !fresh) return OCT_NATIVE_TRANSPORT_ABSENT;
    if (!(runtime->flags & OCT_NATIVE_RUNTIME_HOOKED)) return OCT_NATIVE_TRANSPORT_ABSENT;
    if (runtime->commands == 0) return OCT_NATIVE_TRANSPORT_WAITING;
    if (runtime->flags & OCT_NATIVE_RUNTIME_SERVING) return OCT_NATIVE_TRANSPORT_SERVING;
    return OCT_NATIVE_TRANSPORT_HOST;
}

const char *oct_native_transport_stage_name(oct_native_transport_stage stage)
{
    switch (stage) {
    case OCT_NATIVE_TRANSPORT_ABSENT: return "Transport unavailable";
    case OCT_NATIVE_TRANSPORT_WAITING: return "Waiting for a connection";
    case OCT_NATIVE_TRANSPORT_HOST: return "Host connected";
    case OCT_NATIVE_TRANSPORT_SERVING: return "Session active";
    }
    return "Transport unavailable";
}

const char *oct_native_root_state_name(oct_native_root_state state)
{
    switch (state) {
    case OCT_NATIVE_ROOT_UNREACHABLE: return "not reachable";
    case OCT_NATIVE_ROOT_REFUSED: return "refused by the sandbox";
    case OCT_NATIVE_ROOT_OPENED_EMPTY: return "opens but lists nothing";
    case OCT_NATIVE_ROOT_READABLE: return "readable";
    }
    return "unknown";
}

oct_native_root_state oct_native_root_state_of(const oct_native_root_report *report, oct_native_root root)
{
    if (!report || root >= OCT_NATIVE_ROOT_COUNT) return OCT_NATIVE_ROOT_UNREACHABLE;
    uint8_t bit = (uint8_t)(1u << (unsigned)root);
    /* Refused outranks the rest: a sandbox denial is the fact worth reporting even if something
     * else about the probe also failed. */
    if (report->refused & bit) return OCT_NATIVE_ROOT_REFUSED;
    if (report->listable & bit) return OCT_NATIVE_ROOT_READABLE;
    if (report->openable & bit) return OCT_NATIVE_ROOT_OPENED_EMPTY;
    return OCT_NATIVE_ROOT_UNREACHABLE;
}

oct_native_lease_mode oct_native_lease_mode_for_root(oct_native_root root)
{
    if (root == OCT_NATIVE_ROOT_SYNTHETIC) return OCT_NATIVE_LEASE_EXPOSE_SYNTHETIC;
    if (root >= OCT_NATIVE_ROOT_COUNT) return OCT_NATIVE_LEASE_NONE;
    return (oct_native_lease_mode)((unsigned)OCT_NATIVE_LEASE_EXPOSE_ROOT_BASE + (unsigned)root - 1u);
}

const char *oct_native_lease_root(oct_native_lease_mode mode)
{
    /* One table, shared with the probe above, so a scope a lease can name is always a root whose
     * reachability the companion also reports. Read-only modes start at one base, writable ones at
     * another, and both index the same roots. */
    unsigned base;
    if (mode >= OCT_NATIVE_LEASE_WRITE_ROOT_BASE && mode <= OCT_NATIVE_LEASE_MODE_LAST) {
        base = OCT_NATIVE_LEASE_WRITE_ROOT_BASE;
    } else if (mode >= OCT_NATIVE_LEASE_EXPOSE_ROOT_BASE &&
               mode < OCT_NATIVE_LEASE_WRITE_ROOT_BASE) {
        base = OCT_NATIVE_LEASE_EXPOSE_ROOT_BASE;
    } else {
        return NULL;
    }
    unsigned root = (unsigned)mode - base + 1u;
    return root < OCT_NATIVE_ROOT_COUNT ? oct_native_root_path((oct_native_root)root) : NULL;
}

bool oct_native_lease_is_writable(oct_native_lease_mode mode)
{
    return mode >= OCT_NATIVE_LEASE_WRITE_ROOT_BASE && mode <= OCT_NATIVE_LEASE_MODE_LAST &&
           oct_native_lease_root(mode) != NULL;
}

oct_native_lease_mode oct_native_lease_write_mode_for_root(oct_native_root root)
{
    if (root == OCT_NATIVE_ROOT_SYNTHETIC || root >= OCT_NATIVE_ROOT_COUNT) return OCT_NATIVE_LEASE_NONE;
    return (oct_native_lease_mode)((unsigned)OCT_NATIVE_LEASE_WRITE_ROOT_BASE + (unsigned)root - 1u);
}

bool oct_native_lease_is_filesystem(oct_native_lease_mode mode)
{
    return oct_native_lease_root(mode) != NULL;
}

oct_native_lease_mode oct_native_lease_effective(const oct_native_lease_word *lease, uint64_t now_ms)
{
    if (!lease || lease->mode == OCT_NATIVE_LEASE_NONE) return OCT_NATIVE_LEASE_NONE;
    if (!lease->deadline_ms || now_ms >= lease->deadline_ms) return OCT_NATIVE_LEASE_NONE;
    /* Further ahead than the app is allowed to grant: a clock anomaly or a forged word. */
    if (lease->deadline_ms - now_ms > OCT_NATIVE_LEASE_MS) return OCT_NATIVE_LEASE_NONE;
    /* An unscoped request for real files names no root, so there is nothing to serve: it grants
     * nothing at all rather than being downgraded to something lesser. Decided here so that no
     * caller can forget to. */
    if (lease->mode == OCT_NATIVE_LEASE_EXPOSE_FILESYSTEM) return OCT_NATIVE_LEASE_NONE;
    return (oct_native_lease_mode)lease->mode;
}


uint64_t oct_native_runtime_pack(const oct_native_runtime *runtime)
{
    if (!runtime) return 0;
    uint64_t word = (uint64_t)(OCT_NATIVE_RUNTIME_VERSION & 0xFu)
                  | (uint64_t)(runtime->mode & 0xFu) << 4
                  | (uint64_t)(runtime->flags & 0x1Fu) << 8
                  | (uint64_t)runtime->commands << 13
                  | (uint64_t)runtime->responses << 29
                  | (uint64_t)runtime->provider_events << 45;
    return word | (uint64_t)lease_check(word) << 56;
}

bool oct_native_runtime_unpack(uint64_t word, oct_native_runtime *out)
{
    if (!out) return false;
    memset(out, 0, sizeof *out);
    if (!word || (word & 0xFu) != OCT_NATIVE_RUNTIME_VERSION) return false;
    if ((uint8_t)(word >> 56) != lease_check(word & 0x00FFFFFFFFFFFFFFull)) return false;
    out->mode = (uint8_t)((word >> 4) & 0xFu);
    out->flags = (uint8_t)((word >> 8) & 0x1Fu);
    out->commands = (uint16_t)((word >> 13) & 0xFFFFu);
    out->responses = (uint16_t)((word >> 29) & 0xFFFFu);
    out->provider_events = (uint8_t)((word >> 45) & 0xFFu);
    if (out->mode > 2) return false;
    return true;
}

uint64_t oct_native_install_pack(const oct_native_install_report *r)
{
    if (!r) return 0;
    uint64_t word = (uint64_t)OCT_NATIVE_INSTALL_VERSION
                  | (uint64_t)r->installed << 4
                  | (uint64_t)r->attempted << 12
                  | (uint64_t)r->first_failure << 20
                  | (uint64_t)(r->reason & 0xFu) << 28;
    return word | (uint64_t)lease_check(word) << 56;
}

bool oct_native_install_unpack(uint64_t word, oct_native_install_report *out)
{
    if (!out) return false;
    memset(out, 0, sizeof *out);
    if ((word & 0xFu) != OCT_NATIVE_INSTALL_VERSION) return false;
    if ((uint8_t)(word >> 56) != lease_check(word & 0x00FFFFFFFFFFFFFFull)) return false;
    out->installed = (uint8_t)(word >> 4);
    out->attempted = (uint8_t)(word >> 12);
    out->first_failure = (uint8_t)(word >> 20);
    out->reason = (uint8_t)((word >> 28) & 0xFu);
    return true;
}

const char *oct_native_installed_name(unsigned bit)
{
    switch (bit) {
    case OCT_NATIVE_INSTALLED_DELETE: return "delete";
    case OCT_NATIVE_INSTALLED_UPLOAD_REQUEST: return "upload request";
    case OCT_NATIVE_INSTALLED_UPLOAD_DATA: return "upload data";
    case OCT_NATIVE_INSTALLED_INCOMING_INFO: return "incoming ObjectInfo";
    case OCT_NATIVE_INSTALLED_APPEND_STORAGE: return "storage append";
    case OCT_NATIVE_INSTALLED_APPEND_FOLDER: return "folder append";
    }
    return "unknown";
}

static const char *install_reason_name(uint8_t reason)
{
    switch (reason) {
    case OCT_NATIVE_INSTALL_NO_CLASS: return "this ptpd has no such class";
    case OCT_NATIVE_INSTALL_NO_METHOD: return "the class has no such method";
    case OCT_NATIVE_INSTALL_ENCODING: return "the method's type differs from the recorded one";
    case OCT_NATIVE_INSTALL_EXCHANGE: return "the implementation could not be exchanged";
    case OCT_NATIVE_INSTALL_UNKNOWN: return "the runtime did not say why";
    case OCT_NATIVE_INSTALL_REPLACED: return "another hook already replaced the method";
    case OCT_NATIVE_INSTALL_NOT_ROUTED: return "the observer's request hook is not in place";
    }
    return "no failure";
}

void oct_native_install_describe(const oct_native_install_report *r, char *out, size_t capacity)
{
    if (!out || capacity == 0) return;
    if (!r || r->attempted == 0) { snprintf(out, capacity, "No optional hooks were attempted."); return; }
    char installed[160] = "", missing[160] = "";
    size_t i_used = 0, m_used = 0;
    for (unsigned bit = 1; bit <= (unsigned)OCT_NATIVE_INSTALLED_APPEND_FOLDER; bit <<= 1) {
        if (!(r->attempted & bit)) continue;
        const char *name = oct_native_installed_name(bit);
        if (r->installed & bit) {
            if (i_used < sizeof installed)
                i_used += (size_t)snprintf(installed + i_used, sizeof installed - i_used,
                                           "%s%s", i_used ? ", " : "", name);
        } else if (m_used < sizeof missing) {
            m_used += (size_t)snprintf(missing + m_used, sizeof missing - m_used,
                                       "%s%s", m_used ? ", " : "", name);
        }
    }
    /* The upload pair is what matters most: without both, no upload can be taken over at all. */
    const unsigned pair = OCT_NATIVE_INSTALLED_UPLOAD_REQUEST | OCT_NATIVE_INSTALLED_UPLOAD_DATA;
    const char *verdict = (r->installed & pair) == pair ? " Uploads can be received."
                        : " Uploads cannot be received without both upload hooks.";
    if (m_used == 0) {
        snprintf(out, capacity, "Installed: %s.%s", installed, verdict);
    } else {
        snprintf(out, capacity, "Installed: %s. Not installed: %s (%s).%s",
                 i_used ? installed : "none", missing,
                 install_reason_name(r->reason), verdict);
    }
}

uint64_t oct_native_traffic_pack(const oct_native_traffic *t)
{
    if (!t) return 0;
    uint64_t word = (uint64_t)OCT_NATIVE_TRAFFIC_VERSION
                  | (uint64_t)t->seen << 4
                  | (uint64_t)t->send_info_count << 12
                  | (uint64_t)t->send_info_response << 20
                  | (uint64_t)(t->send_object_response & 0xFFu) << 36
                  | (uint64_t)(t->delete_response & 0xFFu) << 44
                  | (uint64_t)(t->announced & 0xFu) << 52;
    return word | (uint64_t)lease_check(word) << 56;
}

bool oct_native_traffic_unpack(uint64_t word, oct_native_traffic *out)
{
    if (!out) return false;
    memset(out, 0, sizeof *out);
    if ((word & 0xFu) != OCT_NATIVE_TRAFFIC_VERSION) return false;
    if ((uint8_t)(word >> 56) != lease_check(word & 0x00FFFFFFFFFFFFFFull)) return false;
    out->seen = (uint8_t)(word >> 4);
    out->send_info_count = (uint8_t)(word >> 12);
    out->send_info_response = (uint16_t)(word >> 20);
    /* Only the low byte travels for these two; every response they can draw is a PTP one, 0x20xx. */
    uint8_t object = (uint8_t)(word >> 36), removed = (uint8_t)(word >> 44);
    out->send_object_response = object ? (uint16_t)(0x2000u | object) : 0;
    out->delete_response = removed ? (uint16_t)(0x2000u | removed) : 0;
    out->announced = (uint8_t)((word >> 52) & 0xFu);
    return true;
}

const char *oct_native_response_name(uint16_t response)
{
    switch (response) {
    case 0x2001: return "OK";
    case 0x2002: return "GeneralError";
    case 0x2005: return "OperationNotSupported";
    case 0x2008: return "InvalidStorageID";
    case 0x2009: return "InvalidObjectHandle";
    case 0x200B: return "InvalidObjectFormatCode";
    case 0x200C: return "StoreFull";
    case 0x200D: return "ObjectWriteProtected";
    case 0x200E: return "StoreReadOnly";
    case 0x200F: return "AccessDenied";
    case 0x2013: return "StoreNotAvailable";
    case 0x2019: return "DeviceBusy";
    case 0x201A: return "InvalidParentObject";
    case 0x201D: return "InvalidParameter";
    }
    return "unrecognised";
}

void oct_native_traffic_describe(const oct_native_traffic *t, char *out, size_t capacity)
{
    if (!out || capacity == 0) return;
    if (!t) { snprintf(out, capacity, "No write traffic recorded."); return; }
    bool info = t->seen & OCT_NATIVE_TRAFFIC_SEND_OBJECT_INFO;
    bool list = t->seen & OCT_NATIVE_TRAFFIC_SEND_PROP_LIST;
    char head[200];
    if (!info && !list) {
        /* The decisive case: the host never tried, so nothing on the phone could have refused it. */
        snprintf(head, sizeof head, "The computer never asked to add a file or folder: it decided "
                                    "from what the device reported not to offer it.");
    } else if (info && t->send_info_response == 0) {
        /* No response recorded at all: not a refusal but a request still waiting. On 0.4.67 the
         * first upload Windows ever attempted stalled with "Copying..." and no progress, and this
         * case was reported as an answer of 0x0000, which ptpd never sent. */
        snprintf(head, sizeof head, "The computer asked to add something %u time%s, and ptpd has not "
                 "answered: the request is waiting inside Apple's responder.",
                 (unsigned)t->send_info_count, t->send_info_count == 1 ? "" : "s");
    } else if (info && t->send_info_response != 0x2001) {
        snprintf(head, sizeof head, "The computer asked to add something %u time%s, and ptpd answered "
                 "0x%04X %s before Octopus was asked.", (unsigned)t->send_info_count,
                 t->send_info_count == 1 ? "" : "s", (unsigned)t->send_info_response,
                 oct_native_response_name(t->send_info_response));
    } else if (info && (t->seen & OCT_NATIVE_TRAFFIC_SEND_OBJECT) && t->send_object_response == 0) {
        snprintf(head, sizeof head, "ptpd accepted the name of an upload, and the computer sent its "
                 "data, but ptpd has not answered it.");
    } else if (info && t->send_object_response && t->send_object_response != 0x2001) {
        snprintf(head, sizeof head, "ptpd accepted the name of an upload, then answered its data "
                 "0x%04X %s.", (unsigned)t->send_object_response,
                 oct_native_response_name(t->send_object_response));
    } else if (info) {
        snprintf(head, sizeof head, "The computer added something %u time%s and ptpd accepted it.",
                 (unsigned)t->send_info_count, t->send_info_count == 1 ? "" : "s");
    } else {
        snprintf(head, sizeof head, "The computer tried the MTP way of adding a file, "
                                    "SendObjectPropList, which ptpd does not support.");
    }
    char tail[160] = "";
    size_t used = 0;
    if (t->seen & OCT_NATIVE_TRAFFIC_DELETE_OBJECT)
        used += (size_t)snprintf(tail + used, sizeof tail - used, " Delete answered 0x%04X %s.",
                                 (unsigned)t->delete_response, oct_native_response_name(t->delete_response));
    if ((t->seen & OCT_NATIVE_TRAFFIC_SET_PROP_VALUE) && used < sizeof tail)
        used += (size_t)snprintf(tail + used, sizeof tail - used, " A rename was attempted.");
    if ((t->seen & OCT_NATIVE_TRAFFIC_PROPS_SUPPORTED) && used < sizeof tail)
        used += (size_t)snprintf(tail + used, sizeof tail - used, " The computer probed for MTP properties.");
    /* Whether a host already connected was told anything changed. */
    if (used < sizeof tail) {
        const char *how = (t->announced & OCT_NATIVE_ANNOUNCE_CAMERA) ? " The drive was announced through Apple's own method"
                        : (t->announced & OCT_NATIVE_ANNOUNCE_EVENT) ? " The drive was announced by event"
                        : (t->announced & OCT_NATIVE_ANNOUNCE_FAILED) ? " The drive could not be announced: no session was up"
                        : " The drive was never announced";
        used += (size_t)snprintf(tail + used, sizeof tail - used, "%s%s.", how,
                                 (t->announced & OCT_NATIVE_ANNOUNCE_DEVICE_INFO)
                                     ? ", and the USB transport accepted DeviceInfoChanged" : "");
        (void)used;
    }
    snprintf(out, capacity, "%s%s", head, tail);
}

const char *oct_native_write_name(unsigned bit)
{
    switch (bit) {
    case OCT_NATIVE_WRITE_DELETE_HOOKED: return "delete hooked";
    case OCT_NATIVE_WRITE_UPLOAD_HOOKED: return "upload hooked";
    case OCT_NATIVE_WRITE_STORE_WRITABLE: return "store writable at DeviceInfo";
    case OCT_NATIVE_WRITE_ADVERTISED: return "write operations advertised";
    case OCT_NATIVE_WRITE_NAMED: return "upload named";
    case OCT_NATIVE_WRITE_RECEIVED: return "upload received";
    case OCT_NATIVE_WRITE_STORED: return "upload written";
    case OCT_NATIVE_WRITE_DELETED: return "object deleted";
    }
    return "unknown";
}

void oct_native_write_verdict(const oct_native_hook_report *report, char *out, size_t capacity)
{
    if (!out || capacity == 0) return;
    uint8_t writes = report ? report->writes : 0;
    bool early = report && report->early_device_info;
    const char *verdict;
    bool deleteHooked = writes & OCT_NATIVE_WRITE_DELETE_HOOKED;
    bool uploadHooked = writes & OCT_NATIVE_WRITE_UPLOAD_HOOKED;
    if (writes & OCT_NATIVE_WRITE_STORED) {
        verdict = "An upload was received and written.";
    } else if (writes & OCT_NATIVE_WRITE_RECEIVED) {
        verdict = "An upload's bytes arrived but were not written: the store refused them.";
    } else if (writes & OCT_NATIVE_WRITE_NAMED) {
        verdict = "A computer named an upload, but its bytes never reached Octopus.";
    } else if (!deleteHooked && !uploadHooked) {
        verdict = "No write hooks installed: this ptpd lacks the methods they need.";
    } else if (early && !(writes & OCT_NATIVE_WRITE_STORE_WRITABLE)) {
        /* Not a store that was read-only: a host that asked before Octopus was serving at all. */
        verdict = "The computer read the device's capabilities before Octopus was serving, so it was "
                  "told nothing can be written. DeviceInfoChanged has to make it read them again, or "
                  "reconnect USB after enabling.";
    } else if (!(writes & OCT_NATIVE_WRITE_STORE_WRITABLE)) {
        /* The timing fault: the computer asked before the store was on the granted root. */
        verdict = "The store was read-only when the computer read the device's capabilities, so it "
                  "was never offered writes. Reconnect USB.";
    } else if (!(writes & OCT_NATIVE_WRITE_ADVERTISED)) {
        verdict = "The store was writable, but no write operation reached the computer's list.";
    } else if (!uploadHooked) {
        verdict = "Delete was offered; uploads are not hooked in this ptpd.";
    } else {
        verdict = "Uploads were offered, but none reached Octopus: try adding a file.";
    }
    snprintf(out, capacity, "%s%s", verdict,
             (writes & OCT_NATIVE_WRITE_DELETED) ? " A delete was served." : "");
}

const char *oct_native_hook_name(unsigned bit)
{
    switch (bit) {
    case OCT_NATIVE_HOOK_DEVICE_INFO: return "deviceInfoDataset";
    case OCT_NATIVE_HOOK_STORAGE_IDS: return "allStorageIDs";
    case OCT_NATIVE_HOOK_STORAGE_INFO: return "storageInfoDataset";
    case OCT_NATIVE_HOOK_OBJECT_INFO: return "objectInfoDataset";
    case OCT_NATIVE_HOOK_HANDLES: return "objectHandlesInStorage";
    case OCT_NATIVE_HOOK_BULK_INFO: return "objectInfoForObjectsInStorage";
    case OCT_NATIVE_HOOK_DATA_SOURCE: return "dataSourceForObjectHandle";
    case OCT_NATIVE_HOOK_PROP_VALUE: return "objectPropValue";
    case OCT_NATIVE_HOOK_PROP_LIST: return "objectPropList";
    case OCT_NATIVE_HOOK_PROPS_SUPPORTED: return "objectPropertiesSupported";
    }
    return "unknown";
}

uint64_t oct_native_hooks_pack(const oct_native_hook_report *report)
{
    if (!report) return 0;
    uint64_t word = (uint64_t)(OCT_NATIVE_HOOKS_VERSION & 0xFu)
                  | (uint64_t)(report->fired & 0x3FFu) << 4
                  | (uint64_t)(report->formats_added ? 1u : 0u) << 14
                  | (uint64_t)report->device_info << 16
                  | (uint64_t)report->handles << 24
                  | (uint64_t)report->object_info << 32
                  | (uint64_t)report->get_object << 40
                  | (uint64_t)report->writes << 48
                  | (uint64_t)(report->early_device_info ? 1u : 0u) << 15;
    return word | (uint64_t)lease_check(word) << 56;
}

bool oct_native_hooks_unpack(uint64_t word, oct_native_hook_report *out)
{
    if (!out) return false;
    memset(out, 0, sizeof *out);
    if (!word || (word & 0xFu) != OCT_NATIVE_HOOKS_VERSION) return false;
    if ((uint8_t)(word >> 56) != lease_check(word & 0x00FFFFFFFFFFFFFFull)) return false;
    out->fired = (uint16_t)((word >> 4) & 0x3FFu);
    out->formats_added = ((word >> 14) & 1u) != 0;
    out->device_info = (uint8_t)(word >> 16);
    out->handles = (uint8_t)(word >> 24);
    out->object_info = (uint8_t)(word >> 32);
    out->get_object = (uint8_t)(word >> 40);
    out->writes = (uint8_t)(word >> 48);
    out->early_device_info = (word >> 15) & 1u;
    return true;
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
            "the system is not iOS 17.0.0", "ptpd's UUID differs", "ptpd is not arm64e",
            "ptpd's SHA-256 differs", "no loaded image is /usr/libexec/ptpd",
            "more than one loaded image claims to be /usr/libexec/ptpd",
            "the image is loaded but dyld reported no header for it",
            "the image's header is not a thin 64-bit Mach-O, or is too large to read",
            "the image's load commands did not parse",
            "the image at that path is a library, not the program",
            "/usr/libexec/ptpd could not be read to check its digest" };
        snprintf(text, capacity, "The companion was loaded into ptpd but stopped itself: %s. Nothing was hooked.",
                 s->identity < sizeof why / sizeof why[0] ? why[s->identity] : "unknown");
        return OCT_NATIVE_VERDICT_IDENTITY;
    }
    /* Hooks installed and a current lease means the companion is working, whatever the status file
     * did: that file is written into a folder ptpd's sandbox refuses, and everything it used to
     * carry now travels by notification. Reporting its refusal as the headline made a companion
     * that had reached every gate read as a failure, which is what the 10 November run showed.
     * The refusal is still reported, as a note rather than a verdict. */
    if (s->phase >= OCT_NATIVE_PHASE_HOOKED && s->hooks == OCT_NATIVE_HOOKS_INSTALLED) {
        snprintf(text, capacity, "The companion is running in ptpd with observer hooks installed.%s",
                 s->status_stage != OCT_NATIVE_IO_OK && s->status_stage != OCT_NATIVE_IO_NOT_ATTEMPTED
                     ? " Its status file is refused by ptpd's sandbox, which no longer matters: the"
                       " same information arrives by notification."
                     : "");
        return OCT_NATIVE_VERDICT_RUNNING;
    }
    /* Only the lease being refused can still block the companion; the status file cannot. */
    if (sandbox_like(s->lease_stage, s->lease_error)) {
        snprintf(text, capacity, "The companion is loaded and compatible, but ptpd's sandbox refused the shared "
                 "folder when reading the lease: %s, errno %u. The same uid does not grant the same sandbox "
                 "rights. Authorization should be arriving by notification instead; if this persists, that "
                 "channel is not reaching ptpd.",
                 oct_native_io_stage_name((oct_native_io_stage)s->lease_stage), (unsigned)s->lease_error);
        return OCT_NATIVE_VERDICT_SANDBOX;
    }
    if (s->phase < OCT_NATIVE_PHASE_LEASE) {
        if (s->lease_stage == OCT_NATIVE_IO_NOT_ATTEMPTED) {
            snprintf(text, capacity, "The companion is loaded and compatible, but has not yet tried to read an "
                     "authorization. Keep Octopus open with USB Access enabled.");
        } else {
            snprintf(text, capacity, "The companion is loaded and compatible, and waiting for authorization: the "
                     "last attempt ended at %s (errno %u). Keep Octopus open with USB Access enabled.",
                     oct_native_io_stage_name((oct_native_io_stage)s->lease_stage), (unsigned)s->lease_error);
        }
        return OCT_NATIVE_VERDICT_NO_LEASE;
    }
    if (s->status_stage != OCT_NATIVE_IO_OK && s->status_stage != OCT_NATIVE_IO_NOT_ATTEMPTED) {
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

/* One helper for the two extra names; each keeps its token for the life of the process, as the
 * status name does, because notifyd holds a name's state while a registration is open. */
static oct_status notify_write(const char *name, int *token, uint64_t word)
{
    if (*token < 0 && notify_register_check(name, token) != NOTIFY_STATUS_OK) {
        *token = -1;
        return OCT_E_IO;
    }
    if (notify_set_state(*token, word) != NOTIFY_STATUS_OK) return OCT_E_IO;
    return notify_post(name) == NOTIFY_STATUS_OK ? OCT_OK : OCT_E_IO;
}

static oct_status notify_read(const char *name, uint64_t *word)
{
    int token = -1;
    if (notify_register_check(name, &token) != NOTIFY_STATUS_OK) return OCT_E_IO;
    uint32_t status = notify_get_state(token, word);
    notify_cancel(token);
    return status == NOTIFY_STATUS_OK ? OCT_OK : OCT_E_IO;
}

oct_status oct_native_runtime_check(bool *posted)
{
    /* Held for the life of the process: notify_check reports posts since this token last looked,
     * so a token made per call would report every post forever. */
    static int token = -1;
    if (!posted) return OCT_E_INVALID_ARGUMENT;
    *posted = false;
    if (token < 0) {
        if (notify_register_check(OCT_NATIVE_RUNTIME_NAME, &token) != NOTIFY_STATUS_OK) {
            token = -1;
            return OCT_E_IO;
        }
        /* notify_check reports a post on its first call whatever has happened since, so without
         * this a state left behind by a ptpd that has already exited would read as a heartbeat,
         * and for one window after launch an unplugged phone would show a live session. The first
         * answer is consumed here, so only a post that arrives after this point counts. */
        int primed = 0;
        (void)notify_check(token, &primed);
    }
    int check = 0;
    if (notify_check(token, &check) != NOTIFY_STATUS_OK) return OCT_E_IO;
    *posted = check != 0;
    return OCT_OK;
}

oct_status oct_native_detail_publish(const oct_native_detail *detail)
{
    static int token = -1;
    if (!detail) return OCT_E_INVALID_ARGUMENT;
    return notify_write(OCT_NATIVE_DETAIL_NAME, &token, oct_native_detail_pack(detail));
}

oct_status oct_native_detail_read(oct_native_detail *out)
{
    if (!out) return OCT_E_INVALID_ARGUMENT;
    memset(out, 0, sizeof *out);
    uint64_t word = 0;
    oct_status status = notify_read(OCT_NATIVE_DETAIL_NAME, &word);
    if (status != OCT_OK) return status;
    return oct_native_detail_unpack(word, out) ? OCT_OK : OCT_E_STATE;
}

oct_status oct_native_lease_publish(const oct_native_lease_word *lease)
{
    static int token = -1;
    if (!lease) return OCT_E_INVALID_ARGUMENT;
    return notify_write(OCT_NATIVE_LEASE_NAME, &token, oct_native_lease_pack(lease));
}

oct_status oct_native_runtime_publish(const oct_native_runtime *runtime)
{
    static int token = -1;
    if (!runtime) return OCT_E_INVALID_ARGUMENT;
    return notify_write(OCT_NATIVE_RUNTIME_NAME, &token, oct_native_runtime_pack(runtime));
}

oct_status oct_native_install_publish(const oct_native_install_report *report)
{
    static int token = -1;
    if (!report) return OCT_E_INVALID_ARGUMENT;
    return notify_write(OCT_NATIVE_INSTALL_NAME, &token, oct_native_install_pack(report));
}

oct_status oct_native_install_read(oct_native_install_report *out)
{
    if (!out) return OCT_E_INVALID_ARGUMENT;
    memset(out, 0, sizeof *out);
    uint64_t word = 0;
    oct_status status = notify_read(OCT_NATIVE_INSTALL_NAME, &word);
    if (status != OCT_OK) return status;
    return oct_native_install_unpack(word, out) ? OCT_OK : OCT_E_STATE;
}

oct_status oct_native_traffic_publish(const oct_native_traffic *traffic)
{
    static int token = -1;
    if (!traffic) return OCT_E_INVALID_ARGUMENT;
    return notify_write(OCT_NATIVE_TRAFFIC_NAME, &token, oct_native_traffic_pack(traffic));
}

oct_status oct_native_traffic_read(oct_native_traffic *out)
{
    if (!out) return OCT_E_INVALID_ARGUMENT;
    memset(out, 0, sizeof *out);
    uint64_t word = 0;
    oct_status status = notify_read(OCT_NATIVE_TRAFFIC_NAME, &word);
    if (status != OCT_OK) return status;
    return oct_native_traffic_unpack(word, out) ? OCT_OK : OCT_E_STATE;
}

oct_status oct_native_hooks_publish(const oct_native_hook_report *report)
{
    static int token = -1;
    if (!report) return OCT_E_INVALID_ARGUMENT;
    return notify_write(OCT_NATIVE_HOOKS_NAME, &token, oct_native_hooks_pack(report));
}

oct_status oct_native_roots_publish(const oct_native_root_report *report)
{
    static int token = -1;
    if (!report) return OCT_E_INVALID_ARGUMENT;
    return notify_write(OCT_NATIVE_ROOTS_NAME, &token, oct_native_roots_pack(report));
}

oct_status oct_native_roots_read(oct_native_root_report *out)
{
    if (!out) return OCT_E_INVALID_ARGUMENT;
    memset(out, 0, sizeof *out);
    uint64_t word = 0;
    oct_status status = notify_read(OCT_NATIVE_ROOTS_NAME, &word);
    if (status != OCT_OK) return status;
    return oct_native_roots_unpack(word, out) ? OCT_OK : OCT_E_STATE;
}

oct_status oct_native_hooks_read(oct_native_hook_report *out)
{
    if (!out) return OCT_E_INVALID_ARGUMENT;
    memset(out, 0, sizeof *out);
    uint64_t word = 0;
    oct_status status = notify_read(OCT_NATIVE_HOOKS_NAME, &word);
    if (status != OCT_OK) return status;
    return oct_native_hooks_unpack(word, out) ? OCT_OK : OCT_E_STATE;
}

oct_status oct_native_runtime_read(oct_native_runtime *out)
{
    if (!out) return OCT_E_INVALID_ARGUMENT;
    memset(out, 0, sizeof *out);
    uint64_t word = 0;
    oct_status status = notify_read(OCT_NATIVE_RUNTIME_NAME, &word);
    if (status != OCT_OK) return status;
    return oct_native_runtime_unpack(word, out) ? OCT_OK : OCT_E_STATE;
}

oct_status oct_native_lease_read(oct_native_lease_word *out)
{
    if (!out) return OCT_E_INVALID_ARGUMENT;
    memset(out, 0, sizeof *out);
    uint64_t word = 0;
    oct_status status = notify_read(OCT_NATIVE_LEASE_NAME, &word);
    if (status != OCT_OK) return status;
    return oct_native_lease_unpack(word, out) ? OCT_OK : OCT_E_STATE;
}
#else
oct_status oct_native_signal_publish(const oct_native_signal *s) { (void)s; return OCT_E_UNSUPPORTED; }
oct_status oct_native_signal_read(oct_native_signal *out)
{
    if (out) memset(out, 0, sizeof *out);
    return OCT_E_UNSUPPORTED;
}
oct_status oct_native_detail_publish(const oct_native_detail *detail)
{
    (void)detail;
    return OCT_E_UNSUPPORTED;
}
oct_status oct_native_lease_publish(const oct_native_lease_word *lease)
{
    (void)lease;
    return OCT_E_UNSUPPORTED;
}
oct_status oct_native_runtime_publish(const oct_native_runtime *runtime)
{
    (void)runtime;
    return OCT_E_UNSUPPORTED;
}
oct_status oct_native_hooks_publish(const oct_native_hook_report *report)
{
    (void)report;
    return OCT_E_UNSUPPORTED;
}
oct_status oct_native_traffic_publish(const oct_native_traffic *traffic)
{
    (void)traffic;
    return OCT_E_UNSUPPORTED;
}
oct_status oct_native_install_publish(const oct_native_install_report *report)
{
    (void)report;
    return OCT_E_UNSUPPORTED;
}
oct_status oct_native_install_read(oct_native_install_report *out)
{
    if (out) memset(out, 0, sizeof *out);
    return OCT_E_UNSUPPORTED;
}
oct_status oct_native_traffic_read(oct_native_traffic *out)
{
    if (out) memset(out, 0, sizeof *out);
    return OCT_E_UNSUPPORTED;
}
oct_status oct_native_roots_publish(const oct_native_root_report *report)
{
    (void)report;
    return OCT_E_UNSUPPORTED;
}
oct_status oct_native_roots_read(oct_native_root_report *out)
{
    if (out) memset(out, 0, sizeof *out);
    return OCT_E_UNSUPPORTED;
}
oct_status oct_native_hooks_read(oct_native_hook_report *out)
{
    if (out) memset(out, 0, sizeof *out);
    return OCT_E_UNSUPPORTED;
}
oct_status oct_native_runtime_read(oct_native_runtime *out)
{
    if (out) memset(out, 0, sizeof *out);
    return OCT_E_UNSUPPORTED;
}
oct_status oct_native_runtime_check(bool *posted)
{
    /* No notifyd here, so nothing is ever heard: the transport reads as absent. */
    if (posted) *posted = false;
    return OCT_E_UNSUPPORTED;
}
oct_status oct_native_lease_read(oct_native_lease_word *out)
{
    if (out) memset(out, 0, sizeof *out);
    return OCT_E_UNSUPPORTED;
}
oct_status oct_native_detail_read(oct_native_detail *out)
{
    if (out) memset(out, 0, sizeof *out);
    return OCT_E_UNSUPPORTED;
}
#endif

/* ---- Real filesystem roots ----------------------------------------------------------------- */

const char *oct_native_root_path(oct_native_root root)
{
    switch (root) {
    case OCT_NATIVE_ROOT_SYNTHETIC: return NULL;
    case OCT_NATIVE_ROOT_MEDIA: return "/var/mobile/Media";
    case OCT_NATIVE_ROOT_DCIM: return "/var/mobile/Media/DCIM";
    case OCT_NATIVE_ROOT_MOBILE: return "/var/mobile";
    case OCT_NATIVE_ROOT_TMP: return "/var/tmp";
    case OCT_NATIVE_ROOT_SYSTEM: return "/";
    case OCT_NATIVE_ROOT_COUNT: break;
    }
    return NULL;
}

oct_status oct_native_probe_root(const char *path, int *error)
{
    if (error) *error = 0;
    if (!path || !*path || path[0] != '/') {
        if (error) *error = EINVAL;
        return OCT_E_INVALID_ARGUMENT;
    }
    /* Symlinks are followed, because the backend now follows the ones whose target is inside the
     * served root. Refusing them here reported our own refusal as a sandbox denial: /var/mobile
     * came back "refused by the sandbox" when the kernel had never been asked, since /var is a
     * symlink on iOS. The backend still verifies containment itself; this only answers whether the
     * path can be opened and read at all. */
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        if (error) *error = errno;
        return OCT_E_IO;
    }
    /* Opening a directory and reading from it are different permissions under a sandbox, so the
     * second is checked separately rather than assumed from the first. */
    DIR *dir = fdopendir(fd);
    if (!dir) {
        int saved = errno;
        close(fd);
        if (error) *error = saved;
        return OCT_E_IO;
    }
    errno = 0;
    struct dirent *entry;
    do {
        entry = readdir(dir);
    } while (!entry && errno == EINTR);
    int saved = entry ? 0 : errno;
    closedir(dir); /* also closes fd */
    if (!entry && saved) {
        if (error) *error = saved;
        return OCT_E_IO;
    }
    /* An empty directory read cleanly is still a readable directory. */
    return OCT_OK;
}

void oct_native_probe_all_roots(oct_native_root_report *report)
{
    if (!report) return;
    memset(report, 0, sizeof *report);
    for (unsigned i = 1; i < OCT_NATIVE_ROOT_COUNT; i++) {
        const char *path = oct_native_root_path((oct_native_root)i);
        if (!path) continue;
        int error = 0;
        oct_status status = oct_native_probe_root(path, &error);
        uint8_t bit = (uint8_t)(1u << i);
        if (status == OCT_OK) {
            report->openable |= bit;
            report->listable |= bit;
        } else if (error == EPERM || error == EACCES) {
            report->refused |= bit;
        }
    }
}

oct_status oct_native_probe_root_writable(const char *path, int *error)
{
    if (error) *error = 0;
    if (!path || !*path) { if (error) *error = EINVAL; return OCT_E_INVALID_ARGUMENT; }
    /* Hidden, and unique to this process and call, so it cannot meet a real file or another probe. */
    static unsigned counter;
    char probe[1024];
    int written = snprintf(probe, sizeof probe, "%s%s.octopus-write-probe.%ld.%u", path,
                           path[strlen(path) - 1] == '/' ? "" : "/", (long)getpid(), ++counter);
    if (written < 0 || (size_t)written >= sizeof probe) { if (error) *error = ENAMETOOLONG; return OCT_E_INVALID_ARGUMENT; }
    int fd = open(probe, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) { if (error) *error = errno; return OCT_E_IO; }
    close(fd);
    if (unlink(probe) != 0) { if (error) *error = errno; return OCT_E_IO; }
    return OCT_OK;
}

void oct_native_probe_all_roots_writable(oct_native_root_report *report)
{
    if (!report) return;
    report->creatable = report->create_refused = report->read_only = 0;
    report->write_probed = true;
    for (unsigned i = 1; i < OCT_NATIVE_ROOT_COUNT; i++) {
        const char *path = oct_native_root_path((oct_native_root)i);
        if (!path) continue;
        int error = 0;
        uint8_t bit = (uint8_t)(1u << i);
        if (oct_native_probe_root_writable(path, &error) == OCT_OK) report->creatable |= bit;
        else if (error == EPERM) report->create_refused |= bit;
        else if (error == EROFS) report->read_only |= bit;
    }
}

const char *oct_native_root_write_name(const oct_native_root_report *report, oct_native_root root)
{
    if (!report || (unsigned)root >= OCT_NATIVE_ROOT_COUNT) return "not probed";
    uint8_t bit = (uint8_t)(1u << (unsigned)root);
    if (report->creatable & bit) return "files can be created";
    if (report->create_refused & bit) return "writing refused by the sandbox";
    if (report->read_only & bit) return "read-only volume";
    if (!report->write_probed) return "not probed";
    return "not permitted";
}

uint64_t oct_native_roots_pack(const oct_native_root_report *report)
{
    if (!report) return 0;
    uint64_t word = (uint64_t)(OCT_NATIVE_ROOTS_VERSION & 0xFu)
                  | (uint64_t)report->openable << 8
                  | (uint64_t)report->listable << 16
                  | (uint64_t)report->refused << 24
                  | (uint64_t)report->creatable << 32
                  | (uint64_t)report->create_refused << 40
                  | (uint64_t)report->read_only << 48
                  | (uint64_t)(report->write_probed ? 1u : 0u) << 4;
    return word | (uint64_t)lease_check(word) << 56;
}

bool oct_native_roots_unpack(uint64_t word, oct_native_root_report *out)
{
    if (!out) return false;
    memset(out, 0, sizeof *out);
    if (!word || (word & 0xFu) != OCT_NATIVE_ROOTS_VERSION) return false;
    if ((uint8_t)(word >> 56) != lease_check(word & 0x00FFFFFFFFFFFFFFull)) return false;
    out->openable = (uint8_t)(word >> 8);
    out->listable = (uint8_t)(word >> 16);
    out->refused = (uint8_t)(word >> 24);
    out->creatable = (uint8_t)(word >> 32);
    out->create_refused = (uint8_t)(word >> 40);
    out->read_only = (uint8_t)(word >> 48);
    out->write_probed = (word >> 4) & 1u;
    return true;
}

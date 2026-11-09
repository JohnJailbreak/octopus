#ifndef OCT_NATIVE_IO_H
#define OCT_NATIVE_IO_H
#include "oct_store.h"
#define OCT_NATIVE_DIRECTORY "/var/mobile/Library/Caches/Octopus/native"
#define OCT_NATIVE_LEASE_MS 15000u
/* Both peers are mobile. This is not an authentication boundary against other
 * processes with the same UID. Files contain no filenames or file contents. */
oct_status oct_native_io_read(const char *directory, const char *name, void *bytes, size_t capacity, size_t *length);
oct_status oct_native_io_write(const char *directory, const char *name, const void *bytes, size_t length);
oct_status oct_native_io_remove_lease(const char *directory);
oct_status oct_native_write_lease(const char *directory, const char *token, unsigned mode, uint64_t now_ms);
/* Exact canonical OCTN1 grammar; usable without filesystem access for protocol tests. */
unsigned oct_native_parse_lease(const void *bytes, size_t length, uint64_t now_ms, uint64_t *deadline, char token[33]);
unsigned oct_native_read_lease(const char *directory, uint64_t now_ms, uint64_t *deadline, char token[33]);

/* ---- Where a native I/O call failed ---------------------------------------------------------
 *
 * The companion runs inside ptpd, which is sandboxed by Apple's "ptpd" seatbelt profile, so a call
 * that works from the app can be refused there with the same uid. A bare OCT_E_IO cannot tell a
 * sandbox refusal from a missing file or a policy rejection, and the plain functions above lose
 * which step failed. These record the stage and the errno of the call that failed, captured
 * before any cleanup call can overwrite it.
 *
 * OPEN_DIRECTORY and VALIDATE_DIRECTORY are kept apart on purpose: open() refused by the kernel or
 * the sandbox, versus a directory that opened but is not private to this uid. Both used to surface
 * as EPERM. */
typedef enum oct_native_io_stage {
    OCT_NATIVE_IO_OK = 0,
    OCT_NATIVE_IO_ARGUMENT,           /* rejected before touching the filesystem */
    OCT_NATIVE_IO_CREATE_DIRECTORY,   /* mkdir */
    OCT_NATIVE_IO_OPEN_DIRECTORY,     /* open(directory) refused by the kernel or the sandbox */
    OCT_NATIVE_IO_VALIDATE_DIRECTORY, /* opened, but not a private directory owned by this uid */
    OCT_NATIVE_IO_OPEN_FILE,          /* openat(file) or creating the temporary */
    OCT_NATIVE_IO_VALIDATE_FILE,      /* opened, but not a private regular file owned by this uid */
    OCT_NATIVE_IO_READ,
    OCT_NATIVE_IO_WRITE,
    OCT_NATIVE_IO_FSYNC,
    OCT_NATIVE_IO_CLOSE,
    OCT_NATIVE_IO_RENAME,
    OCT_NATIVE_IO_CONTENT,            /* read fine, but the bytes are not a valid record */
} oct_native_io_stage;

typedef struct oct_native_io_failure {
    oct_native_io_stage stage; /* OCT_NATIVE_IO_OK when the call succeeded */
    int                 error; /* errno of the failing call; 0 on success */
} oct_native_io_failure;

const char *oct_native_io_stage_name(oct_native_io_stage stage);
oct_status oct_native_io_read_detailed(const char *directory, const char *name, void *bytes, size_t capacity,
                                       size_t *length, oct_native_io_failure *failure);
oct_status oct_native_io_write_detailed(const char *directory, const char *name, const void *bytes, size_t length,
                                        oct_native_io_failure *failure);
unsigned oct_native_read_lease_detailed(const char *directory, uint64_t now_ms, uint64_t *deadline, char token[33],
                                        oct_native_io_failure *failure);

/* ---- An independent startup signal ----------------------------------------------------------
 *
 * The status file cannot be the only way to learn that the companion ran, because writing it is
 * one of the things that may be failing. This 64-bit word is posted through Darwin notifications
 * (notifyd), which needs no filesystem access, so it survives a sandbox that refuses every path.
 * The app reads it back. Absent: the constructor never ran where it could post. Present: how far
 * startup got, and the exact stage and errno of the last lease read and status write.
 *
 * Layout, least significant first: version:4 phase:4 identity:4 hooks:4 lease_stage:4
 * status_stage:4 lease_error:8 status_error:8 pid:16 check:8, where check is the XOR of the other
 * seven bytes, so a foreign or corrupted word is rejected rather than misread. */
#define OCT_NATIVE_SIGNAL_NAME    "com.parv.octopus.native.signal"
#define OCT_NATIVE_SIGNAL_VERSION 1u

typedef enum oct_native_phase {
    OCT_NATIVE_PHASE_NONE = 0,
    OCT_NATIVE_PHASE_CONSTRUCTOR,  /* the dylib's constructor ran in this process */
    OCT_NATIVE_PHASE_IDENTITY,     /* the process identity check finished (see identity) */
    OCT_NATIVE_PHASE_TIMER,        /* the lease poll timer is running */
    OCT_NATIVE_PHASE_LEASE,        /* a lease was read and matched */
    OCT_NATIVE_PHASE_PUBLISHED,    /* the status file was written */
    OCT_NATIVE_PHASE_HOOKED,       /* the observer hooks are installed */
} oct_native_phase;

typedef enum oct_native_identity {
    OCT_NATIVE_IDENTITY_UNKNOWN = 0,
    OCT_NATIVE_IDENTITY_COMPATIBLE,
    OCT_NATIVE_IDENTITY_WRONG_PROCESS,
    OCT_NATIVE_IDENTITY_WRONG_SYSTEM,
    OCT_NATIVE_IDENTITY_WRONG_UUID,
    OCT_NATIVE_IDENTITY_WRONG_ARCHITECTURE,
    OCT_NATIVE_IDENTITY_WRONG_DIGEST,
} oct_native_identity;

typedef enum oct_native_hooks {
    OCT_NATIVE_HOOKS_NOT_ATTEMPTED = 0,
    OCT_NATIVE_HOOKS_INSTALLED,
    OCT_NATIVE_HOOKS_REFUSED,      /* an encoding or provenance check failed; nothing was hooked */
} oct_native_hooks;

typedef struct oct_native_signal {
    uint8_t  phase;        /* oct_native_phase: the furthest phase reached */
    uint8_t  identity;     /* oct_native_identity */
    uint8_t  hooks;        /* oct_native_hooks */
    uint8_t  lease_stage;  /* oct_native_io_stage of the last lease read */
    uint8_t  status_stage; /* oct_native_io_stage of the last status write */
    uint8_t  lease_error;  /* errno of the last lease read, clamped to 255 */
    uint8_t  status_error; /* errno of the last status write, clamped to 255 */
    uint16_t pid;          /* low 16 bits of the posting process id */
} oct_native_signal;

uint64_t   oct_native_signal_pack(const oct_native_signal *signal);
/* False for zero, another version, a field out of range, or a failed check byte. */
bool       oct_native_signal_unpack(uint64_t word, oct_native_signal *out);
/* Whether the signal came from `pid`; the low 16 bits are compared, which is enough to notice a
 * restarted ptpd, not to identify a process. */
bool       oct_native_signal_from_pid(const oct_native_signal *signal, int32_t pid);
/* Darwin only: post through notifyd. OCT_E_UNSUPPORTED elsewhere. */
oct_status oct_native_signal_publish(const oct_native_signal *signal);
/* Darwin only: OCT_E_STATE when nothing valid was ever posted since boot. */
oct_status oct_native_signal_read(oct_native_signal *out);

/* What a signal (or its absence) means, for the app's diagnostics. Each verdict names one cause,
 * so a single device run separates the hypotheses the v0.4.20 run could not. */
typedef enum oct_native_verdict {
    OCT_NATIVE_VERDICT_NO_SIGNAL = 0,  /* constructor never posted: not loaded, or notify refused */
    OCT_NATIVE_VERDICT_STALE,          /* posted by a ptpd that is no longer the running one */
    OCT_NATIVE_VERDICT_IDENTITY,       /* loaded, but the process identity check rejected it */
    OCT_NATIVE_VERDICT_SANDBOX,        /* loaded and compatible; the shared folder was refused */
    OCT_NATIVE_VERDICT_NO_LEASE,       /* loaded and compatible; no current lease to read */
    OCT_NATIVE_VERDICT_STATUS_FAILED,  /* a lease was read, but the status file was not written */
    OCT_NATIVE_VERDICT_HOOKS_REFUSED,  /* an encoding or provenance check stopped the hooks */
    OCT_NATIVE_VERDICT_RUNNING,        /* status written; hooks installed or not yet wanted */
} oct_native_verdict;

/* Fills `text` with one sentence; `observed_pid` is the running ptpd's pid, or 0 if unknown. */
oct_native_verdict oct_native_signal_describe(const oct_native_signal *signal, bool present,
                                              int32_t observed_pid, char *text, size_t capacity);

#endif

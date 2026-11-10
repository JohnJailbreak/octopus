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
    /* Never tried. Zero means success, so without this a field left at its default reads as
     * "succeeded": the 9 November capture reported the lease stage as ok when the lease had in
     * fact never been read, because identity rejection stopped the companion first. */
    OCT_NATIVE_IO_NOT_ATTEMPTED,
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
#define OCT_NATIVE_SIGNAL_VERSION 1u  /* what 0.4.24 posted; still accepted when reading */
#define OCT_NATIVE_SIGNAL_VERSION_2 2u /* adds the identity codes and stages above */

/* A second notification carrying what will not fit in the first. The status word is full at 64
 * bits, and the single most useful missing fact is the UUID the companion actually read, which is
 * 128 bits. This carries its first eight bytes, which is enough to tell "read ptpd's UUID" from
 * "read some other image's", plus the image index and how many images were enumerated. Absent is
 * not an error: the status word stands alone. */
#define OCT_NATIVE_DETAIL_NAME "com.parv.octopus.native.detail"

typedef struct oct_native_detail {
    uint8_t  uuid_prefix[6]; /* first six bytes of the UUID read, zero if none was */
    uint8_t  image_index;    /* index of the matching image, 255 when there was none */
    uint8_t  image_count;    /* images enumerated, saturating at 255 */
} oct_native_detail;

/* ---- Authorization carried by notification ---------------------------------------------
 *
 * The file the app writes its lease into cannot be read from inside ptpd: the 9 November run
 * measured EPERM opening that folder, from both ptpd processes, with the folder owned by the same
 * uid ptpd runs as. The notification channel is the one route proven to cross that boundary, so
 * authorization travels the same way.
 *
 * What this is NOT. A Darwin notification name is not an authenticated channel: any process on the
 * device can register the same name and set its state, and there is no shared secret to sign with,
 * because every place the two could keep one is either unreadable from ptpd or readable by anyone
 * who can post. Replay within the window is possible. So this word is treated as an unauthenticated
 * hint, and what a hint may switch on is limited accordingly:
 *
 *   - OBSERVE is accepted. It installs passive hooks that forward Apple's own flows unchanged and
 *     record operation codes, transaction numbers and counts. No filenames or payloads are kept,
 *     and nothing is served to the host.
 *   - EXPOSE_SYNTHETIC is accepted. What it exposes is the six-object tree this code builds in
 *     memory from constants: a demo text file and its folders. There is no user data in it, so a
 *     forged grant would show a connected host Octopus's own sample files and nothing else, which
 *     is a different thing from reading someone's phone. Apple's own storages are untouched.
 *   - EXPOSE_FILESYSTEM is refused from this channel, always, whatever the word says. Serving real
 *     files is where a forged grant would matter, and it waits for a channel whose sender can be
 *     established. The read-only backend is not connected to the live provider either way.
 *
 * The expiry is carried as milliseconds since boot so a word left behind by an earlier boot cannot
 * read as current, and is clamped to the 15 second lease the app already uses. The check byte
 * rejects a corrupted or foreign word; it is not a signature and does not pretend to be. */
#define OCT_NATIVE_LEASE_NAME    "com.parv.octopus.native.lease"
#define OCT_NATIVE_LEASE_VERSION 1u

typedef enum oct_native_lease_mode {
    OCT_NATIVE_LEASE_NONE = 0,
    OCT_NATIVE_LEASE_OBSERVE,           /* passive observation only */
    OCT_NATIVE_LEASE_EXPOSE_SYNTHETIC,  /* the built-in six-object tree */
    OCT_NATIVE_LEASE_EXPOSE_FILESYSTEM, /* real files: refused from this channel */
} oct_native_lease_mode;

typedef struct oct_native_lease_word {
    uint8_t  mode;        /* oct_native_lease_mode */
    uint64_t deadline_ms; /* milliseconds since boot, 40 bits, about 34 years */
    uint16_t generation;  /* changes when the app restarts, so a stale word is noticed */
} oct_native_lease_word;

uint64_t oct_native_lease_pack(const oct_native_lease_word *lease);
bool     oct_native_lease_unpack(uint64_t word, oct_native_lease_word *out);
/* What a reader at `now_ms` may act on: OBSERVE only while unexpired, NONE otherwise. EXPOSE is
 * downgraded to NONE here, not at the call site, so no caller can forget to. */
oct_native_lease_mode oct_native_lease_effective(const oct_native_lease_word *lease, uint64_t now_ms);

oct_status oct_native_lease_publish(const oct_native_lease_word *lease);
oct_status oct_native_lease_read(oct_native_lease_word *out);

/* ---- The companion's runtime state, without the blocked folder ----------------------------
 *
 * The status JSON is written into a folder ptpd cannot open, so the app could not learn whether
 * hooks are installed or whether the provider is eligible, and the synthetic-storage toggle, which
 * waits on exactly that, could never unlock. These counters travel the proven direction instead.
 * Bounded by construction: counts saturate rather than wrap, and nothing identifying a file or a
 * payload is carried, only how many commands and responses were seen.
 *
 * version:4 mode:4 flags:4 commands:16 responses:16 provider_events:8 check:8, 60 of 64 bits. */
#define OCT_NATIVE_RUNTIME_NAME    "com.parv.octopus.native.runtime"
#define OCT_NATIVE_RUNTIME_VERSION 1u

/* Named, like the other enums Swift reads: Swift imports an anonymous C enum's constants as plain
 * integers with no rawValue, so the app could not use these the way it uses every other one. */
typedef enum oct_native_runtime_flag {
    OCT_NATIVE_RUNTIME_HOOKED    = 1u << 0, /* observer hooks are installed */
    OCT_NATIVE_RUNTIME_ELIGIBLE  = 1u << 1, /* the provider's own checks passed */
    OCT_NATIVE_RUNTIME_SAW_IDS   = 1u << 2, /* a real GetStorageIDs arrived from a host */
    OCT_NATIVE_RUNTIME_SAW_INFO  = 1u << 3, /* a real GetStorageInfo arrived from a host */
} oct_native_runtime_flag;

typedef struct oct_native_runtime {
    uint8_t  mode;            /* 0 none, 1 observing, 2 serving the synthetic tree */
    uint8_t  flags;           /* the OCT_NATIVE_RUNTIME_* bits above */
    uint16_t commands;        /* host commands seen, saturating at 65535 */
    uint16_t responses;       /* Apple responses paired with them, saturating */
    uint8_t  provider_events; /* provider calls served, saturating at 255 */
} oct_native_runtime;

uint64_t   oct_native_runtime_pack(const oct_native_runtime *runtime);
bool       oct_native_runtime_unpack(uint64_t word, oct_native_runtime *out);
oct_status oct_native_runtime_publish(const oct_native_runtime *runtime);
oct_status oct_native_runtime_read(oct_native_runtime *out);

uint64_t   oct_native_detail_pack(const oct_native_detail *detail);
bool       oct_native_detail_unpack(uint64_t word, oct_native_detail *out);
oct_status oct_native_detail_publish(const oct_native_detail *detail);
oct_status oct_native_detail_read(oct_native_detail *out);

typedef enum oct_native_phase {
    OCT_NATIVE_PHASE_NONE = 0,
    OCT_NATIVE_PHASE_CONSTRUCTOR,  /* the dylib's constructor ran in this process */
    OCT_NATIVE_PHASE_IDENTITY,     /* the process identity check finished (see identity) */
    OCT_NATIVE_PHASE_TIMER,        /* the lease poll timer is running */
    OCT_NATIVE_PHASE_LEASE,        /* a lease was read and matched */
    OCT_NATIVE_PHASE_PUBLISHED,    /* the status file was written */
    OCT_NATIVE_PHASE_HOOKED,       /* the observer hooks are installed */
} oct_native_phase;

/* One code per cause. The first seven are version 1's, unchanged, so a signal from 0.4.24 still
 * decodes; the rest are new in version 2 and separate the four conditions that version lumped
 * into WRONG_UUID. Fifteen values is the limit of the field. */
typedef enum oct_native_identity {
    OCT_NATIVE_IDENTITY_UNKNOWN = 0,
    OCT_NATIVE_IDENTITY_COMPATIBLE,
    OCT_NATIVE_IDENTITY_WRONG_PROCESS,
    OCT_NATIVE_IDENTITY_WRONG_SYSTEM,
    OCT_NATIVE_IDENTITY_WRONG_UUID,          /* a UUID was read and it differs */
    OCT_NATIVE_IDENTITY_WRONG_ARCHITECTURE,
    OCT_NATIVE_IDENTITY_WRONG_DIGEST,        /* the on-disk SHA-256 differs */
    OCT_NATIVE_IDENTITY_NO_IMAGE = 7,        /* no loaded image is /usr/libexec/ptpd */
    OCT_NATIVE_IDENTITY_AMBIGUOUS_IMAGE,     /* more than one claims that path */
    OCT_NATIVE_IDENTITY_NO_HEADER,           /* listed, but dyld gave no header */
    OCT_NATIVE_IDENTITY_BAD_HEADER,          /* not a thin 64-bit Mach-O, or oversized */
    OCT_NATIVE_IDENTITY_MALFORMED_HEADER,    /* load commands did not parse */
    OCT_NATIVE_IDENTITY_NOT_EXECUTABLE,      /* parsed, but not MH_EXECUTE */
    OCT_NATIVE_IDENTITY_UNREADABLE_FILE,     /* /usr/libexec/ptpd could not be read to digest */
    OCT_NATIVE_IDENTITY_LAST = OCT_NATIVE_IDENTITY_UNREADABLE_FILE,
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

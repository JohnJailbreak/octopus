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
    OCT_NATIVE_LEASE_EXPOSE_FILESYSTEM, /* unscoped: never honoured, no root to serve */
    /* Real files, read-only, under one of the roots in oct_native_root. The word has no room for a
     * path, so a grant selects from that table by number and no path travels over the channel.
     * Mode 4 is oct_native_root 1, and so on, so the two stay in step by construction rather than
     * by a second list that could drift from the first. */
    OCT_NATIVE_LEASE_EXPOSE_ROOT_BASE = 4,
    /* The same roots again, read-write. A grant that changes a user's files is a different grant
     * from one that reads them, so it is a different mode rather than a flag on the same one: a
     * lease asking to read cannot be mistaken for one asking to write. */
    OCT_NATIVE_LEASE_WRITE_ROOT_BASE = 9,
    OCT_NATIVE_LEASE_MODE_LAST = OCT_NATIVE_LEASE_WRITE_ROOT_BASE + 4, /* roots 1..5, writable */
} oct_native_lease_mode;

/* The root a mode serves, or NULL for the modes that serve no filesystem. The returned string is
 * a compiled-in literal: a caller cannot widen a scope, only choose among these.
 *
 * Why these are honoured over a channel whose sender cannot be established, when
 * EXPOSE_FILESYSTEM is not: the roots come from a table fixed in this source rather than supplied
 * by the caller; nothing is writable,
 * because the backend implements no write operation; ptpd's own sandbox decides independently
 * what is readable inside the chosen root, so a grant is not access; and the lease still expires
 * in fifteen seconds. What a forged word could do is show a host files under one of these roots
 * that ptpd can already read, on a phone where someone is already running code. That is a real
 * escalation and it is bounded; a path supplied over the same channel would not be. */
const char *oct_native_lease_root(oct_native_lease_mode mode);
/* Whether a mode serves real files, for callers that gate on that rather than on a specific root. */
bool        oct_native_lease_is_filesystem(oct_native_lease_mode mode);

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
 * version:4 mode:4 flags:5 commands:16 responses:16 provider_events:8 check:8, 61 of 64 bits. */
#define OCT_NATIVE_RUNTIME_NAME    "com.parv.octopus.native.runtime"
#define OCT_NATIVE_RUNTIME_VERSION 1u

/* Named, like the other enums Swift reads: Swift imports an anonymous C enum's constants as plain
 * integers with no rawValue, so the app could not use these the way it uses every other one. */
typedef enum oct_native_runtime_flag {
    OCT_NATIVE_RUNTIME_HOOKED    = 1u << 0, /* observer hooks are installed */
    OCT_NATIVE_RUNTIME_ELIGIBLE  = 1u << 1, /* the provider's own checks passed */
    OCT_NATIVE_RUNTIME_SAW_IDS   = 1u << 2, /* a real GetStorageIDs arrived from a host */
    OCT_NATIVE_RUNTIME_SAW_INFO  = 1u << 3, /* a real GetStorageInfo arrived from a host */
    OCT_NATIVE_RUNTIME_SERVING   = 1u << 4, /* the provider is installed and serving */
} oct_native_runtime_flag;

typedef struct oct_native_runtime {
    uint8_t  mode;            /* 0 none, 1 observing, 2 serving the synthetic tree */
    uint8_t  flags;           /* the OCT_NATIVE_RUNTIME_* bits above, five of them */
    uint16_t commands;        /* host commands seen, saturating at 65535 */
    uint16_t responses;       /* Apple responses paired with them, saturating */
    uint8_t  provider_events; /* provider calls served, saturating at 255 */
} oct_native_runtime;

uint64_t   oct_native_runtime_pack(const oct_native_runtime *runtime);
bool       oct_native_runtime_unpack(uint64_t word, oct_native_runtime *out);
oct_status oct_native_runtime_publish(const oct_native_runtime *runtime);
oct_status oct_native_runtime_read(oct_native_runtime *out);

/* ---- Is the USB transport live? --------------------------------------------------------------
 *
 * The runtime word is posted every poll, about once a second, for as long as ptpd runs, and ptpd
 * runs only while a host is connected. Its state, though, outlives it: notifyd keeps a name's state
 * while any registration is open, and the app's own registration keeps it open. So reading the
 * state alone would show "session active" indefinitely after the cable is pulled.
 *
 * What does not outlive it is the posting. This reports whether the runtime name has been posted
 * since the previous call, on a token held for the life of the process, which is a heartbeat that
 * stops when ptpd does. */
oct_status oct_native_runtime_check(bool *posted);

typedef enum oct_native_transport_stage {
    OCT_NATIVE_TRANSPORT_ABSENT = 0, /* no companion heard from recently: not installed, or unplugged */
    OCT_NATIVE_TRANSPORT_WAITING,    /* companion live in ptpd with hooks in, no host commands yet */
    OCT_NATIVE_TRANSPORT_HOST,       /* host commands arriving over USB */
    OCT_NATIVE_TRANSPORT_SERVING,    /* commands arriving and Octopus's storage being served */
} oct_native_transport_stage;

/* Whether the companion counts as live: heard from within `window_ms` of `now_ms`. A last-heard
 * time of zero means never, and a clock that runs backwards is treated as never too, rather than
 * as a post from the future. */
bool oct_native_transport_fresh(uint64_t now_ms, uint64_t last_heard_ms, uint64_t window_ms);

/* The stage a runtime snapshot describes. A stale snapshot is ABSENT whatever it says, which is the
 * point: the counters in a left-behind state describe a session that has ended. */
oct_native_transport_stage oct_native_transport_stage_of(const oct_native_runtime *runtime, bool fresh);
const char *oct_native_transport_stage_name(oct_native_transport_stage stage);

/* ---- Real filesystem roots -------------------------------------------------------------
 *
 * The read-only backend can serve a real directory, and has been tested in isolation since it
 * arrived, but it has never been connected to the live provider. The obstacle is not the backend:
 * ptpd runs under Apple's "ptpd" seatbelt profile, and on 9 November it was refused `open()` on a
 * directory owned by the same uid it runs as. Whatever that profile permits is the ceiling on what
 * can ever be served this way, and it is not knowable from outside the process.
 *
 * So the companion probes a fixed table of candidate roots and reports which it can open and list.
 * A table rather than a path for two reasons: a path does not fit in a notification word, and the
 * authorization channel is forgeable, so the set of things a forged grant could ask for must be
 * closed and known in advance. An index selects from this table and nothing else.
 *
 * Probing opens a directory read-only and reads one entry. It exposes nothing: the result is a
 * pair of bitmasks, never a name.
 */
typedef enum oct_native_root {
    OCT_NATIVE_ROOT_SYNTHETIC = 0, /* the in-memory tree; touches no filesystem */
    OCT_NATIVE_ROOT_MEDIA,         /* /var/mobile/Media, the tree Apple already serves from */
    OCT_NATIVE_ROOT_DCIM,          /* /var/mobile/Media/DCIM */
    OCT_NATIVE_ROOT_MOBILE,        /* /var/mobile */
    OCT_NATIVE_ROOT_TMP,           /* /var/tmp */
    OCT_NATIVE_ROOT_SYSTEM,        /* / */
    OCT_NATIVE_ROOT_COUNT
} oct_native_root;

/* NULL for OCT_NATIVE_ROOT_SYNTHETIC and for anything out of range. */
const char *oct_native_root_path(oct_native_root root);

/* OCT_OK when the directory opened and one entry could be read; *error carries errno otherwise.
 * A symlink in any component is refused, as the backend itself refuses one. */
oct_status oct_native_probe_root(const char *path, int *error);

#define OCT_NATIVE_ROOTS_NAME    "com.parv.octopus.native.roots"
#define OCT_NATIVE_ROOTS_VERSION 1u

typedef struct oct_native_root_report {
    uint8_t openable; /* bit per oct_native_root: the directory opened */
    uint8_t listable; /* bit per oct_native_root: an entry could be read from it */
    uint8_t refused;  /* bit per oct_native_root: EPERM or EACCES, so the sandbox refused it */
} oct_native_root_report;

uint64_t   oct_native_roots_pack(const oct_native_root_report *report);
bool       oct_native_roots_unpack(uint64_t word, oct_native_root_report *out);
oct_status oct_native_roots_publish(const oct_native_root_report *report);
oct_status oct_native_roots_read(oct_native_root_report *out);
/* Fills `report` by probing every root in the table. Serial caller. */
void       oct_native_probe_all_roots(oct_native_root_report *report);

/* What a report says about one root, so a caller reads a verdict rather than doing bit arithmetic
 * on three masks. */
typedef enum oct_native_root_state {
    OCT_NATIVE_ROOT_UNREACHABLE = 0, /* not openable, and not refused either */
    OCT_NATIVE_ROOT_REFUSED,         /* EPERM or EACCES: the sandbox said no */
    OCT_NATIVE_ROOT_OPENED_EMPTY,    /* opened, but no entry could be read */
    OCT_NATIVE_ROOT_READABLE,        /* opened and listed: servable read-only */
} oct_native_root_state;

const char *oct_native_root_state_name(oct_native_root_state state);
oct_native_root_state oct_native_root_state_of(const oct_native_root_report *report, oct_native_root root);

/* The lease mode that serves a root: synthetic exposure for the in-memory tree, the matching scope
 * for a real one. Lives here, next to the table it indexes, rather than in a caller that would have
 * to do the arithmetic itself; the first caller to try that got the types wrong. Returns
 * OCT_NATIVE_LEASE_NONE for a root outside the table. */
oct_native_lease_mode oct_native_lease_mode_for_root(oct_native_root root);
/* The writable mode for a root, or NONE for the synthetic tree, which is never writable. */
oct_native_lease_mode oct_native_lease_write_mode_for_root(oct_native_root root);
/* Whether a mode grants writes. Read-only modes return false, as does anything outside the table. */
bool oct_native_lease_is_writable(oct_native_lease_mode mode);

/* ---- Which provider hooks a host actually called -------------------------------------------
 *
 * The provider records a named event for every call it serves, but that journal only leaves ptpd
 * in the status file, which the sandbox refuses, so the app can see a count and nothing else. On
 * 10 November that left a question unanswerable: Explorer listed the folders and the binary object
 * and dropped all three text ones, and with only "11 provider events" there was no way to tell
 * whether the hook that advertises the text format to the host had run at all.
 *
 * One bit per hook, set the first time it is called, plus counts for the four that matter to a
 * listing, plus whether the text format was actually appended to the host's format list.
 *
 * version:4 fired:10 formats_added:1 device_info:8 handles:8 object_info:8 get_object:8 check:8. */
#define OCT_NATIVE_HOOKS_NAME    "com.parv.octopus.native.hooks"
#define OCT_NATIVE_HOOKS_VERSION 1u

enum {
    OCT_NATIVE_HOOK_DEVICE_INFO   = 1u << 0, /* deviceInfoDataset */
    OCT_NATIVE_HOOK_STORAGE_IDS   = 1u << 1, /* allStorageIDs */
    OCT_NATIVE_HOOK_STORAGE_INFO  = 1u << 2, /* storageInfoDatasetForStorageID: */
    OCT_NATIVE_HOOK_OBJECT_INFO   = 1u << 3, /* objectInfoDatasetForObjectHandle: */
    OCT_NATIVE_HOOK_HANDLES       = 1u << 4, /* objectHandlesInStorage:... */
    OCT_NATIVE_HOOK_BULK_INFO     = 1u << 5, /* objectInfoForObjectsInStorage:... */
    OCT_NATIVE_HOOK_DATA_SOURCE   = 1u << 6, /* dataSourceForObjectHandle:... */
    OCT_NATIVE_HOOK_PROP_VALUE    = 1u << 7, /* objectPropValueForHandle:... */
    OCT_NATIVE_HOOK_PROP_LIST     = 1u << 8, /* objectPropListForHandle:... */
    OCT_NATIVE_HOOK_PROPS_SUPPORTED = 1u << 9, /* objectPropertiesSupportedForObjectFormatCode: */
};

typedef struct oct_native_hook_report {
    uint16_t fired;         /* the OCT_NATIVE_HOOK_* bits above, ten of them */
    bool     formats_added; /* the text format was appended to a host's format list */
    uint8_t  device_info;   /* calls, saturating at 255 */
    uint8_t  handles;
    uint8_t  object_info;
    uint8_t  get_object;
} oct_native_hook_report;

const char *oct_native_hook_name(unsigned bit);
uint64_t   oct_native_hooks_pack(const oct_native_hook_report *report);
bool       oct_native_hooks_unpack(uint64_t word, oct_native_hook_report *out);
oct_status oct_native_hooks_publish(const oct_native_hook_report *report);
oct_status oct_native_hooks_read(oct_native_hook_report *out);

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

/*
 * The core half of Settings > Run Full Diagnostics: every non-destructive check of the storage provider, the
 * responder and the transport adapter, one at a time, each with a status, the kind of test, its checks, its time
 * and a reason. The responder checks are the self-test's own phases (oct_self_test_run_phase); the adapter checks
 * run the real responder behind a loopback link and are reported as simulated, never as a connection. The app adds
 * its own checks and the ones that need a computer.
 */
#ifndef OCT_DIAG_H
#define OCT_DIAG_H

#include "oct_service.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum oct_diag_status {
    OCT_DIAG_PASS = 0,
    OCT_DIAG_FAIL = 1,
    OCT_DIAG_SKIPPED = 2, /* not run: not applicable, or it needs a check that failed */
    OCT_DIAG_BLOCKED = 3  /* can't run: something it needs is missing */
} oct_diag_status;

typedef enum oct_diag_kind {
    OCT_DIAG_LOCAL = 0,     /* inside the app */
    OCT_DIAG_SIMULATED = 1, /* communication with a simulated host */
    OCT_DIAG_EXTERNAL = 2   /* a real connection to something outside the iPhone */
} oct_diag_kind;

#define OCT_DIAG_DETAIL_MAX 320

typedef struct oct_diag_result {
    char            id[48];
    char            group[40];
    char            title[96];
    oct_diag_status status;
    oct_diag_kind   kind;
    uint32_t        checks;
    uint64_t        duration_us;
    char            detail[OCT_DIAG_DETAIL_MAX];
} oct_diag_result;

typedef struct oct_diag oct_diag;

oct_status  oct_diag_create(oct_diag **out);
void        oct_diag_destroy(oct_diag *diag);
uint32_t    oct_diag_count(const oct_diag *diag);
/* Check `index`'s id, group, title and kind, without running it. */
oct_status  oct_diag_info(const oct_diag *diag, uint32_t index, oct_diag_result *out);
/* Runs check `index`, 0 to count - 1, in order. */
oct_status  oct_diag_run(oct_diag *diag, uint32_t index, oct_diag_result *out);
/* For the tests: check `id` fails as though its checks had, so what depends on it can be exercised. */
void        oct_diag_inject_failure(oct_diag *diag, const char *id);
const char *oct_diag_status_name(oct_diag_status status); /* PASS, FAIL, SKIPPED, BLOCKED */
const char *oct_diag_kind_name(oct_diag_kind kind);       /* local, simulated, external */

/* Whether a process named `name` is running, from the kernel's process table, read-only: *pid is its ID, or 0 when
 * none runs. OCT_E_UNSUPPORTED where the table can't be read. */
oct_status  oct_diag_find_process(const char *name, int32_t *pid);

/* ---- Restarting Apple's PTP responder ------------------------------------------------------
 *
 * ptpd owns the USB PTP function and is started by launchd when a computer is connected. A host
 * reads the list of storages once per session, so a storage Octopus starts serving mid-session is
 * not seen until the session restarts. Ending the process is how that happens without unplugging:
 * launchd starts a fresh one, Octopus's companion loads into it, and the host re-reads the list.
 *
 * This only ever signals a process whose name is exactly "ptpd", and only when it is running,
 * which on a phone means only while a computer is attached. It sends SIGTERM, never SIGKILL, so
 * the process can finish what it is doing; a transfer in flight would be interrupted, which is why
 * callers check that the link is quiet first.
 *
 * `exited` reports whether the old process is gone when the call returns. The new one is launchd's
 * business and may not exist yet, so its absence is not an error here. */
typedef struct oct_responder_restart {
    int32_t  previous_pid;  /* the process that was signalled, or 0 if none was running */
    bool     signalled;     /* SIGTERM was delivered */
    bool     exited;        /* and the process was gone within the wait */
    int32_t  error;         /* errno from kill when signalling failed */
} oct_responder_restart;

oct_status  oct_diag_restart_responder(oct_responder_restart *out);

#ifdef __cplusplus
}
#endif

#endif /* OCT_DIAG_H */

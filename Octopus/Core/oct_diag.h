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

#ifdef __cplusplus
}
#endif

#endif /* OCT_DIAG_H */

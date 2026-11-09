/* oct_diag.c - the core checks behind Run Full Diagnostics. See oct_diag.h. */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#elif !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "oct_diag.h"
#include "oct_bridge.h"
#include "oct_compat.h"
#include "oct_link.h"
#include "oct_sha256.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#else
#include <dirent.h>
#endif

#define DIAG_MAX 32

typedef struct diag_entry {
    const char   *id;
    const char   *group;
    const char   *title;
    oct_diag_kind kind;
    int           phase; /* a self-test phase, or -1 */
    void (*run)(oct_diag *diag, oct_diag_result *out);
} diag_entry;

struct oct_diag {
    diag_entry         entries[DIAG_MAX];
    char               phase_ids[DIAG_MAX][48];
    uint32_t           count;
    oct_self_test_run *self_test;
    bool               phase_failed;
    char               failed_title[96];
    char               injected[48];
};

static int64_t monotonic_us(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (int64_t)now.tv_sec * 1000000 + (int64_t)(now.tv_nsec / 1000);
}

const char *oct_diag_status_name(oct_diag_status status)
{
    switch (status) {
    case OCT_DIAG_PASS: return "PASS";
    case OCT_DIAG_FAIL: return "FAIL";
    case OCT_DIAG_SKIPPED: return "SKIPPED";
    case OCT_DIAG_BLOCKED: return "BLOCKED";
    }
    return "FAIL";
}

const char *oct_diag_kind_name(oct_diag_kind kind)
{
    switch (kind) {
    case OCT_DIAG_LOCAL: return "local";
    case OCT_DIAG_SIMULATED: return "simulated";
    case OCT_DIAG_EXTERNAL: return "external";
    }
    return "local";
}

__attribute__((format(printf, 3, 4)))
static void report(oct_diag_result *out, oct_diag_status status, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    out->status = status;
    vsnprintf(out->detail, sizeof out->detail, format, args);
    va_end(args);
}

/* ---- Storage --------------------------------------------------------------------- */

static oct_status open_synthetic(oct_store **out)
{
    oct_synthetic_options options = { 0, false };
    oct_backend backend;
    *out = NULL;
    oct_status status = oct_synthetic_backend_create(&options, &backend);
    if (status == OCT_OK) status = oct_store_open(NULL, backend, out);
    if (status == OCT_OK) status = oct_store_index_all(*out);
    return status;
}

static void storage_synthetic(oct_diag *diag, oct_diag_result *out)
{
    (void)diag;
    static const char *const expected[] = { "README.txt", "Documents", "example.txt", "sample.bin", "Information",
                                            "version.txt" };
    oct_store *store = NULL;
    oct_handle_list list = { 0 };
    oct_status status = open_synthetic(&store);
    if (status == OCT_OK) status = oct_store_list(store, OCT_ROOT, true, &list);
    if (status != OCT_OK) {
        report(out, OCT_DIAG_FAIL, "The synthetic storage couldn't be built: %s", oct_status_name(status));
        oct_store_close(store);
        return;
    }
    uint32_t folders = 0, files = 0, writable = 0, found = 0;
    uint64_t bytes = 0;
    bool nested = false;
    uint32_t documents = 0;
    oct_store_find(store, OCT_ROOT, "Documents", &documents);
    for (uint32_t i = 0; i < list.count; i++) {
        oct_object_info info;
        if (oct_store_get(store, list.handles[i], &info) != OCT_OK) continue;
        out->checks++;
        if (info.is_directory) folders++; else files++;
        if (!info.write_protected) writable++;
        bytes += info.size;
        for (size_t k = 0; k < sizeof expected / sizeof expected[0]; k++) found += strcmp(info.name, expected[k]) == 0;
        if (documents && info.parent == documents && strcmp(info.name, "example.txt") == 0) nested = true;
    }
    oct_storage_info storage;
    bool read_only = oct_store_storage_info(store, &storage) == OCT_OK && storage.access_capability == 0x0001;
    out->checks += 3;
    if (list.count == 6 && found == 6 && writable == 0 && read_only && nested) {
        report(out, OCT_DIAG_PASS, "%u objects (%u folders, %u files), %.1f KB, read-only; Documents/example.txt is nested",
               list.count, folders, files, (double)bytes / 1024.0);
    } else {
        report(out, OCT_DIAG_FAIL, "%u objects (%u expected names found), %u writable, storage %s, nested file %s",
               list.count, found, writable, read_only ? "read-only" : "writable", nested ? "found" : "missing");
    }
    oct_handle_list_free(&list);
    oct_store_close(store);
}

static void storage_handles(oct_diag *diag, oct_diag_result *out)
{
    (void)diag;
    oct_store *store = NULL;
    oct_handle_list first = { 0 }, second = { 0 };
    oct_status status = open_synthetic(&store);
    if (status == OCT_OK) status = oct_store_list(store, OCT_ROOT, true, &first);
    if (status == OCT_OK) status = oct_store_index_all(store);
    if (status == OCT_OK) status = oct_store_list(store, OCT_ROOT, true, &second);
    bool same = status == OCT_OK && first.count == second.count && first.count > 0 &&
                memcmp(first.handles, second.handles, first.count * sizeof first.handles[0]) == 0;
    out->checks = 2;
    if (same) {
        report(out, OCT_DIAG_PASS, "%u handles, unchanged across two listings and a re-index", first.count);
    } else {
        report(out, OCT_DIAG_FAIL, "Handles changed between listings (%u, then %u): %s", first.count, second.count,
               oct_status_name(status));
    }
    oct_handle_list_free(&first);
    oct_handle_list_free(&second);
    oct_store_close(store);
}

static void storage_contents(oct_diag *diag, oct_diag_result *out)
{
    (void)diag;
    oct_store *store = NULL;
    oct_handle_list list = { 0 };
    oct_status status = open_synthetic(&store);
    if (status == OCT_OK) status = oct_store_list(store, OCT_ROOT, true, &list);
    uint32_t files = 0;
    uint64_t total = 0;
    for (uint32_t i = 0; status == OCT_OK && i < list.count; i++) {
        oct_object_info info;
        status = oct_store_get(store, list.handles[i], &info);
        if (status != OCT_OK || info.is_directory) continue;
        uint8_t *bytes = malloc(info.size ? (size_t)info.size : 1u);
        size_t got = 0;
        status = bytes ? oct_store_read(store, list.handles[i], 0, bytes, (size_t)info.size, &got) : OCT_E_NO_MEMORY;
        if (status == OCT_OK && got != info.size) status = OCT_E_IO;
        free(bytes);
        files++;
        total += info.size;
        out->checks++;
    }
    oct_handle_list_free(&list);
    oct_store_close(store);
    char digest[65];
    uint64_t size = 0;
    if (status == OCT_OK) status = oct_compat_reference_digest("Documents", "example.txt", &size, digest);
    if (status == OCT_OK) {
        out->checks++;
        report(out, OCT_DIAG_PASS, "%u files read whole, %llu bytes; Documents/example.txt is %llu bytes, SHA-256 %s",
               files, (unsigned long long)total, (unsigned long long)size, digest);
    } else {
        report(out, OCT_DIAG_FAIL, "Reading the files failed: %s", oct_status_name(status));
    }
}

/* ---- The self-test's phases ------------------------------------------------------ */

static void run_phase(oct_diag *diag, const diag_entry *entry, oct_diag_result *out)
{
    if (diag->phase_failed) {
        report(out, OCT_DIAG_SKIPPED, "Not run: it needs \"%s\", which failed", diag->failed_title);
        return;
    }
    if (!diag->self_test) {
        oct_status status = oct_self_test_open(true, &diag->self_test);
        if (status != OCT_OK) {
            report(out, OCT_DIAG_FAIL, "The temporary storage couldn't be built: %s", oct_status_name(status));
            diag->phase_failed = true;
            oct_copy_utf8(diag->failed_title, sizeof diag->failed_title, entry->title);
            return;
        }
    }
    char failure[OCT_EVENT_TEXT_MAX] = "";
    uint32_t checks = 0;
    oct_status status = oct_self_test_run_phase(diag->self_test, (uint32_t)entry->phase, &checks, failure, sizeof failure);
    out->checks = checks;
    if (status == OCT_OK) {
        report(out, OCT_DIAG_PASS, "%u checks", checks);
    } else {
        report(out, OCT_DIAG_FAIL, "%s", failure);
        diag->phase_failed = true;
        oct_copy_utf8(diag->failed_title, sizeof diag->failed_title, entry->title);
    }
}

/* ---- The transport adapter, simulated ----------------------------------------------- */

static void adapter_loopback(oct_diag *diag, oct_diag_result *out)
{
    (void)diag;
    char failure[OCT_EVENT_TEXT_MAX] = "";
    uint32_t checks = 0;
    oct_status status = oct_link_self_test(&checks, failure, sizeof failure);
    out->checks = checks;
    if (status == OCT_OK) {
        report(out, OCT_DIAG_PASS, "%u checks: fragmented input, a malformed container, a stall, a cancel, a reset, "
               "repeated sessions and a reconnect", checks);
    } else {
        report(out, OCT_DIAG_FAIL, "%s", failure);
    }
}

static void run_compat(oct_diag_result *out, oct_status (*check)(oct_compat_report *))
{
    oct_compat_report result;
    oct_status status = check(&result);
    out->checks = result.checks;
    report(out, status == OCT_OK ? OCT_DIAG_PASS : OCT_DIAG_FAIL, "%s", status == OCT_OK ? result.summary : result.failure);
}

static void adapter_enumeration(oct_diag *d, oct_diag_result *out) { (void)d; run_compat(out, oct_compat_mtp_enumeration); }
static void adapter_requests(oct_diag *d, oct_diag_result *out) { (void)d; run_compat(out, oct_compat_class_requests); }
static void adapter_events(oct_diag *d, oct_diag_result *out) { (void)d; run_compat(out, oct_compat_events); }
static void adapter_lifecycle(oct_diag *d, oct_diag_result *out) { (void)d; run_compat(out, oct_compat_lifecycle); }

static void adapter_usb_boundary(oct_diag *d, oct_diag_result *out) { (void)d; run_compat(out, oct_compat_usb_boundary); }

static oct_status bridge_self_test(oct_compat_report *report) { return oct_bridge_self_test(NULL, report); }
static void adapter_bridge(oct_diag *d, oct_diag_result *out) { (void)d; run_compat(out, bridge_self_test); }

/* ---- The list -------------------------------------------------------------------------- */

static void add(oct_diag *diag, const char *id, const char *group, const char *title, oct_diag_kind kind, int phase,
                void (*run)(oct_diag *, oct_diag_result *))
{
    if (diag->count >= DIAG_MAX) return;
    diag->entries[diag->count++] = (diag_entry){ id, group, title, kind, phase, run };
}

oct_status oct_diag_create(oct_diag **out)
{
    if (!out) return OCT_E_INVALID_ARGUMENT;
    oct_diag *diag = calloc(1, sizeof *diag);
    if (!diag) return OCT_E_NO_MEMORY;
    add(diag, "storage.synthetic", "Storage provider", "Synthetic storage", OCT_DIAG_LOCAL, -1, storage_synthetic);
    add(diag, "storage.handles", "Storage provider", "Stable handles", OCT_DIAG_LOCAL, -1, storage_handles);
    add(diag, "storage.contents", "Storage provider", "File contents", OCT_DIAG_LOCAL, -1, storage_contents);
    for (uint32_t phase = 0; phase < oct_self_test_phase_count(); phase++) {
        const char *id = oct_self_test_phase_id(phase);
        if (!id || strcmp(id, "transport") == 0) continue; /* the adapter has its own group */
        snprintf(diag->phase_ids[phase], sizeof diag->phase_ids[phase], "responder.%s", id);
        add(diag, diag->phase_ids[phase], "Responder", oct_self_test_phase_title(phase), OCT_DIAG_LOCAL, (int)phase, NULL);
    }
    add(diag, "adapter.loopback", "Transport adapter (simulated)", "Adapter self-test", OCT_DIAG_SIMULATED, -1,
        adapter_loopback);
    add(diag, "adapter.mtp_enumeration", "Transport adapter (simulated)", "MTP enumeration of the read-only storage",
        OCT_DIAG_SIMULATED, -1, adapter_enumeration);
    add(diag, "adapter.class_requests", "Transport adapter (simulated)", "USB class requests", OCT_DIAG_SIMULATED, -1,
        adapter_requests);
    add(diag, "adapter.events", "Transport adapter (simulated)", "Events on the interrupt pipe", OCT_DIAG_SIMULATED, -1,
        adapter_events);
    add(diag, "adapter.lifecycle", "Transport adapter (simulated)", "USB lifecycle", OCT_DIAG_SIMULATED, -1,
        adapter_lifecycle);
    add(diag, "adapter.usb_boundary", "Transport adapter (simulated)", "USB interface and setup validation",
        OCT_DIAG_SIMULATED, -1, adapter_usb_boundary);
    add(diag, "adapter.bridge", "Transport adapter (simulated)", "Bridge socket transport", OCT_DIAG_SIMULATED, -1,
        adapter_bridge);
    *out = diag;
    return OCT_OK;
}

void oct_diag_destroy(oct_diag *diag)
{
    if (!diag) return;
    oct_self_test_close(diag->self_test);
    free(diag);
}

uint32_t oct_diag_count(const oct_diag *diag) { return diag ? diag->count : 0; }

void oct_diag_inject_failure(oct_diag *diag, const char *id)
{
    if (diag) oct_copy_utf8(diag->injected, sizeof diag->injected, id ? id : "");
}

oct_status oct_diag_info(const oct_diag *diag, uint32_t index, oct_diag_result *out)
{
    if (!diag || !out || index >= diag->count) return OCT_E_INVALID_ARGUMENT;
    const diag_entry *entry = &diag->entries[index];
    memset(out, 0, sizeof *out);
    oct_copy_utf8(out->id, sizeof out->id, entry->id);
    oct_copy_utf8(out->group, sizeof out->group, entry->group);
    oct_copy_utf8(out->title, sizeof out->title, entry->title);
    out->kind = entry->kind;
    return OCT_OK;
}

oct_status oct_diag_run(oct_diag *diag, uint32_t index, oct_diag_result *out)
{
    oct_status identity = oct_diag_info(diag, index, out);
    if (identity != OCT_OK) return identity;
    const diag_entry *entry = &diag->entries[index];
    int64_t started = monotonic_us();
    if (diag->injected[0] && strcmp(diag->injected, entry->id) == 0) {
        report(out, OCT_DIAG_FAIL, "Failure injected by a test");
        if (entry->phase >= 0) {
            diag->phase_failed = true;
            oct_copy_utf8(diag->failed_title, sizeof diag->failed_title, entry->title);
        }
    } else if (entry->phase >= 0) {
        run_phase(diag, entry, out);
    } else {
        entry->run(diag, out);
    }
    int64_t elapsed = monotonic_us() - started;
    out->duration_us = elapsed > 0 ? (uint64_t)elapsed : 0;
    return OCT_OK;
}

/* ---- Processes ------------------------------------------------------------------------- */

oct_status oct_diag_find_process(const char *name, int32_t *pid)
{
    if (!name || !name[0] || !pid) return OCT_E_INVALID_ARGUMENT;
    *pid = 0;
#if defined(__APPLE__)
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0 };
    size_t size = 0;
    if (sysctl(mib, 3, NULL, &size, NULL, 0) != 0 || size == 0) return OCT_E_UNSUPPORTED;
    size += size / 4 + 16 * sizeof(struct kinfo_proc);
    struct kinfo_proc *processes = malloc(size);
    if (!processes) return OCT_E_NO_MEMORY;
    if (sysctl(mib, 3, processes, &size, NULL, 0) != 0) {
        free(processes);
        return OCT_E_UNSUPPORTED;
    }
    size_t count = size / sizeof *processes;
    for (size_t i = 0; i < count; i++) {
        if (strncmp(processes[i].kp_proc.p_comm, name, sizeof processes[i].kp_proc.p_comm) == 0) {
            *pid = (int32_t)processes[i].kp_proc.p_pid;
            break;
        }
    }
    free(processes);
    return OCT_OK;
#else
    DIR *proc = opendir("/proc");
    if (!proc) return OCT_E_UNSUPPORTED;
    struct dirent *entry;
    while ((entry = readdir(proc)) != NULL && *pid == 0) {
        char *end = NULL;
        long number = strtol(entry->d_name, &end, 10);
        if (!end || *end != '\0' || number <= 0) continue;
        char path[64], comm[64] = "";
        snprintf(path, sizeof path, "/proc/%ld/comm", number);
        FILE *file = fopen(path, "r");
        if (!file) continue;
        if (fgets(comm, sizeof comm, file)) comm[strcspn(comm, "\n")] = '\0';
        fclose(file);
        if (strcmp(comm, name) == 0) *pid = (int32_t)number;
    }
    closedir(proc);
    return OCT_OK;
#endif
}

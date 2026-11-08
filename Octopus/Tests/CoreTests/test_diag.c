#include "test_support.h"

#include "../../Core/oct_compat.h"
#include "../../Core/oct_diag.h"
#include "../../Core/oct_log.h"
#include "../../Core/oct_sha256.h"
#include "../../Core/oct_zip.h"

#include <sys/stat.h>
#include <unistd.h>

static void test_sha256_and_crc32(void)
{
    char hex[65];
    oct_sha256_hex("", 0, hex);
    CHECK_STR(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    oct_sha256_hex("abc", 3, hex);
    CHECK_STR(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const char *two_blocks = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    oct_sha256_hex(two_blocks, strlen(two_blocks), hex);
    CHECK_STR(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    oct_sha256 context;
    oct_sha256_init(&context);
    char piece[997]; /* a million 'a', in pieces that straddle block boundaries */
    memset(piece, 'a', sizeof piece);
    for (size_t left = 1000000; left > 0;) {
        size_t n = left < sizeof piece ? left : sizeof piece;
        oct_sha256_update(&context, piece, n);
        left -= n;
    }
    uint8_t digest[32];
    oct_sha256_final(&context, digest);
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    CHECK_STR(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    CHECK_EQ(oct_crc32("123456789", 9), 0xCBF43926u);
    CHECK_EQ(oct_crc32(NULL, 0), 0);
}

static uint32_t le32_at(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void test_zip(void)
{
    oct_zip *zip = NULL;
    CHECK_OK(oct_zip_create(&zip));
    static const char *const bad[] = { "", "/abs", "dir/", "a//b", "./a", "a/../b", "..", "a\\b", "tab\tname" };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK_EQ(oct_zip_add(zip, bad[i], "x", 1, 0), OCT_E_INVALID_ARGUMENT);
    }
    char long_name[300];
    memset(long_name, 'n', sizeof long_name - 1);
    long_name[sizeof long_name - 1] = '\0';
    CHECK_EQ(oct_zip_add(zip, long_name, "x", 1, 0), OCT_E_INVALID_ARGUMENT);
    CHECK_EQ(oct_zip_add(zip, "data.bin", NULL, 5, 0), OCT_E_INVALID_ARGUMENT);
    CHECK_EQ(oct_zip_count(zip), 0);
    CHECK_OK(oct_zip_add(zip, "results.json", "{}\n", 3, 1762632000));
    CHECK_OK(oct_zip_add(zip, "logs/octopus-debug.log", "line\n", 5, 100)); /* before 1980: stored as 1980 */
    CHECK_OK(oct_zip_add(zip, "empty.txt", NULL, 0, 1762632000));
    CHECK_OK(oct_zip_add(zip, "caf\xC3\xA9/r\xC3\xA9sum\xC3\xA9.txt", "utf-8", 5, 1762632000));
    CHECK_EQ(oct_zip_count(zip), 4);
    char path[] = "/tmp/octopus-zip-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    close(fd);
    CHECK_OK(oct_zip_write(zip, path));
    oct_zip_destroy(zip);
    FILE *file = fopen(path, "rb");
    uint8_t bytes[1024];
    size_t length = file ? fread(bytes, 1, sizeof bytes, file) : 0;
    if (file) fclose(file);
    CHECK(length > 22);
    CHECK_EQ(le32_at(bytes), 0x04034b50u);                 /* a local header first */
    const uint8_t *end = bytes + length - 22;
    CHECK_EQ(le32_at(end), 0x06054b50u);                    /* the end of the central directory */
    CHECK_EQ(end[10] | (end[11] << 8), 4);                  /* four entries */
    CHECK_EQ(le32_at(end + 12) + le32_at(end + 16) + 22u, length); /* directory size + offset + record = file */
    CHECK_EQ(le32_at(bytes + le32_at(end + 16)), 0x02014b50u);
    unlink(path);
    char partial[64];
    snprintf(partial, sizeof partial, "%s.partial", path);
    CHECK(access(partial, F_OK) != 0); /* nothing left behind */
    CHECK_EQ(oct_zip_write(NULL, path), OCT_E_INVALID_ARGUMENT);
}

static void test_compatibility(void)
{
    oct_compat_report report;
    CHECK_OK(oct_compat_mtp_enumeration(&report));
    CHECK(report.checks >= 70);
    char reference[65];
    uint64_t size = 0;
    CHECK_OK(oct_compat_reference_digest("Documents", "example.txt", &size, reference));
    CHECK(size > 0 && strstr(report.summary, reference) != NULL);
    CHECK_OK(oct_compat_class_requests(&report));
    CHECK(report.checks >= 20);
    CHECK_OK(oct_compat_events(&report));
    CHECK(report.checks >= 15);
    CHECK_OK(oct_compat_lifecycle(&report));
    CHECK(report.checks >= 40);
    CHECK_EQ(oct_compat_reference_digest("Documents", "missing.txt", &size, reference), OCT_E_INVALID_HANDLE);
    CHECK_EQ(oct_compat_reference_digest(NULL, "Documents", &size, reference), OCT_E_IS_DIRECTORY);
}

static void test_diagnostics(void)
{
    oct_diag *diag = NULL;
    CHECK_OK(oct_diag_create(&diag));
    uint32_t count = oct_diag_count(diag);
    CHECK_EQ(count, 17);
    uint32_t passed = 0, simulated = 0;
    for (uint32_t i = 0; i < count; i++) {
        oct_diag_result result, identity;
        CHECK_OK(oct_diag_info(diag, i, &identity));
        CHECK_OK(oct_diag_run(diag, i, &result));
        CHECK_STR(identity.id, result.id);
        CHECK_STR(identity.title, result.title);
        CHECK_EQ(identity.kind, result.kind);
        passed += result.status == OCT_DIAG_PASS;
        if (strncmp(result.id, "adapter.", 8) == 0) {
            CHECK_EQ(result.kind, OCT_DIAG_SIMULATED);
            simulated++;
        } else {
            CHECK_EQ(result.kind, OCT_DIAG_LOCAL);
        }
        CHECK(result.title[0] && result.group[0] && result.detail[0] && result.checks > 0);
        if (result.status != OCT_DIAG_PASS) fprintf(stderr, "diagnostic %s: %s\n", result.id, result.detail);
    }
    CHECK_EQ(passed, count);
    CHECK_EQ(simulated, 5);
    oct_diag_result result;
    CHECK_EQ(oct_diag_run(diag, count, &result), OCT_E_INVALID_ARGUMENT);
    oct_diag_destroy(diag);

    /* A failed phase skips the phases that need it, and nothing else. */
    CHECK_OK(oct_diag_create(&diag));
    oct_diag_inject_failure(diag, "responder.session");
    uint32_t failed = 0, skipped = 0, after_pass = 0;
    bool past = false;
    for (uint32_t i = 0; i < oct_diag_count(diag); i++) {
        CHECK_OK(oct_diag_run(diag, i, &result));
        if (strcmp(result.id, "responder.session") == 0) {
            CHECK_EQ(result.status, OCT_DIAG_FAIL);
            failed++;
            past = true;
        } else if (past && strncmp(result.id, "responder.", 10) == 0) {
            CHECK_EQ(result.status, OCT_DIAG_SKIPPED);
            CHECK(strstr(result.detail, "Session rules") != NULL);
            skipped++;
        } else if (past) {
            after_pass += result.status == OCT_DIAG_PASS;
        }
    }
    CHECK_EQ(failed, 1);
    CHECK_EQ(skipped, 7);
    CHECK_EQ(after_pass, 5); /* the adapter checks don't depend on the responder phases */
    oct_diag_destroy(diag);
    CHECK_STR(oct_diag_status_name(OCT_DIAG_BLOCKED), "BLOCKED");
    CHECK_STR(oct_diag_kind_name(OCT_DIAG_EXTERNAL), "external");
}

static void test_find_process(void)
{
    int32_t pid = -1;
#if defined(__APPLE__)
    CHECK_OK(oct_diag_find_process("launchd", &pid)); /* process 1 on every Mac */
    CHECK_EQ(pid, 1);
#else
    char own[64] = ""; /* this test binary's own name: core-tests, core-tests-san or core-tests-tsan */
    FILE *self = fopen("/proc/self/comm", "r");
    if (self) {
        if (fgets(own, sizeof own, self)) own[strcspn(own, "\n")] = '\0';
        fclose(self);
    }
    if (own[0]) {
        CHECK_OK(oct_diag_find_process(own, &pid));
        CHECK(pid > 0);
    }
#endif
    CHECK_OK(oct_diag_find_process("no-such-process", &pid));
    CHECK_EQ(pid, 0);
    CHECK_EQ(oct_diag_find_process("", &pid), OCT_E_INVALID_ARGUMENT);
    CHECK_EQ(oct_diag_find_process("ptpd", NULL), OCT_E_INVALID_ARGUMENT);
}

static void test_log_recovery(void)
{
    char directory[] = "/tmp/octopus-flush-XXXXXX", path[128];
    CHECK(mkdtemp(directory) != NULL);
    snprintf(path, sizeof path, "%s/octopus-debug.log", directory);
    oct_log_config config = { path, 0, 1, OCT_LOG_INFO };
    CHECK_OK(oct_log_open(&config));
    oct_logf(OCT_LOG_INFO, "test", "before", "written before the file vanished");
    CHECK_OK(oct_log_flush());
    CHECK(unlink(path) == 0); /* something cleans the folder */
    CHECK_OK(oct_log_flush());
    oct_logf(OCT_LOG_INFO, "test", "after", "written to the recreated file");
    oct_log_stats stats;
    oct_log_get_stats(&stats);
    CHECK_EQ(stats.reopens, 1);
    CHECK(stats.file_open);
    struct stat info;
    CHECK(stat(path, &info) == 0 && info.st_size > 0);
    FILE *file = fopen(path, "r");
    char text[512] = "";
    size_t got = file ? fread(text, 1, sizeof text - 1, file) : 0;
    text[got] = '\0';
    if (file) fclose(file);
    CHECK(strstr(text, "written to the recreated file") != NULL);
    CHECK(strstr(text, "written before") == NULL);
    if (access("/dev/full", W_OK) == 0) { /* a full disk isn't a bad handle: no reopening, an error */
        oct_log_config full = { "/dev/full", 0, 1, OCT_LOG_INFO };
        CHECK_OK(oct_log_open(&full));
        oct_logf(OCT_LOG_ERROR, "test", "full", "disk full");
        oct_log_get_stats(&stats);
        CHECK_EQ(stats.reopens, 0);
        CHECK_EQ(stats.write_errors, 1);
    }
    unlink(path);
    rmdir(directory);
    oct_log_config memory = { NULL, 0, 0, OCT_LOG_INFO };
    oct_log_open(&memory);
    CHECK_OK(oct_log_flush()); /* memory only: nothing to flush */
}

void run_diag_tests(void)
{
    test_sha256_and_crc32();
    test_zip();
    test_compatibility();
    test_diagnostics();
    test_find_process();
    test_log_recovery();
}

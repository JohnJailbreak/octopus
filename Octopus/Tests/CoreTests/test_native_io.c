#include "test_support.h"
#include <stdio.h>
#include <string.h>
#include "../../Core/oct_native_io.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static const char token_value[] = "0123456789abcdef0123456789abcdef";

static void test_round_trip(const char *directory)
{
    char bytes[65537];
    size_t length = 99;
    CHECK_OK(oct_native_io_write(directory, "status", "first", 5));
    CHECK_OK(oct_native_io_read(directory, "status", bytes, sizeof bytes - 1, &length));
    CHECK_EQ(length, 5); CHECK(memcmp(bytes, "first", 5) == 0);
    CHECK_OK(oct_native_io_write(directory, "status", "next", 4));
    CHECK_OK(oct_native_io_read(directory, "status", bytes, sizeof bytes - 1, &length));
    CHECK_EQ(length, 4); CHECK(memcmp(bytes, "next", 4) == 0);
    CHECK_EQ(oct_native_io_read(directory, "status", bytes, 3, &length), OCT_E_IO);
    CHECK_EQ(length, 0);
    CHECK_OK(oct_native_io_write(directory, "status", NULL, 0));
    CHECK_OK(oct_native_io_read(directory, "status", bytes, 0, &length));
    CHECK_EQ(length, 0);
    for (size_t i = 0; i < 65536; i++) bytes[i] = (char)(i % 127);
    CHECK_OK(oct_native_io_write(directory, "status", bytes, 65536));
    memset(bytes, 0, sizeof bytes);
    CHECK_OK(oct_native_io_read(directory, "status", bytes, 65536, &length));
    CHECK_EQ(length, 65536);
    for (size_t i = 0; i < length; i++) CHECK_EQ(bytes[i], (char)(i % 127));
    CHECK_EQ(oct_native_io_write(directory, "status", bytes, 65537), OCT_E_INVALID_ARGUMENT);
    CHECK_EQ(oct_native_io_read(directory, "status", bytes, 65537, &length), OCT_E_INVALID_ARGUMENT);
    CHECK_EQ(length, 0);
    const char *invalid[] = { "", ".", "..", "../lease", "sub/status", "/status", "Lease", "lease.tmp", "status/", NULL };
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        CHECK_EQ(oct_native_io_write(directory, invalid[i], bytes, 1), OCT_E_INVALID_ARGUMENT);
        CHECK_EQ(oct_native_io_read(directory, invalid[i], bytes, sizeof bytes - 1, &length), OCT_E_INVALID_ARGUMENT);
        CHECK_EQ(length, 0);
    }
    CHECK_EQ(oct_native_io_write(directory, "lease", NULL, 1), OCT_E_INVALID_ARGUMENT);
    CHECK_EQ(oct_native_io_read(directory, "status", NULL, 1, &length), OCT_E_INVALID_ARGUMENT);
    CHECK_EQ(oct_native_io_read(directory, "status", bytes, 1, NULL), OCT_E_INVALID_ARGUMENT);
    CHECK_EQ(oct_native_io_write(NULL, "status", bytes, 1), OCT_E_IO);
    CHECK_EQ(oct_native_io_read(NULL, "status", bytes, 1, &length), OCT_E_IO);
    CHECK_OK(oct_native_io_remove_lease(directory));
    /* A stale/existing staging name cannot be overwritten, and the old status
     * remains intact. Removing the obstruction permits a subsequent publish. */
    char temporary[1024];
    snprintf(temporary,sizeof temporary,"%s/.status.%ld.tmp",directory,(long)getpid());
    int fd=open(temporary,O_WRONLY|O_CREAT|O_EXCL,0600);
    CHECK(fd>=0);
    if(fd>=0)close(fd);
    CHECK_EQ(oct_native_io_write(directory,"status","blocked",7),OCT_E_IO);
    CHECK_OK(oct_native_io_read(directory,"status",bytes,65536,&length));
    CHECK_EQ(length,65536);
    CHECK_EQ(unlink(temporary),0);
    CHECK_OK(oct_native_io_write(directory,"status","recovered",9));
}

static void invalid_lease(const char *directory, const char *bytes, size_t length, uint64_t now)
{
    uint64_t deadline = 99;
    char token[33];
    memset(token, 'x', sizeof token);
    if (directory) {
        CHECK_OK(oct_native_io_write(directory, "lease", bytes, length));
        CHECK_EQ(oct_native_read_lease(directory, now, &deadline, token), 0);
    } else CHECK_EQ(oct_native_parse_lease(bytes,length,now,&deadline,token),0);
    CHECK_EQ(deadline, 0);
    CHECK_EQ(token[0], 0);
}

static void test_parse_lease(void)
{
    uint64_t deadline = 0;
    char token[33];
    static const char *invalid[] = {
        "", "OCTN2 16000 1 0123456789abcdef0123456789abcdef\n",
        "OCTN1 +16000 1 0123456789abcdef0123456789abcdef\n",
        "OCTN1 -16000 1 0123456789abcdef0123456789abcdef\n",
        "OCTN1 016000 1 0123456789abcdef0123456789abcdef\n",
        "OCTN1 18446744073709551616 1 0123456789abcdef0123456789abcdef\n",
        "OCTN1 999999999999999999999999999999 1 0123456789abcdef0123456789abcdef\n",
        "OCTN1 16000 +1 0123456789abcdef0123456789abcdef\n",
        "OCTN1 16000 -4294967295 0123456789abcdef0123456789abcdef\n",
        "OCTN1 16000 4294967297 0123456789abcdef0123456789abcdef\n",
        "OCTN1 16000 0 0123456789abcdef0123456789abcdef\n",
        "OCTN1 16000 3 0123456789abcdef0123456789abcdef\n",
        "OCTN1 16000 01 0123456789abcdef0123456789abcdef\n",
        "OCTN1 16000 1 0123456789abcdef0123456789abcdef",
        "OCTN1 16000 1 0123456789abcdef0123456789abcdef\n\n",
        "OCTN1 16000 1 0123456789abcdef0123456789abcdef\r\n",
        "OCTN1 16000 1 0123456789abcdef0123456789abcdef extra\n",
        " OCTN1 16000 1 0123456789abcdef0123456789abcdef\n",
        "OCTN1\t16000 1 0123456789abcdef0123456789abcdef\n",
        "OCTN1  16000 1 0123456789abcdef0123456789abcdef\n",
        "OCTN1 16000 1 0123456789ABCDEF0123456789ABCDEF\n"
    };
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++) invalid_lease(NULL, invalid[i], strlen(invalid[i]), 1000);
    const char embedded[] = "OCTN1 16000 1 0123456789abcdef0123456789abcdef\n\0garbage";
    invalid_lease(NULL, embedded, sizeof embedded - 1, 1000);
    const char canonical[] = "OCTN1 16000 1 0123456789abcdef0123456789abcdef\n";
    for (size_t i = 0; i < sizeof canonical - 1; i++) invalid_lease(NULL, canonical, i, 1000);
    invalid_lease(NULL, canonical, sizeof canonical, 1000); /* NUL is not canonical on disk */

    const char good[] = "OCTN1 16000 1 0123456789abcdef0123456789abcdef\n";
    CHECK_EQ(oct_native_parse_lease(good,sizeof good-1,1000,&deadline,token),1);
    CHECK_EQ(deadline,16000); CHECK_STR(token,token_value);
    CHECK_EQ(oct_native_parse_lease(good,sizeof good-1,16000,&deadline,token),0);
    CHECK_EQ(deadline,0); CHECK_EQ(token[0],0);
    CHECK_EQ(oct_native_parse_lease(good,sizeof good-1,999,&deadline,token),0);
    CHECK_EQ(oct_native_parse_lease(NULL,0,1000,&deadline,token),0);
    CHECK_EQ(oct_native_parse_lease(good,97,1000,&deadline,token),0);
    CHECK_EQ(oct_native_parse_lease(good,sizeof good-1,1000,NULL,token),0);
    CHECK_EQ(oct_native_parse_lease(good,sizeof good-1,1000,&deadline,NULL),0);
    /* Every accepted single-byte mutation must re-encode to the same canonical
     * bytes, and satisfy the bounded deadline/mode/token invariants. */
    for (size_t pos=0;pos<sizeof good-1;pos++) {
        for(unsigned byte=0;byte<256;byte++) {
            char mutated[sizeof good], encoded[96];
            memcpy(mutated,good,sizeof good);
            mutated[pos]=(char)byte;
            unsigned mode=oct_native_parse_lease(mutated,sizeof good-1,1000,&deadline,token);
            if(mode) {
                int n=snprintf(encoded,sizeof encoded,"OCTN1 %llu %u %s\n",(unsigned long long)deadline,mode,token);
                CHECK(n>0 && (size_t)n==sizeof good-1);
                CHECK(memcmp(encoded,mutated,sizeof good-1)==0);
                CHECK(deadline>1000 && deadline-1000<=OCT_NATIVE_LEASE_MS);
                CHECK(mode==1 || mode==2);
            } else {
                CHECK_EQ(deadline,0); CHECK_EQ(token[0],0);
            }
        }
    }
}

static void test_leases(const char *directory)
{
    char token[33];
    uint64_t deadline = 99;
    CHECK_OK(oct_native_write_lease(directory, token_value, 1, 1000));
    CHECK_EQ(oct_native_read_lease(directory, 1000, &deadline, token), 1);
    CHECK_EQ(deadline, 16000); CHECK_STR(token, token_value);
    CHECK_EQ(oct_native_read_lease(directory, 15999, &deadline, token), 1);
    CHECK_EQ(oct_native_read_lease(directory, 16000, &deadline, token), 0);
    CHECK_EQ(deadline, 0); CHECK_EQ(token[0], 0);
    CHECK_EQ(oct_native_read_lease(directory, 999, &deadline, token), 0); /* clock moved backwards */
    CHECK_OK(oct_native_write_lease(directory, token_value, 2, 2000));
    CHECK_EQ(oct_native_read_lease(directory, 2000, &deadline, token), 2);
    CHECK_OK(oct_native_io_remove_lease(directory));
    CHECK_EQ(oct_native_read_lease(directory, 2000, &deadline, token), 0);
    CHECK_EQ(deadline, 0); CHECK_EQ(token[0], 0);
    CHECK_OK(oct_native_io_remove_lease(directory));
    CHECK_EQ(oct_native_write_lease(directory, token_value, 0, 1000), OCT_E_INVALID_ARGUMENT);
    CHECK_EQ(oct_native_write_lease(directory, token_value, 3, 1000), OCT_E_INVALID_ARGUMENT);
    CHECK_EQ(oct_native_write_lease(directory, token_value, 1, UINT64_MAX - OCT_NATIVE_LEASE_MS + 1), OCT_E_INVALID_ARGUMENT);
    CHECK_OK(oct_native_write_lease(directory, token_value, 1, UINT64_MAX - OCT_NATIVE_LEASE_MS));
    CHECK_EQ(oct_native_read_lease(directory, UINT64_MAX - OCT_NATIVE_LEASE_MS, &deadline, token), 1);
    CHECK_EQ(deadline, UINT64_MAX);
    CHECK_EQ(oct_native_read_lease(directory, UINT64_MAX, &deadline, token), 0);
    const char *bad_tokens[] = { "", "0", "0123456789ABCDEF0123456789ABCDEF", "0123456789abcdef0123456789abcdeg", "0123456789abcdef0123456789abcdef0", NULL };
    for (size_t i = 0; i < sizeof bad_tokens / sizeof bad_tokens[0]; i++)
        CHECK_EQ(oct_native_write_lease(directory, bad_tokens[i], 1, 1000), OCT_E_INVALID_ARGUMENT);
    static const char *invalid[] = {
        "", "OCTN2 16000 1 0123456789abcdef0123456789abcdef\n",
        "OCTN1 +16000 1 0123456789abcdef0123456789abcdef\n",
        "OCTN1 -16000 1 0123456789abcdef0123456789abcdef\n",
        "OCTN1 016000 1 0123456789abcdef0123456789abcdef\n",
        "OCTN1 18446744073709551616 1 0123456789abcdef0123456789abcdef\n",
        "OCTN1 999999999999999999999999999999 1 0123456789abcdef0123456789abcdef\n",
        "OCTN1 16000 +1 0123456789abcdef0123456789abcdef\n",
        "OCTN1 16000 -4294967295 0123456789abcdef0123456789abcdef\n",
        "OCTN1 16000 4294967297 0123456789abcdef0123456789abcdef\n",
        "OCTN1 16000 0 0123456789abcdef0123456789abcdef\n",
        "OCTN1 16000 3 0123456789abcdef0123456789abcdef\n",
        "OCTN1 16000 01 0123456789abcdef0123456789abcdef\n",
        "OCTN1 16000 1 0123456789abcdef0123456789abcdef",
        "OCTN1 16000 1 0123456789abcdef0123456789abcdef\n\n",
        "OCTN1 16000 1 0123456789abcdef0123456789abcdef\r\n",
        "OCTN1 16000 1 0123456789abcdef0123456789abcdef extra\n",
        " OCTN1 16000 1 0123456789abcdef0123456789abcdef\n",
        "OCTN1\t16000 1 0123456789abcdef0123456789abcdef\n",
        "OCTN1  16000 1 0123456789abcdef0123456789abcdef\n",
        "OCTN1 16000 1 0123456789ABCDEF0123456789ABCDEF\n"
    };
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++) invalid_lease(directory, invalid[i], strlen(invalid[i]), 1000);
    const char embedded[] = "OCTN1 16000 1 0123456789abcdef0123456789abcdef\n\0garbage";
    invalid_lease(directory, embedded, sizeof embedded - 1, 1000);
    const char canonical[] = "OCTN1 16000 1 0123456789abcdef0123456789abcdef\n";
    for (size_t i = 0; i < sizeof canonical - 1; i++) invalid_lease(directory, canonical, i, 1000);
    invalid_lease(directory, canonical, sizeof canonical, 1000); /* NUL is not canonical on disk */
    CHECK_EQ(oct_native_read_lease(directory, 1000, NULL, token), 0);
    CHECK_EQ(token[0], 0);
    deadline = 99;
    CHECK_EQ(oct_native_read_lease(directory, 1000, &deadline, NULL), 0);
    CHECK_EQ(deadline, 0);
}

static void test_permissions(const char *directory)
{
    char path[1024], other[1024], alias[1024], bytes[32];
    size_t length;
    snprintf(path, sizeof path, "%s/status", directory);
    snprintf(other, sizeof other, "%s/victim", directory);
    snprintf(alias, sizeof alias, "%s-alias", directory);
    CHECK_OK(oct_native_io_write(directory, "status", "safe", 4));
    CHECK_EQ(chmod(path, 0644), 0);
    CHECK_EQ(oct_native_io_read(directory, "status", bytes, sizeof bytes, &length), OCT_E_IO);
    CHECK_EQ(length, 0);
    CHECK_EQ(chmod(path, 0600), 0);
    CHECK_OK(oct_native_io_read(directory, "status", bytes, sizeof bytes, &length));
    CHECK_EQ(chmod(directory, 0755), 0);
    CHECK_EQ(oct_native_io_read(directory, "status", bytes, sizeof bytes, &length), OCT_E_IO);
    CHECK_EQ(oct_native_io_write(directory, "status", "bad", 3), OCT_E_IO);
    CHECK_EQ(oct_native_io_remove_lease(directory), OCT_E_IO);
    CHECK_EQ(chmod(directory, 0700), 0);
    CHECK_OK(oct_native_io_read(directory, "status", bytes, sizeof bytes, &length));
    /* Replacing a hardlink must not overwrite the victim; reads reject aliases. */
    CHECK_EQ(link(path, other), 0);
    struct stat st;
    CHECK_EQ(stat(path, &st), 0); CHECK_EQ(st.st_nlink, 2);
    CHECK_EQ(oct_native_io_read(directory, "status", bytes, sizeof bytes, &length), OCT_E_IO);
    CHECK_OK(oct_native_io_write(directory, "status", "fresh", 5));
    int fd = open(other, O_RDONLY);
    CHECK(fd >= 0);
    if (fd >= 0) { CHECK_EQ(read(fd, bytes, sizeof bytes), 4); CHECK(memcmp(bytes,"safe",4) == 0); close(fd); }
    CHECK_EQ(unlink(path), 0);
    CHECK_EQ(symlink(other, path), 0);
    CHECK_EQ(lstat(path, &st), 0); CHECK(S_ISLNK(st.st_mode));
    CHECK_EQ(oct_native_io_read(directory, "status", bytes, sizeof bytes, &length), OCT_E_IO);
    CHECK_OK(oct_native_io_write(directory, "status", "new", 3));
    fd = open(other, O_RDONLY);
    CHECK(fd >= 0);
    if (fd >= 0) { CHECK_EQ(read(fd,bytes,sizeof bytes),4); CHECK(memcmp(bytes,"safe",4) == 0); close(fd); }
    CHECK_EQ(symlink(directory, alias), 0);
    CHECK_EQ(lstat(alias, &st), 0); CHECK(S_ISLNK(st.st_mode));
    CHECK_EQ(oct_native_io_read(alias, "status", bytes, sizeof bytes, &length), OCT_E_IO);
    CHECK_EQ(oct_native_io_write(alias, "status", "bad", 3), OCT_E_IO);
    CHECK_EQ(oct_native_io_remove_lease(alias), OCT_E_IO);
    unlink(alias);
    unlink(other);
    /* A directory in place of a regular file cannot be opened or replaced. */
    CHECK_EQ(unlink(path), 0);
    CHECK_EQ(mkdir(path,0700),0);
    CHECK_EQ(oct_native_io_read(directory,"status",bytes,sizeof bytes,&length),OCT_E_IO);
    CHECK_EQ(oct_native_io_write(directory,"status","bad",3),OCT_E_IO);
    CHECK_EQ(rmdir(path),0);
    CHECK_OK(oct_native_io_write(directory,"status","recovered",9));
}

/* ---- Stage-precise failures: the diagnosis the device run could not make ----------------- */

#include <signal.h>
#include <sys/resource.h>

static void expect_stage(const oct_native_io_failure *f, oct_native_io_stage stage, int error)
{
    if (f->stage != stage || f->error != error) {
        fprintf(stderr, "expected stage %s errno %d, got %s errno %d\n", oct_native_io_stage_name(stage), error,
                oct_native_io_stage_name(f->stage), f->error);
    }
    CHECK_EQ(f->stage, stage);
    CHECK_EQ(f->error, error);
}

static void test_detailed_failures(void)
{
    char root[] = "/tmp/octopus-nio-XXXXXX", path[512];
    CHECK(mkdtemp(root) != NULL);
    oct_native_io_failure f;
    char bytes[64];
    size_t length = 0;

    /* What the device showed: a missing status file in a valid folder is OPEN_FILE/ENOENT. */
    CHECK_EQ(chmod(root, 0700), 0);
    CHECK_EQ(oct_native_io_read_detailed(root, "status", bytes, sizeof bytes, &length, &f), OCT_E_IO);
    expect_stage(&f, OCT_NATIVE_IO_OPEN_FILE, ENOENT);

    /* A folder that isn't there fails at OPEN_DIRECTORY, not later. */
    snprintf(path, sizeof path, "%s/missing", root);
    CHECK_EQ(oct_native_io_read_detailed(path, "status", bytes, sizeof bytes, &length, &f), OCT_E_IO);
    expect_stage(&f, OCT_NATIVE_IO_OPEN_DIRECTORY, ENOENT);

    /* A folder that opens but isn't private is a policy rejection, kept apart from a refusal by
     * the kernel or the sandbox. Both used to surface as EPERM. */
    snprintf(path, sizeof path, "%s/public", root);
    CHECK_EQ(mkdir(path, 0755), 0);
    CHECK_EQ(chmod(path, 0755), 0);
    CHECK_EQ(oct_native_io_read_detailed(path, "status", bytes, sizeof bytes, &length, &f), OCT_E_IO);
    expect_stage(&f, OCT_NATIVE_IO_VALIDATE_DIRECTORY, EPERM);
    CHECK_EQ(oct_native_io_write_detailed(path, "status", "x", 1, &f), OCT_E_IO);
    expect_stage(&f, OCT_NATIVE_IO_VALIDATE_DIRECTORY, EPERM);

    /* A file where a folder should be. */
    snprintf(path, sizeof path, "%s/plain", root);
    FILE *file = fopen(path, "w");
    CHECK(file != NULL);
    if (file) CHECK_EQ(fclose(file), 0);
    CHECK_EQ(oct_native_io_read_detailed(path, "status", bytes, sizeof bytes, &length, &f), OCT_E_IO);
    expect_stage(&f, OCT_NATIVE_IO_OPEN_DIRECTORY, ENOTDIR);

    /* A name outside the protocol never touches the filesystem. */
    CHECK_EQ(oct_native_io_write_detailed(root, "../escape", "x", 1, &f), OCT_E_INVALID_ARGUMENT);
    expect_stage(&f, OCT_NATIVE_IO_ARGUMENT, EINVAL);

    /* Success reports OK with no errno, and the bytes read back. */
    CHECK_OK(oct_native_io_write_detailed(root, "status", "{\"ok\":1}", 8, &f));
    expect_stage(&f, OCT_NATIVE_IO_OK, 0);
    CHECK_OK(oct_native_io_read_detailed(root, "status", bytes, sizeof bytes, &length, &f));
    expect_stage(&f, OCT_NATIVE_IO_OK, 0);
    CHECK_EQ(length, 8u);
}

/* A write that fails part-way must be reported as WRITE with the write's own errno: the old code
 * captured errno only after close(), which could replace it. RLIMIT_FSIZE makes write() fail
 * for real with EFBIG once the file reaches the limit. */
static void test_partial_write_is_reported_exactly(void)
{
    char root[] = "/tmp/octopus-nio-XXXXXX", path[512];
    CHECK(mkdtemp(root) != NULL);
    CHECK_EQ(chmod(root, 0700), 0);
    char payload[4096];
    memset(payload, 'x', sizeof payload);
    struct rlimit before, capped;
    CHECK_EQ(getrlimit(RLIMIT_FSIZE, &before), 0);
    void (*previous)(int) = signal(SIGXFSZ, SIG_IGN);
    capped = before;
    capped.rlim_cur = 100;
    CHECK_EQ(setrlimit(RLIMIT_FSIZE, &capped), 0);
    oct_native_io_failure f;
    oct_status status = oct_native_io_write_detailed(root, "status", payload, sizeof payload, &f);
    CHECK_EQ(setrlimit(RLIMIT_FSIZE, &before), 0);
    signal(SIGXFSZ, previous);
    CHECK_EQ(status, OCT_E_IO);
    expect_stage(&f, OCT_NATIVE_IO_WRITE, EFBIG);
    /* Nothing half-written is published, and the temporary is cleaned up. */
    snprintf(path, sizeof path, "%s/status", root);
    CHECK(access(path, F_OK) != 0);
    snprintf(path, sizeof path, "%s/.status.%ld.tmp", root, (long)getpid());
    CHECK(access(path, F_OK) != 0);
}

static void test_detailed_lease_reads(void)
{
    char root[] = "/tmp/octopus-nio-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    CHECK_EQ(chmod(root, 0700), 0);
    oct_native_io_failure f;
    uint64_t deadline = 0;
    char token[33];
    CHECK_EQ(oct_native_read_lease_detailed(root, 1000, &deadline, token, &f), 0u);
    expect_stage(&f, OCT_NATIVE_IO_OPEN_FILE, ENOENT);              /* no lease at all */
    CHECK_OK(oct_native_write_lease(root, "0123456789abcdef0123456789abcdef", 1, 1000));
    CHECK(oct_native_read_lease_detailed(root, 2000, &deadline, token, &f) > 0);
    expect_stage(&f, OCT_NATIVE_IO_OK, 0);                         /* current */
    CHECK_EQ(oct_native_read_lease_detailed(root, 1000 + OCT_NATIVE_LEASE_MS + 1, &deadline, token, &f), 0u);
    expect_stage(&f, OCT_NATIVE_IO_CONTENT, 0);                    /* expired: readable, rejected */
    CHECK_OK(oct_native_io_write(root, "lease", "garbage", 7));
    CHECK_EQ(oct_native_read_lease_detailed(root, 2000, &deadline, token, &f), 0u);
    expect_stage(&f, OCT_NATIVE_IO_CONTENT, 0);                    /* malformed */
}

/* ---- The startup signal ------------------------------------------------------------------ */

static void test_startup_signal(void)
{
    oct_native_signal s = { .phase = OCT_NATIVE_PHASE_TIMER, .identity = OCT_NATIVE_IDENTITY_COMPATIBLE,
                            .hooks = OCT_NATIVE_HOOKS_NOT_ATTEMPTED, .lease_stage = OCT_NATIVE_IO_OPEN_DIRECTORY,
                            .status_stage = OCT_NATIVE_IO_CREATE_DIRECTORY, .lease_error = EPERM,
                            .status_error = 255, .pid = 1152 };
    uint64_t word = oct_native_signal_pack(&s);
    oct_native_signal back;
    CHECK(oct_native_signal_unpack(word, &back));
    CHECK(memcmp(&s, &back, sizeof s) == 0);                       /* every field survives */
    CHECK(oct_native_signal_from_pid(&back, 1152));
    CHECK(!oct_native_signal_from_pid(&back, 43138));              /* a restarted ptpd is noticed */
    CHECK(!oct_native_signal_from_pid(&back, 0));

    CHECK(!oct_native_signal_unpack(0, &back));                    /* never posted */
    CHECK(!oct_native_signal_unpack(word ^ (1ull << 30), &back));  /* one flipped bit */
    CHECK(!oct_native_signal_unpack((word & ~0xFull) | 3u, &back)); /* a version we don't know */
    CHECK(!oct_native_signal_unpack((word & ~0xFull), &back));       /* version 0 */
    /* A word from 0.4.24 still decodes: version 1, with that version's field ranges. Built by
     * hand because pack() now writes version 2. This is the compatibility the migration promises. */
    uint64_t v1 = (word & ~0xFull) | 1u;
    v1 &= 0x00FFFFFFFFFFFFFFull;
    uint8_t check = 0;
    for (unsigned i = 0; i < 7; i++) check ^= (uint8_t)(v1 >> (8 * i));
    v1 |= (uint64_t)check << 56;
    CHECK(oct_native_signal_unpack(v1, &back));
    CHECK_EQ(back.phase, s.phase);
    CHECK_EQ(back.identity, s.identity);
    CHECK_EQ(back.pid, s.pid);
    /* But a value only version 2 can mean is refused inside a version 1 word. */
    oct_native_signal future = { .phase = OCT_NATIVE_PHASE_IDENTITY,
                                 .identity = OCT_NATIVE_IDENTITY_NO_IMAGE, .pid = 1 };
    uint64_t as_v1 = (oct_native_signal_pack(&future) & ~0xFull) | 1u;
    as_v1 &= 0x00FFFFFFFFFFFFFFull;
    check = 0;
    for (unsigned i = 0; i < 7; i++) check ^= (uint8_t)(as_v1 >> (8 * i));
    as_v1 |= (uint64_t)check << 56;
    CHECK(!oct_native_signal_unpack(as_v1, &back));
    CHECK(oct_native_signal_unpack(oct_native_signal_pack(&future), &back)); /* fine as version 2 */
    CHECK_EQ(back.identity, (uint8_t)OCT_NATIVE_IDENTITY_NO_IMAGE);

    /* Not attempted is not success. */
    oct_native_signal untried = { .phase = OCT_NATIVE_PHASE_IDENTITY,
                                  .identity = OCT_NATIVE_IDENTITY_COMPATIBLE,
                                  .lease_stage = OCT_NATIVE_IO_NOT_ATTEMPTED,
                                  .status_stage = OCT_NATIVE_IO_NOT_ATTEMPTED, .pid = 1 };
    CHECK(oct_native_signal_unpack(oct_native_signal_pack(&untried), &back));
    CHECK_EQ(back.lease_stage, (uint8_t)OCT_NATIVE_IO_NOT_ATTEMPTED);
    CHECK(back.lease_stage != (uint8_t)OCT_NATIVE_IO_OK);

    /* The detail word: the UUID actually read, where it was found, and how many images there were. */
    oct_native_detail detail = { .uuid_prefix = { 0xFF, 0xD5, 0x4F, 0xC7, 0x05, 0x08 },
                                 .image_index = 7, .image_count = 42 };
    oct_native_detail detail_back;
    CHECK(oct_native_detail_unpack(oct_native_detail_pack(&detail), &detail_back));
    CHECK(memcmp(&detail, &detail_back, sizeof detail) == 0);
    CHECK(!oct_native_detail_unpack(0, &detail_back));        /* nothing posted */
    CHECK(!oct_native_detail_unpack(1, NULL));
    oct_native_detail none = { .image_index = 255, .image_count = 3 };
    CHECK(oct_native_detail_unpack(oct_native_detail_pack(&none), &detail_back));
    CHECK_EQ(detail_back.image_index, 255);                   /* no match, still reported */
    CHECK_EQ(detail_back.uuid_prefix[0], 0);

    oct_native_signal bad = s;
    bad.phase = 15;                                                /* out of range, check recomputed */
    CHECK(!oct_native_signal_unpack(oct_native_signal_pack(&bad), &back));
    bad = s;
    bad.lease_stage = 15;
    CHECK(!oct_native_signal_unpack(oct_native_signal_pack(&bad), &back));
    CHECK(!oct_native_signal_unpack(word, NULL));

    for (int stage = OCT_NATIVE_IO_OK; stage <= OCT_NATIVE_IO_CONTENT; stage++) {
        CHECK(strcmp(oct_native_io_stage_name((oct_native_io_stage)stage), "unknown") != 0);
    }
#if !defined(__APPLE__)
    CHECK_EQ(oct_native_signal_publish(&s), OCT_E_UNSUPPORTED);
    CHECK_EQ(oct_native_signal_read(&back), OCT_E_UNSUPPORTED);
#endif
}

/* Each verdict names exactly one cause, which is what the v0.4.20 run could not get. */
static void test_signal_verdicts(void)
{
    char text[400];
    oct_native_signal s = { 0 };
    CHECK_EQ(oct_native_signal_describe(NULL, false, 1152, text, sizeof text), OCT_NATIVE_VERDICT_NO_SIGNAL);
    CHECK(strstr(text, "never posted") != NULL);

    s = (oct_native_signal){ .phase = OCT_NATIVE_PHASE_TIMER, .identity = OCT_NATIVE_IDENTITY_COMPATIBLE, .pid = 43138 & 0xFFFF };
    CHECK_EQ(oct_native_signal_describe(&s, true, 1152, text, sizeof text), OCT_NATIVE_VERDICT_STALE);

    s = (oct_native_signal){ .phase = OCT_NATIVE_PHASE_IDENTITY, .identity = OCT_NATIVE_IDENTITY_WRONG_DIGEST, .pid = 1152 };
    CHECK_EQ(oct_native_signal_describe(&s, true, 1152, text, sizeof text), OCT_NATIVE_VERDICT_IDENTITY);
    CHECK(strstr(text, "SHA-256") != NULL);

    /* The leading hypothesis for the device run: the sandbox refuses the shared folder. */
    s = (oct_native_signal){ .phase = OCT_NATIVE_PHASE_TIMER, .identity = OCT_NATIVE_IDENTITY_COMPATIBLE,
                             .lease_stage = OCT_NATIVE_IO_OPEN_DIRECTORY, .lease_error = EPERM, .pid = 1152 };
    CHECK_EQ(oct_native_signal_describe(&s, true, 1152, text, sizeof text), OCT_NATIVE_VERDICT_SANDBOX);
    CHECK(strstr(text, "open-directory") != NULL);
    /* A policy rejection of our own is not mistaken for the sandbox. */
    s.lease_stage = OCT_NATIVE_IO_VALIDATE_DIRECTORY;
    CHECK_EQ(oct_native_signal_describe(&s, true, 1152, text, sizeof text), OCT_NATIVE_VERDICT_NO_LEASE);
    /* Nor is a lease that simply isn't there yet. */
    s.lease_stage = OCT_NATIVE_IO_OPEN_FILE; s.lease_error = ENOENT;
    CHECK_EQ(oct_native_signal_describe(&s, true, 1152, text, sizeof text), OCT_NATIVE_VERDICT_NO_LEASE);

    s = (oct_native_signal){ .phase = OCT_NATIVE_PHASE_LEASE, .identity = OCT_NATIVE_IDENTITY_COMPATIBLE,
                             .status_stage = OCT_NATIVE_IO_RENAME, .status_error = EXDEV, .pid = 1152 };
    CHECK_EQ(oct_native_signal_describe(&s, true, 1152, text, sizeof text), OCT_NATIVE_VERDICT_STATUS_FAILED);
    s.status_stage = OCT_NATIVE_IO_OPEN_FILE; s.status_error = EPERM;
    CHECK_EQ(oct_native_signal_describe(&s, true, 1152, text, sizeof text), OCT_NATIVE_VERDICT_SANDBOX);

    s = (oct_native_signal){ .phase = OCT_NATIVE_PHASE_PUBLISHED, .identity = OCT_NATIVE_IDENTITY_COMPATIBLE,
                             .hooks = OCT_NATIVE_HOOKS_REFUSED, .pid = 1152 };
    CHECK_EQ(oct_native_signal_describe(&s, true, 1152, text, sizeof text), OCT_NATIVE_VERDICT_HOOKS_REFUSED);
    s.hooks = OCT_NATIVE_HOOKS_INSTALLED; s.phase = OCT_NATIVE_PHASE_HOOKED;
    CHECK_EQ(oct_native_signal_describe(&s, true, 1152, text, sizeof text), OCT_NATIVE_VERDICT_RUNNING);
    /* An unknown observed pid skips the staleness check rather than guessing. */
    CHECK_EQ(oct_native_signal_describe(&s, true, 0, text, sizeof text), OCT_NATIVE_VERDICT_RUNNING);
    CHECK_EQ(oct_native_signal_describe(&s, true, 1152, NULL, 0), OCT_NATIVE_VERDICT_RUNNING); /* no buffer */
}

/*
 * Authorization carried by notification. The file-based lease cannot be read inside ptpd, so this
 * is the channel that replaces it; it is unauthenticated, and these tests pin down exactly what it
 * is therefore allowed to switch on.
 */
static void test_lease_word(void)
{
    oct_native_lease_word out;
    const uint64_t now = 1000000;

    /* A current observe lease is honoured. */
    oct_native_lease_word observe = { .mode = OCT_NATIVE_LEASE_OBSERVE,
                                      .deadline_ms = now + 10000, .generation = 7 };
    uint64_t word = oct_native_lease_pack(&observe);
    CHECK(oct_native_lease_unpack(word, &out));
    CHECK_EQ(out.mode, (uint8_t)OCT_NATIVE_LEASE_OBSERVE);
    CHECK_EQ(out.deadline_ms, observe.deadline_ms);
    CHECK_EQ(out.generation, observe.generation);
    CHECK_EQ(oct_native_lease_effective(&out, now), OCT_NATIVE_LEASE_OBSERVE);

    /* The synthetic tree may be exposed: it is six objects this code builds from constants, with
     * no user data in it, so a forged grant would show a host Octopus's own sample files. */
    oct_native_lease_word synthetic = { .mode = OCT_NATIVE_LEASE_EXPOSE_SYNTHETIC,
                                        .deadline_ms = now + 1000 };
    CHECK(oct_native_lease_unpack(oct_native_lease_pack(&synthetic), &out));
    CHECK_EQ(oct_native_lease_effective(&out, now), OCT_NATIVE_LEASE_EXPOSE_SYNTHETIC);
    CHECK_EQ(oct_native_lease_effective(&out, now + 2000), OCT_NATIVE_LEASE_NONE); /* still expires */

    /* Real files are refused however they are asked for, because any process can post here, and
     * the request grants nothing rather than being downgraded to something lesser. */
    oct_native_lease_word files = { .mode = OCT_NATIVE_LEASE_EXPOSE_FILESYSTEM,
                                    .deadline_ms = now + 1000 };
    CHECK(oct_native_lease_unpack(oct_native_lease_pack(&files), &out)); /* decodes */
    CHECK_EQ(out.mode, (uint8_t)OCT_NATIVE_LEASE_EXPOSE_FILESYSTEM);     /* and says so */
    CHECK_EQ(oct_native_lease_effective(&out, now), OCT_NATIVE_LEASE_NONE); /* but grants nothing */

    /* Expiry, to the millisecond. */
    CHECK_EQ(oct_native_lease_effective(&observe, observe.deadline_ms - 1), OCT_NATIVE_LEASE_OBSERVE);
    CHECK_EQ(oct_native_lease_effective(&observe, observe.deadline_ms), OCT_NATIVE_LEASE_NONE);
    CHECK_EQ(oct_native_lease_effective(&observe, observe.deadline_ms + 60000), OCT_NATIVE_LEASE_NONE);

    /* Replay of a word from an earlier boot: the deadline is milliseconds since boot, so after a
     * restart the clock is small again and a large deadline is too far ahead to honour. */
    oct_native_lease_word old_boot = { .mode = OCT_NATIVE_LEASE_OBSERVE, .deadline_ms = 900000000 };
    CHECK_EQ(oct_native_lease_effective(&old_boot, 5000), OCT_NATIVE_LEASE_NONE);

    /* A word granting more than the app is allowed to grant is not honoured. */
    oct_native_lease_word greedy = { .mode = OCT_NATIVE_LEASE_OBSERVE,
                                     .deadline_ms = now + OCT_NATIVE_LEASE_MS + 1 };
    CHECK_EQ(oct_native_lease_effective(&greedy, now), OCT_NATIVE_LEASE_NONE);
    oct_native_lease_word exact = { .mode = OCT_NATIVE_LEASE_OBSERVE,
                                    .deadline_ms = now + OCT_NATIVE_LEASE_MS };
    CHECK_EQ(oct_native_lease_effective(&exact, now), OCT_NATIVE_LEASE_OBSERVE);

    /* No lease, no deadline, nothing posted, a corrupted word, an unknown version or mode. */
    oct_native_lease_word none = { .mode = OCT_NATIVE_LEASE_NONE, .deadline_ms = now + 1000 };
    CHECK_EQ(oct_native_lease_effective(&none, now), OCT_NATIVE_LEASE_NONE);
    oct_native_lease_word undated = { .mode = OCT_NATIVE_LEASE_OBSERVE, .deadline_ms = 0 };
    CHECK_EQ(oct_native_lease_effective(&undated, now), OCT_NATIVE_LEASE_NONE);
    CHECK_EQ(oct_native_lease_effective(NULL, now), OCT_NATIVE_LEASE_NONE);
    CHECK(!oct_native_lease_unpack(0, &out));
    CHECK(!oct_native_lease_unpack(word ^ (1ull << 20), &out));
    CHECK(!oct_native_lease_unpack((word & ~0xFull) | 2u, &out));
    CHECK(!oct_native_lease_unpack(word, NULL));
    uint64_t bad_mode = (word & ~0xF0ull) | (0xFull << 4);
    uint8_t fixed = 0;
    for (unsigned i = 0; i < 7; i++) fixed ^= (uint8_t)(bad_mode >> (8 * i));
    bad_mode = (bad_mode & 0x00FFFFFFFFFFFFFFull) | (uint64_t)fixed << 56;
    CHECK(!oct_native_lease_unpack(bad_mode, &out)); /* check byte valid, mode is not */

    /* The app restarting shows up as a different generation, so a reader can notice. */
    oct_native_lease_word restarted = observe;
    restarted.generation = 8;
    CHECK(oct_native_lease_pack(&restarted) != word);
    /* The deadline field holds the whole 40 bits it claims. */
    oct_native_lease_word wide = { .mode = OCT_NATIVE_LEASE_OBSERVE, .deadline_ms = 0xFFFFFFFFFFull };
    CHECK(oct_native_lease_unpack(oct_native_lease_pack(&wide), &out));
    CHECK_EQ(out.deadline_ms, 0xFFFFFFFFFFull);
    CHECK_EQ(oct_native_lease_pack(NULL), 0u);
}

/* The companion's runtime state, which replaces the status file the sandbox blocks. The
 * synthetic-storage toggle waits on the eligibility bit, so this is what unlocks it. */
static void test_runtime_word(void)
{
    oct_native_runtime out;
    oct_native_runtime live = { .mode = 2,
                                .flags = OCT_NATIVE_RUNTIME_HOOKED | OCT_NATIVE_RUNTIME_ELIGIBLE |
                                         OCT_NATIVE_RUNTIME_SAW_IDS | OCT_NATIVE_RUNTIME_SAW_INFO,
                                .commands = 1234, .responses = 1200, .provider_events = 42 };
    CHECK(oct_native_runtime_unpack(oct_native_runtime_pack(&live), &out));
    CHECK(memcmp(&live, &out, sizeof live) == 0);
    CHECK(out.flags & OCT_NATIVE_RUNTIME_ELIGIBLE);

    /* Every field at its maximum survives, so a busy session is not mistaken for an empty one. */
    oct_native_runtime full = { .mode = 2, .flags = 0xF, .commands = 0xFFFF,
                                .responses = 0xFFFF, .provider_events = 0xFF };
    CHECK(oct_native_runtime_unpack(oct_native_runtime_pack(&full), &out));
    CHECK(memcmp(&full, &out, sizeof full) == 0);

    /* Nothing observed yet is distinguishable from nothing posted. */
    oct_native_runtime quiet = { .mode = 1, .flags = OCT_NATIVE_RUNTIME_HOOKED };
    CHECK(oct_native_runtime_unpack(oct_native_runtime_pack(&quiet), &out));
    CHECK_EQ(out.commands, 0);
    CHECK_EQ(out.mode, 1);
    CHECK(!(out.flags & OCT_NATIVE_RUNTIME_ELIGIBLE));
    CHECK(!oct_native_runtime_unpack(0, &out));                 /* never posted */

    /* Corrupted, unknown version, impossible mode. */
    uint64_t word = oct_native_runtime_pack(&live);
    CHECK(!oct_native_runtime_unpack(word ^ (1ull << 20), &out));
    CHECK(!oct_native_runtime_unpack((word & ~0xFull) | 2u, &out));
    CHECK(!oct_native_runtime_unpack(word, NULL));
    CHECK_EQ(oct_native_runtime_pack(NULL), 0u);
    oct_native_runtime impossible = { .mode = 3 };
    uint64_t bad = oct_native_runtime_pack(&impossible);
    CHECK(!oct_native_runtime_unpack(bad, &out)); /* check byte valid, mode is not */
}

void run_native_io_tests(void)
{
    test_detailed_failures();
    test_partial_write_is_reported_exactly();
    test_detailed_lease_reads();
    test_startup_signal();
    test_signal_verdicts();
    test_lease_word();
    test_runtime_word();
    char directory[512], path[1024];
    const char *temporary = getenv("OCT_NATIVE_TEST_TMP");
    int count = snprintf(directory, sizeof directory, "%s/octopus-native-XXXXXX", temporary ? temporary : "/tmp");
    CHECK(count > 0 && (size_t)count < sizeof directory);
    CHECK(mkdtemp(directory) != NULL);
    test_parse_lease();
    test_round_trip(directory);
    test_leases(directory);
    test_permissions(directory);
    snprintf(path,sizeof path,"%s/status",directory); unlink(path);
    CHECK_OK(oct_native_io_remove_lease(directory));
    CHECK_EQ(rmdir(directory),0);
    CHECK_OK(oct_native_io_remove_lease(directory)); /* absent directory is already revoked */
}

#if defined(OCT_NATIVE_IO_TEST_MAIN) || defined(OCT_NATIVE_LEASE_TEST_MAIN)
int test_checks;
int test_failures;
int main(void)
{
#if defined(OCT_NATIVE_LEASE_TEST_MAIN)
    test_parse_lease();
#else
    run_native_io_tests();
#endif
    printf("Native lease IPC: %d checks, %d failures\n",test_checks,test_failures);
    return test_failures ? 1 : 0;
}
#endif


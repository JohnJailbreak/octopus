#include "test_support.h"

#include "../../Core/oct_log.h"

#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

static char log_directory[256];
static char log_path[300];

static void fresh_directory(void)
{
    snprintf(log_directory, sizeof log_directory, "/tmp/octopus-log-XXXXXX");
    CHECK(mkdtemp(log_directory) != NULL);
    snprintf(log_path, sizeof log_path, "%s/octopus-debug.log", log_directory);
}

static void remove_directory(void)
{
    char path[400];
    unlink(log_path);
    for (unsigned i = 1; i <= 9; i++) {
        snprintf(path, sizeof path, "%s/octopus-debug.%u.log", log_directory, i);
        unlink(path);
    }
    rmdir(log_directory);
}

static char *read_text(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    rewind(file);
    char *text = calloc(1, (size_t)(size > 0 ? size : 0) + 1);
    if (text && size > 0 && fread(text, 1, (size_t)size, file) != (size_t)size) text[0] = '\0';
    fclose(file);
    return text;
}

static size_t count_lines(const char *text)
{
    size_t lines = 0;
    for (const char *p = text; p && *p; p++) lines += *p == '\n';
    return lines;
}

/* 2026-11-08T19:51:02.123Z LEVEL ... */
static bool line_well_formed(const char *line)
{
    static const char shape[] = "dddd-dd-ddTdd:dd:dd.dddZ ";
    for (size_t i = 0; shape[i]; i++) {
        if (shape[i] == 'd' ? !(line[i] >= '0' && line[i] <= '9') : line[i] != shape[i]) return false;
    }
    const char *level = line + 25;
    return strncmp(level, "DEBUG ", 6) == 0 || strncmp(level, "INFO  ", 6) == 0 || strncmp(level, "WARN  ", 6) == 0 ||
           strncmp(level, "ERROR ", 6) == 0;
}

/* The numbers in "line line 0123" (the event, then the message), first and last. */
static int line_number(const char *text, bool last)
{
    int number = -1;
    for (const char *p = text ? strstr(text, "line line ") : NULL; p; p = strstr(p + 1, "line line ")) {
        number = atoi(p + 10);
        if (!last) break;
    }
    return number;
}

static void open_log(size_t max_bytes, unsigned keep, oct_log_level level)
{
    oct_log_config config = { log_path, max_bytes, keep, level };
    CHECK_OK(oct_log_open(&config));
}

static void test_format_and_levels(void)
{
    fresh_directory();
    open_log(0, OCT_LOG_DEFAULT_KEEP, OCT_LOG_INFO);
    oct_logf(OCT_LOG_DEBUG, "link", "transaction", "dropped at INFO");
    oct_logf(OCT_LOG_INFO, "link", "transaction", "op=0x%04X tid=%u", 0x1001u, 1u);
    oct_logf(OCT_LOG_WARN, "my component", "", "two\nlines\tand a tab");
    oct_log_message(OCT_LOG_ERROR, NULL, "failure", "the reason");
    oct_log_stats stats;
    oct_log_get_stats(&stats);
    CHECK_EQ(stats.lines, 3);
    CHECK_EQ(stats.filtered, 1);
    CHECK(stats.file_open);
    char *text = read_text(log_path);
    CHECK(text != NULL);
    CHECK_EQ(count_lines(text), 3);
    CHECK_EQ(stats.file_bytes, text ? strlen(text) : 0);
    if (text) {
        char *second = strchr(text, '\n') + 1, *third = strchr(second, '\n') + 1;
        CHECK(line_well_formed(text) && line_well_formed(second) && line_well_formed(third));
        CHECK(strstr(text, "INFO  link       transaction op=0x1001 tid=1\n") != NULL);
        CHECK(strstr(second, "WARN  my_component - two lines and a tab\n") == second + 25);
        CHECK(strstr(third, "ERROR -          failure the reason\n") == third + 25);
        CHECK(strstr(text, "dropped at INFO") == NULL);
    }
    free(text);

    oct_log_set_level(OCT_LOG_DEBUG);
    CHECK(oct_log_enabled(OCT_LOG_DEBUG));
    oct_logf(OCT_LOG_DEBUG, "link", "transaction", "now kept");
    text = read_text(log_path);
    CHECK(text && strstr(text, "DEBUG link       transaction now kept\n"));
    free(text);

    /* A long line is cut to the limit on a UTF-8 boundary and marked. */
    char message[1400] = "";
    for (int i = 0; i < 600; i++) strcat(message, "\xC3\xA9");
    oct_log_message(OCT_LOG_INFO, "long", "line", message);
    char recent[4096];
    oct_log_recent(recent, sizeof recent);
    char *last = recent + strlen(recent) - 1;
    *last = '\0';
    char *start = strrchr(recent, '\n');
    start = start ? start + 1 : recent;
    CHECK(strlen(start) <= OCT_LOG_LINE_MAX);
    CHECK(strlen(start) > OCT_LOG_LINE_MAX - 8);
    CHECK(strcmp(start + strlen(start) - 3, "...") == 0);
    CHECK(((unsigned char)start[strlen(start) - 4] & 0xC0u) == 0x80u); /* ends on a whole character */
    CHECK(((unsigned char)start[strlen(start) - 5] & 0xE0u) == 0xC0u);
    oct_log_get_stats(&stats);
    CHECK_EQ(stats.truncated, 1);
    oct_log_close();
    remove_directory();
}

static void test_persistence(void)
{
    fresh_directory();
    open_log(0, 3, OCT_LOG_INFO);
    oct_logf(OCT_LOG_INFO, "app", "launch", "first");
    oct_logf(OCT_LOG_INFO, "app", "launch", "second");
    oct_log_close();
    oct_logf(OCT_LOG_INFO, "app", "launch", "while closed"); /* memory only */
    open_log(0, 3, OCT_LOG_INFO);                            /* the next launch */
    oct_logf(OCT_LOG_INFO, "app", "launch", "third");
    char *text = read_text(log_path);
    CHECK(text != NULL);
    CHECK_EQ(count_lines(text), 3);
    if (text) {
        char *first = strstr(text, "first"), *second = strstr(text, "second"), *third = strstr(text, "third");
        CHECK(first && second && third && first < second && second < third);
        CHECK(strstr(text, "while closed") == NULL);
    }
    oct_log_stats stats;
    oct_log_get_stats(&stats);
    CHECK_EQ(stats.file_bytes, text ? strlen(text) : 0);
    free(text);
    oct_log_close();
    remove_directory();
}

static void test_rotation(void)
{
    fresh_directory();
    open_log(2048, 2, OCT_LOG_INFO);
    for (int i = 0; i < 300; i++) oct_logf(OCT_LOG_INFO, "rotate", "line", "line %04d padding padding", i);
    char rotated1[400], rotated2[400], rotated3[400];
    CHECK(oct_log_file_path(1, rotated1, sizeof rotated1));
    CHECK(oct_log_file_path(2, rotated2, sizeof rotated2));
    CHECK(!oct_log_file_path(3, rotated3, sizeof rotated3));
    CHECK(strstr(rotated1, "octopus-debug.1.log") != NULL);
    struct stat info;
    CHECK(stat(log_path, &info) == 0 && info.st_size > 0 && info.st_size <= 2048);
    CHECK(stat(rotated1, &info) == 0 && info.st_size > 1500 && info.st_size <= 2048);
    CHECK(stat(rotated2, &info) == 0 && info.st_size <= 2048);
    snprintf(rotated3, sizeof rotated3, "%s/octopus-debug.3.log", log_directory);
    CHECK(stat(rotated3, &info) != 0);
    oct_log_stats stats;
    oct_log_get_stats(&stats);
    CHECK(stats.rotations >= 3);
    char *newest = read_text(log_path), *older = read_text(rotated1), *oldest = read_text(rotated2);
    CHECK(newest && strstr(newest, "line 0299"));
    /* Order holds across files: .2 ends just before .1 begins, and .1 just before the current file. */
    CHECK(line_number(oldest, true) >= 0);
    CHECK_EQ(line_number(oldest, true) + 1, line_number(older, false));
    CHECK_EQ(line_number(older, true) + 1, line_number(newest, false));
    CHECK_EQ(line_number(newest, true), 299);
    free(newest);
    free(older);
    free(oldest);
    oct_log_close();

    /* keep 0: the file starts over instead of rotating, and stays under the limit. */
    open_log(1024, 0, OCT_LOG_INFO);
    for (int i = 0; i < 100; i++) oct_logf(OCT_LOG_INFO, "rotate", "line", "again %d with some padding text", i);
    CHECK(stat(log_path, &info) == 0 && info.st_size <= 1024);
    CHECK(!oct_log_file_path(1, rotated1, sizeof rotated1));

    /* Clearing empties the file and removes the rotated ones. */
    open_log(2048, 2, OCT_LOG_INFO);
    for (int i = 0; i < 100; i++) oct_logf(OCT_LOG_INFO, "rotate", "line", "line %04d padding padding", i);
    CHECK_OK(oct_log_clear());
    CHECK(stat(log_path, &info) == 0 && info.st_size == 0);
    CHECK(stat(rotated1, &info) != 0);
    char recent[64];
    CHECK_EQ(oct_log_recent(recent, sizeof recent), 0);
    oct_logf(OCT_LOG_INFO, "rotate", "line", "after clear");
    char *text = read_text(log_path);
    CHECK(text && count_lines(text) == 1 && strstr(text, "after clear"));
    free(text);
    oct_log_close();
    remove_directory();
}

#define THREADS 8
#define LINES_PER_THREAD 400

static void *writer(void *context)
{
    int id = (int)(intptr_t)context;
    for (int i = 0; i < LINES_PER_THREAD; i++) {
        oct_logf(OCT_LOG_INFO, "thread", "write", "thread=%d seq=%04d %s", id, i, "padding to make lines longer");
    }
    return NULL;
}

static void *reader(void *context)
{
    (void)context;
    char buffer[8192];
    oct_log_stats stats;
    for (int i = 0; i < 300; i++) {
        oct_log_recent(buffer, sizeof buffer);
        oct_log_get_stats(&stats);
    }
    return NULL;
}

static void test_concurrency(void)
{
    fresh_directory();
    open_log(8u * 1024u * 1024u, 1, OCT_LOG_INFO);
    pthread_t threads[THREADS + 1];
    for (int i = 0; i < THREADS; i++) CHECK(pthread_create(&threads[i], NULL, writer, (void *)(intptr_t)i) == 0);
    CHECK(pthread_create(&threads[THREADS], NULL, reader, NULL) == 0);
    for (int i = 0; i <= THREADS; i++) pthread_join(threads[i], NULL);
    char *text = read_text(log_path);
    CHECK(text != NULL);
    CHECK_EQ(count_lines(text), THREADS * LINES_PER_THREAD);
    int next[THREADS] = { 0 };
    bool intact = true;
    for (char *line = text; line && *line;) {
        char *end = strchr(line, '\n');
        if (!end) break;
        *end = '\0';
        int id = -1, seq = -1;
        char *field = strstr(line, "thread=");
        if (!line_well_formed(line) || !field || sscanf(field, "thread=%d seq=%d", &id, &seq) != 2 || id < 0 ||
            id >= THREADS || seq != next[id] || !strstr(line, "padding to make lines longer")) {
            intact = false;
        } else {
            next[id]++;
        }
        line = end + 1;
    }
    CHECK(intact); /* no torn or interleaved lines, and each thread's lines in its own order */
    for (int i = 0; i < THREADS; i++) CHECK_EQ(next[i], LINES_PER_THREAD);
    free(text);
    char recent[OCT_LOG_RECENT_LINES * (OCT_LOG_LINE_MAX + 1) + 1];
    oct_log_recent(recent, sizeof recent);
    CHECK_EQ(count_lines(recent), OCT_LOG_RECENT_LINES);
    oct_log_close();
    remove_directory();
}

static void test_errors(void)
{
    fresh_directory();
    oct_log_config config = { log_directory, 0, 3, OCT_LOG_INFO }; /* a directory, not a file */
    CHECK_EQ(oct_log_open(&config), OCT_E_IO);
    oct_log_stats stats;
    oct_log_get_stats(&stats);
    CHECK(!stats.file_open);
    CHECK(strstr(stats.last_error, "Opening the log") == stats.last_error);
    oct_logf(OCT_LOG_WARN, "app", "fallback", "kept in memory");
    char recent[8192];
    oct_log_recent(recent, sizeof recent);
    CHECK(strstr(recent, "kept in memory") != NULL);

    char missing[400];
    snprintf(missing, sizeof missing, "%s/no/such/folder/octopus-debug.log", log_directory);
    config.path = missing;
    CHECK_EQ(oct_log_open(&config), OCT_E_IO);

    char long_path[2000];
    memset(long_path, 'a', sizeof long_path - 1);
    long_path[0] = '/';
    long_path[sizeof long_path - 1] = '\0';
    config.path = long_path;
    CHECK_EQ(oct_log_open(&config), OCT_E_INVALID_ARGUMENT);

    config.path = NULL; /* memory only is a valid choice */
    CHECK_OK(oct_log_open(&config));
    char path[64];
    CHECK(!oct_log_file_path(0, path, sizeof path));
    oct_log_message(OCT_LOG_INFO, NULL, NULL, NULL);
    oct_log_message(OCT_LOG_INFO, "app", "null", NULL);
    oct_log_get_stats(&stats);
    CHECK_EQ(stats.lines, 2);

    if (access("/dev/full", W_OK) == 0) { /* writes fail with ENOSPC: counted, reported, still in memory */
        config.path = "/dev/full";
        CHECK_OK(oct_log_open(&config));
        oct_logf(OCT_LOG_ERROR, "app", "full", "disk full");
        oct_log_get_stats(&stats);
        CHECK_EQ(stats.write_errors, 1);
        CHECK(strstr(stats.last_error, "Writing the log") == stats.last_error);
        oct_log_recent(recent, sizeof recent);
        CHECK(strstr(recent, "disk full") != NULL);
    }
    oct_log_close();
    remove_directory();
}

void run_log_tests(void)
{
    test_format_and_levels();
    test_persistence();
    test_rotation();
    test_concurrency();
    test_errors();
    oct_log_config memory = { NULL, 0, 0, OCT_LOG_INFO }; /* the other suites log to memory */
    oct_log_open(&memory);
}

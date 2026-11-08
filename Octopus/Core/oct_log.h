/*
 * Octopus debug log: one process-wide, thread-safe log shared by the C core and the app.
 *
 * Each line is:   2026-11-08T19:51:02.123Z INFO  link       transaction op=0x1007 tid=4 rc=0x2001 ...
 * that is, a UTC timestamp with milliseconds, the level, a component and an event identifier
 * ([A-Za-z0-9._-], at most 24 characters), and a one-line message. Lines go to a file that
 * persists across launches and rotates by size (octopus-debug.log, then octopus-debug.1.log ...)
 * and to a fixed ring of recent lines in memory, so the log still works when the file can't be
 * opened. Memory use is fixed: no line is longer than OCT_LOG_LINE_MAX bytes.
 *
 * Callers must not log file contents, credentials or personal information: Octopus logs
 * operation codes, handles, sizes, states and reasons, never names or bytes of files.
 */
#ifndef OCT_LOG_H
#define OCT_LOG_H

#include "oct_store.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum oct_log_level {
    OCT_LOG_DEBUG = 0,
    OCT_LOG_INFO = 1,
    OCT_LOG_WARN = 2,
    OCT_LOG_ERROR = 3
} oct_log_level;

#define OCT_LOG_LINE_MAX      512u                /* longer lines are cut and end in "..." */
#define OCT_LOG_NAME_MAX      24u                 /* component and event identifiers */
#define OCT_LOG_RECENT_LINES  200u                /* kept in memory */
#define OCT_LOG_DEFAULT_BYTES (512u * 1024u)      /* a file rotates before it grows past this */
#define OCT_LOG_DEFAULT_KEEP  3u                  /* rotated files kept; .1 is the newest */
#define OCT_LOG_PATH_MAX      1024u

typedef struct oct_log_config {
    const char   *path;       /* the log file; NULL logs to memory only */
    size_t        max_bytes;  /* 0 selects OCT_LOG_DEFAULT_BYTES; at least 1024 */
    unsigned      keep;       /* rotated files to keep, 0 to 9 (0 starts the file over) */
    oct_log_level level;      /* lines below it are dropped */
} oct_log_config;

typedef struct oct_log_stats {
    bool          file_open;
    oct_log_level level;
    uint64_t      lines;          /* lines logged since the log was opened */
    uint64_t      filtered;       /* lines below the level */
    uint64_t      truncated;      /* lines cut to OCT_LOG_LINE_MAX */
    uint64_t      rotations;
    uint64_t      write_errors;
    uint64_t      file_bytes;     /* size of the current file */
    char          path[OCT_LOG_PATH_MAX];
    char          last_error[160];
} oct_log_stats;

/* Opens (or switches to) a log file, appending to what is there. On failure it returns OCT_E_IO,
 * records why in the stats, and keeps logging to memory. */
oct_status    oct_log_open(const oct_log_config *config);
void          oct_log_close(void);
void          oct_log_set_level(oct_log_level level);
oct_log_level oct_log_get_level(void);
bool          oct_log_enabled(oct_log_level level);

#if defined(__GNUC__) || defined(__clang__)
#define OCT_LOG_PRINTF(f, a) __attribute__((format(printf, f, a)))
#else
#define OCT_LOG_PRINTF(f, a)
#endif

void oct_logf(oct_log_level level, const char *component, const char *event, const char *format, ...)
    OCT_LOG_PRINTF(4, 5);
/* The same, with a finished message: what Swift calls, as it can't call variadic C functions. */
void oct_log_message(oct_log_level level, const char *component, const char *event, const char *message);

/* Empties the current file and removes the rotated ones. */
oct_status oct_log_clear(void);
/* The most recent lines kept in memory that fit, oldest first, one per line, NUL-terminated.
 * Returns the length written. */
size_t     oct_log_recent(char *buffer, size_t capacity);
void       oct_log_get_stats(oct_log_stats *out);
/* The path of the current file (number 0) or of rotated file `number`; false when there is no
 * file or the number is beyond `keep`. */
bool       oct_log_file_path(unsigned number, char *out, size_t capacity);
const char *oct_log_level_name(oct_log_level level);

#ifdef __cplusplus
}
#endif

#endif /* OCT_LOG_H */

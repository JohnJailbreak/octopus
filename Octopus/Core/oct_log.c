/* oct_log.c - the debug log. See oct_log.h. */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#elif !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "oct_log.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LINE_BUFFER (OCT_LOG_LINE_MAX + 1u)
#define ROTATED_MAX 9u

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_int g_level = OCT_LOG_INFO;
static _Atomic uint64_t g_filtered;
static int g_fd = -1;
static char g_path[OCT_LOG_PATH_MAX];
static size_t g_max = OCT_LOG_DEFAULT_BYTES;
static unsigned g_keep = OCT_LOG_DEFAULT_KEEP;
static uint64_t g_size;
static char g_ring[OCT_LOG_RECENT_LINES][LINE_BUFFER]; /* lines without their newline */
static size_t g_ring_head, g_ring_count;
static oct_log_stats g_stats;

const char *oct_log_level_name(oct_log_level level)
{
    switch (level) {
    case OCT_LOG_DEBUG: return "DEBUG";
    case OCT_LOG_INFO: return "INFO";
    case OCT_LOG_WARN: return "WARN";
    case OCT_LOG_ERROR: return "ERROR";
    }
    return "INFO";
}

static oct_log_level clamp_level(oct_log_level level)
{
    return level <= OCT_LOG_ERROR ? level : OCT_LOG_ERROR;
}

void oct_log_set_level(oct_log_level level) { atomic_store(&g_level, (int)clamp_level(level)); }
oct_log_level oct_log_get_level(void) { return (oct_log_level)atomic_load(&g_level); }
bool oct_log_enabled(oct_log_level level) { return (int)level >= atomic_load(&g_level); }

static void set_error_locked(const char *what, int error)
{
    snprintf(g_stats.last_error, sizeof g_stats.last_error, "%s: %s", what, strerror(error));
}

/* The current file for number 0, otherwise octopus-debug.<number>.log beside it. */
static bool rotated_path(char *out, size_t capacity, const char *path, unsigned number)
{
    int n;
    size_t length = strlen(path);
    if (number == 0) {
        n = snprintf(out, capacity, "%s", path);
    } else if (length > 4 && strcmp(path + length - 4, ".log") == 0) {
        n = snprintf(out, capacity, "%.*s.%u.log", (int)(length - 4), path, number);
    } else {
        n = snprintf(out, capacity, "%s.%u", path, number);
    }
    return n > 0 && (size_t)n < capacity;
}

static bool write_all(int fd, const char *bytes, size_t length)
{
    while (length > 0) {
        ssize_t written = write(fd, bytes, length);
        if (written < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        bytes += written;
        length -= (size_t)written;
    }
    return true;
}

static void close_locked(void)
{
    if (g_fd >= 0) close(g_fd);
    g_fd = -1;
    g_stats.file_open = false;
}

/* Shifts octopus-debug.log to .1, .1 to .2 and so on, dropping the oldest, then starts a new file. When the file
 * can't be moved, it starts over so it never grows without bound. */
static void rotate_locked(void)
{
    char from[OCT_LOG_PATH_MAX + 16], to[OCT_LOG_PATH_MAX + 16];
    bool moved = false;
    close(g_fd);
    g_fd = -1;
    if (g_keep > 0 && rotated_path(to, sizeof to, g_path, g_keep)) {
        unlink(to);
        for (unsigned number = g_keep; number > 1; number--) {
            if (rotated_path(from, sizeof from, g_path, number - 1) && rotated_path(to, sizeof to, g_path, number) &&
                rename(from, to) != 0 && errno != ENOENT) {
                set_error_locked("Rotating the log", errno);
            }
        }
        if (rotated_path(to, sizeof to, g_path, 1)) moved = rename(g_path, to) == 0;
        if (!moved) set_error_locked("Rotating the log", errno);
    }
    g_fd = open(g_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | (moved ? 0 : O_TRUNC), 0644);
    if (g_fd < 0) {
        set_error_locked("Reopening the log", errno);
        g_stats.file_open = false;
    }
    g_size = 0;
    g_stats.rotations++;
}

/* Opens the file again at its path, after the handle went bad or the file vanished. */
static bool reopen_locked(void)
{
    if (!g_path[0]) return false;
    if (g_fd >= 0) close(g_fd);
    g_fd = open(g_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    g_stats.reopens++;
    if (g_fd < 0) {
        set_error_locked("Reopening the log", errno);
        g_stats.file_open = false;
        return false;
    }
    struct stat info;
    g_size = fstat(g_fd, &info) == 0 && S_ISREG(info.st_mode) ? (uint64_t)info.st_size : 0;
    g_stats.file_open = true;
    return true;
}

/* Errors that a fresh handle can cure; a full disk isn't one of them. */
static bool handle_error(int error)
{
    return error == EBADF || error == EIO || error == ENOENT || error == ESTALE || error == EPIPE;
}

static void write_locked(const char *line, size_t length)
{
    size_t slot = g_ring_count < OCT_LOG_RECENT_LINES ? (g_ring_head + g_ring_count) % OCT_LOG_RECENT_LINES
                                                      : g_ring_head;
    memcpy(g_ring[slot], line, length + 1);
    if (g_ring_count < OCT_LOG_RECENT_LINES) {
        g_ring_count++;
    } else {
        g_ring_head = (g_ring_head + 1) % OCT_LOG_RECENT_LINES;
    }
    g_stats.lines++;
    if (g_fd < 0) return;
    if (g_size > 0 && g_size + length + 1 > g_max) rotate_locked();
    if (g_fd < 0) return;
    char buffer[LINE_BUFFER + 1];
    memcpy(buffer, line, length);
    buffer[length] = '\n';
    if (!write_all(g_fd, buffer, length + 1)) {
        int error = errno;
        if (handle_error(error) && reopen_locked() && write_all(g_fd, buffer, length + 1)) {
            g_size += length + 1;
            return;
        }
        g_stats.write_errors++;
        set_error_locked("Writing the log", error);
        return;
    }
    g_size += length + 1;
}

/* Identifiers keep [A-Za-z0-9._-]; anything else becomes '_'. */
static void copy_name(char *out, size_t capacity, const char *name)
{
    size_t n = 0;
    if (name) {
        for (; name[n] && n + 1 < capacity && n < OCT_LOG_NAME_MAX; n++) {
            char c = name[n];
            bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                        c == '_' || c == '-';
            out[n] = keep ? c : (char)'_';
        }
    }
    if (n == 0) out[n++] = '-';
    out[n] = '\0';
}

/* Builds the whole line. Control characters in the message become spaces, so a line never spans two; a message
 * that doesn't fit is cut on a UTF-8 boundary and ends in "...". */
static size_t format_line(char *line, oct_log_level level, const char *component, const char *event,
                          const char *message, bool *truncated)
{
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    time_t seconds = now.tv_sec;
    struct tm utc;
    gmtime_r(&seconds, &utc);
    char comp[OCT_LOG_NAME_MAX + 1], ev[OCT_LOG_NAME_MAX + 1];
    copy_name(comp, sizeof comp, component);
    copy_name(ev, sizeof ev, event);
    int head = snprintf(line, LINE_BUFFER, "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ %-5s %-10s %s ",
                        utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec,
                        (long)(now.tv_nsec / 1000000), oct_log_level_name(level), comp, ev);
    size_t length = head > 0 ? (size_t)head : 0;
    if (length > LINE_BUFFER - 1) length = LINE_BUFFER - 1;
    const char *text = message ? message : "";
    size_t room = LINE_BUFFER - 1 - length;
    size_t take = strlen(text);
    bool cut = take > room;
    if (cut) {
        take = room >= 3 ? room - 3 : 0;
        while (take > 0 && ((unsigned char)text[take] & 0xC0u) == 0x80u) take--;
    }
    for (size_t i = 0; i < take && length < LINE_BUFFER - 1; i++) {
        unsigned char c = (unsigned char)text[i];
        char shown = (char)c;
        if (c < 0x20u || c == 0x7Fu) shown = ' ';
        line[length++] = shown;
    }
    if (cut && length + 3 <= LINE_BUFFER - 1) {
        memcpy(line + length, "...", 3);
        length += 3;
    }
    while (length > 0 && line[length - 1] == ' ') length--;
    line[length] = '\0';
    *truncated = cut;
    return length;
}

static void log_line(oct_log_level level, const char *component, const char *event, const char *message)
{
    char line[LINE_BUFFER];
    bool truncated = false;
    size_t length = format_line(line, clamp_level(level), component, event, message, &truncated);
    pthread_mutex_lock(&g_lock);
    if (truncated) g_stats.truncated++;
    write_locked(line, length);
    pthread_mutex_unlock(&g_lock);
}

void oct_logf(oct_log_level level, const char *component, const char *event, const char *format, ...)
{
    if (!oct_log_enabled(level)) {
        atomic_fetch_add(&g_filtered, 1);
        return;
    }
    char message[LINE_BUFFER];
    va_list args;
    va_start(args, format);
    if (format) {
        vsnprintf(message, sizeof message, format, args);
    } else {
        message[0] = '\0'; /* no empty format string: Clang rejects one (-Wformat-zero-length) */
    }
    va_end(args);
    log_line(level, component, event, message);
}

void oct_log_message(oct_log_level level, const char *component, const char *event, const char *message)
{
    if (!oct_log_enabled(level)) {
        atomic_fetch_add(&g_filtered, 1);
        return;
    }
    log_line(level, component, event, message);
}

oct_status oct_log_open(const oct_log_config *config)
{
    oct_status status = OCT_OK;
    pthread_mutex_lock(&g_lock);
    close_locked();
    memset(&g_stats, 0, sizeof g_stats);
    atomic_store(&g_filtered, 0);
    g_max = config && config->max_bytes ? (config->max_bytes < 1024u ? 1024u : config->max_bytes) : OCT_LOG_DEFAULT_BYTES;
    g_keep = config ? (config->keep > ROTATED_MAX ? ROTATED_MAX : config->keep) : OCT_LOG_DEFAULT_KEEP;
    atomic_store(&g_level, (int)clamp_level(config ? config->level : OCT_LOG_INFO));
    g_path[0] = '\0';
    g_size = 0;
    if (config && config->path && config->path[0]) {
        if (strlen(config->path) >= sizeof g_path) {
            snprintf(g_stats.last_error, sizeof g_stats.last_error, "Opening the log: the path is too long");
            status = OCT_E_INVALID_ARGUMENT;
        } else {
            snprintf(g_path, sizeof g_path, "%s", config->path);
            g_fd = open(g_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
            struct stat info;
            if (g_fd < 0) {
                set_error_locked("Opening the log", errno);
                status = OCT_E_IO;
            } else {
                bool known = fstat(g_fd, &info) == 0;
                int error = known ? EINVAL : errno;
                /* A regular file, or a character device such as /dev/null for a discarded log. */
                if (!known || (!S_ISREG(info.st_mode) && !S_ISCHR(info.st_mode))) {
                    set_error_locked("Opening the log", error);
                    close_locked();
                    status = OCT_E_IO;
                } else {
                    g_size = S_ISREG(info.st_mode) ? (uint64_t)info.st_size : 0;
                    g_stats.file_open = true;
                }
            }
        }
    }
    snprintf(g_stats.path, sizeof g_stats.path, "%s", g_path);
    pthread_mutex_unlock(&g_lock);
    return status;
}

void oct_log_close(void)
{
    pthread_mutex_lock(&g_lock);
    close_locked();
    pthread_mutex_unlock(&g_lock);
}

oct_status oct_log_flush(void)
{
    oct_status status = OCT_OK;
    pthread_mutex_lock(&g_lock);
    if (g_path[0]) {
        struct stat on_disk, opened;
        bool vanished = g_fd < 0 || stat(g_path, &on_disk) != 0 || fstat(g_fd, &opened) != 0 ||
                        on_disk.st_ino != opened.st_ino || on_disk.st_dev != opened.st_dev;
        if (vanished && !reopen_locked()) status = OCT_E_IO;
        if (g_fd >= 0 && fsync(g_fd) != 0 && errno != EINVAL && errno != ENOTSUP) {
            set_error_locked("Flushing the log", errno);
            status = OCT_E_IO;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return status;
}

oct_status oct_log_clear(void)
{
    oct_status status = OCT_OK;
    pthread_mutex_lock(&g_lock);
    if (g_path[0]) {
        char rotated[OCT_LOG_PATH_MAX + 16];
        for (unsigned number = 1; number <= ROTATED_MAX; number++) {
            if (rotated_path(rotated, sizeof rotated, g_path, number)) unlink(rotated);
        }
    }
    if (g_fd >= 0 && ftruncate(g_fd, 0) != 0) {
        set_error_locked("Clearing the log", errno);
        status = OCT_E_IO;
    }
    g_size = 0;
    g_ring_head = 0;
    g_ring_count = 0;
    pthread_mutex_unlock(&g_lock);
    return status;
}

size_t oct_log_recent(char *buffer, size_t capacity)
{
    if (!buffer || capacity == 0) return 0;
    size_t length = 0, need = 1;
    pthread_mutex_lock(&g_lock);
    size_t first = g_ring_count;
    /* The newest lines that fit, written oldest first. */
    while (first > 0) {
        size_t n = strlen(g_ring[(g_ring_head + first - 1) % OCT_LOG_RECENT_LINES]) + 1;
        if (need + n > capacity) break;
        need += n;
        first--;
    }
    for (size_t i = first; i < g_ring_count; i++) {
        const char *line = g_ring[(g_ring_head + i) % OCT_LOG_RECENT_LINES];
        size_t n = strlen(line);
        memcpy(buffer + length, line, n);
        length += n;
        buffer[length++] = '\n';
    }
    pthread_mutex_unlock(&g_lock);
    buffer[length] = '\0';
    return length;
}

void oct_log_get_stats(oct_log_stats *out)
{
    if (!out) return;
    pthread_mutex_lock(&g_lock);
    *out = g_stats;
    out->file_bytes = g_size;
    pthread_mutex_unlock(&g_lock);
    out->level = oct_log_get_level();
    out->filtered = atomic_load(&g_filtered);
}

bool oct_log_file_path(unsigned number, char *out, size_t capacity)
{
    if (!out || capacity == 0) return false;
    pthread_mutex_lock(&g_lock);
    bool ok = g_path[0] && number <= g_keep && rotated_path(out, capacity, g_path, number);
    pthread_mutex_unlock(&g_lock);
    if (!ok) out[0] = '\0';
    return ok;
}

/* oct_zip.c - stored ZIP archives. See oct_zip.h. */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#elif !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "oct_zip.h"
#include "../Protocol/oct_ptp.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define ZIP_NAME_MAX 255u
#define ZIP_MAX_ENTRIES 0xFFFFu
#define ZIP_BODY_LIMIT (0xFFFFFFFFull - 16u * 1024u * 1024u) /* room for the central directory */
#define ZIP_1980 315532800 /* 1980-01-01T00:00:00Z, the earliest DOS date */
#define ZIP_PATH_MAX 4096
#define ZIP_VERIFY_LIMIT (1024u * 1024u * 1024u)

typedef struct zip_entry {
    char     name[ZIP_NAME_MAX + 1];
    uint16_t name_length;
    uint16_t time, date;
    uint32_t crc, size, offset;
} zip_entry;

struct oct_zip {
    oct_buf    body;   /* local headers and data, back to back */
    zip_entry *entries;
    size_t     count, capacity;
    char       error[2048];
};

static atomic_uint g_probe_counter;

__attribute__((format(printf, 3, 4)))
static void say(char *out, size_t capacity, const char *format, ...)
{
    if (!out || capacity == 0) return;
    va_list args;
    va_start(args, format);
    vsnprintf(out, capacity, format, args);
    va_end(args);
}

/* "Creating /path: No such file or directory" */
static void describe(char *out, size_t capacity, const char *step, const char *path, int code)
{
    say(out, capacity, "%s %s: %s", step, path, strerror(code));
}

static void append(char *out, size_t capacity, const char *text)
{
    size_t used = strlen(out);
    if (used + 1 >= capacity || !text[0]) return;
    snprintf(out + used, capacity - used, "%s%s", used ? "; " : "", text);
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint32_t oct_crc32(const void *data, size_t length)
{
    const uint8_t *p = data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; i++) {
        crc ^= p[i];
        for (unsigned bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static bool name_ok(const char *name, size_t *length)
{
    size_t n = name ? strlen(name) : 0;
    if (n == 0 || n > ZIP_NAME_MAX || name[0] == '/' || name[n - 1] == '/') return false;
    const char *part = name;
    for (const char *p = name;; p++) {
        if (*p == '/' || *p == '\0') {
            size_t part_length = (size_t)(p - part);
            if (part_length == 0 || (part_length == 1 && part[0] == '.') ||
                (part_length == 2 && part[0] == '.' && part[1] == '.')) {
                return false;
            }
            if (*p == '\0') break;
            part = p + 1;
        } else if (*p == '\\' || (unsigned char)*p < 0x20u) {
            return false;
        }
    }
    *length = n;
    return true;
}

static void dos_time(int64_t when, uint16_t *time_out, uint16_t *date_out)
{
    struct tm utc;
    time_t seconds = (time_t)when;
    if (when < ZIP_1980 || !gmtime_r(&seconds, &utc) || utc.tm_year + 1900 > 2107) {
        *time_out = 0;
        *date_out = (uint16_t)((1 << 5) | 1);
        return;
    }
    *time_out = (uint16_t)((utc.tm_hour << 11) | (utc.tm_min << 5) | (utc.tm_sec / 2));
    *date_out = (uint16_t)(((utc.tm_year + 1900 - 1980) << 9) | ((utc.tm_mon + 1) << 5) | utc.tm_mday);
}

oct_status oct_zip_create(oct_zip **out)
{
    if (!out) return OCT_E_INVALID_ARGUMENT;
    *out = calloc(1, sizeof **out);
    if (!*out) return OCT_E_NO_MEMORY;
    oct_buf_init(&(*out)->body, 512u * 1024u * 1024u);
    return OCT_OK;
}

void oct_zip_destroy(oct_zip *zip)
{
    if (!zip) return;
    oct_buf_free(&zip->body);
    free(zip->entries);
    free(zip);
}

size_t oct_zip_count(const oct_zip *zip) { return zip ? zip->count : 0; }

const char *oct_zip_error(const oct_zip *zip) { return zip ? zip->error : "no archive"; }

oct_status oct_zip_add(oct_zip *zip, const char *name, const void *data, size_t length, int64_t modified)
{
    size_t name_length = 0;
    if (!zip || (!data && length) || !name_ok(name, &name_length)) return OCT_E_INVALID_ARGUMENT;
    if (zip->body.failed) return OCT_E_NO_MEMORY;
    if (zip->count >= ZIP_MAX_ENTRIES) return OCT_E_TOO_LARGE;
    if ((uint64_t)zip->body.length + 30u + name_length + length > ZIP_BODY_LIMIT) return OCT_E_TOO_LARGE;
    if (zip->count == zip->capacity) {
        size_t capacity = zip->capacity ? zip->capacity * 2 : 16;
        zip_entry *grown = realloc(zip->entries, capacity * sizeof *grown);
        if (!grown) return OCT_E_NO_MEMORY;
        zip->entries = grown;
        zip->capacity = capacity;
    }
    zip_entry *e = &zip->entries[zip->count];
    memcpy(e->name, name, name_length + 1);
    e->name_length = (uint16_t)name_length;
    e->crc = oct_crc32(data, length);
    e->size = (uint32_t)length;
    e->offset = (uint32_t)zip->body.length;
    dos_time(modified, &e->time, &e->date);
    oct_buf *b = &zip->body;
    oct_buf_u32(b, 0x04034b50u); /* local file header */
    oct_buf_u16(b, 20);          /* version needed: 2.0 */
    oct_buf_u16(b, 0x0800);      /* names are UTF-8 */
    oct_buf_u16(b, 0);           /* stored */
    oct_buf_u16(b, e->time);
    oct_buf_u16(b, e->date);
    oct_buf_u32(b, e->crc);
    oct_buf_u32(b, e->size);
    oct_buf_u32(b, e->size);
    oct_buf_u16(b, e->name_length);
    oct_buf_u16(b, 0);
    oct_buf_bytes(b, name, name_length);
    if (length) oct_buf_bytes(b, data, length);
    if (b->failed) return OCT_E_NO_MEMORY;
    zip->count++;
    return OCT_OK;
}

static bool write_all(int fd, const uint8_t *bytes, size_t length)
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

oct_status oct_zip_write(oct_zip *zip, const char *path)
{
    if (!zip || !path || !path[0]) return OCT_E_INVALID_ARGUMENT;
    zip->error[0] = '\0';
    if (zip->body.failed) {
        say(zip->error, sizeof zip->error, "The archive ran out of memory while it was built");
        return OCT_E_NO_MEMORY;
    }
    oct_buf central;
    oct_buf_init(&central, 0);
    for (size_t i = 0; i < zip->count; i++) {
        const zip_entry *e = &zip->entries[i];
        oct_buf_u32(&central, 0x02014b50u); /* central directory header */
        oct_buf_u16(&central, 0x031E);      /* made by: Unix, ZIP 3.0 */
        oct_buf_u16(&central, 20);
        oct_buf_u16(&central, 0x0800);
        oct_buf_u16(&central, 0);
        oct_buf_u16(&central, e->time);
        oct_buf_u16(&central, e->date);
        oct_buf_u32(&central, e->crc);
        oct_buf_u32(&central, e->size);
        oct_buf_u32(&central, e->size);
        oct_buf_u16(&central, e->name_length);
        oct_buf_u16(&central, 0); /* extra */
        oct_buf_u16(&central, 0); /* comment */
        oct_buf_u16(&central, 0); /* disk */
        oct_buf_u16(&central, 0); /* internal attributes */
        oct_buf_u32(&central, 0x81A40000u); /* a regular file, rw-r--r-- */
        oct_buf_u32(&central, e->offset);
        oct_buf_bytes(&central, e->name, e->name_length);
    }
    size_t central_length = central.length;
    oct_buf_u32(&central, 0x06054b50u); /* end of central directory */
    oct_buf_u16(&central, 0);
    oct_buf_u16(&central, 0);
    oct_buf_u16(&central, (uint16_t)zip->count);
    oct_buf_u16(&central, (uint16_t)zip->count);
    oct_buf_u32(&central, (uint32_t)central_length);
    oct_buf_u32(&central, (uint32_t)zip->body.length);
    oct_buf_u16(&central, 0);
    if (central.failed) {
        say(zip->error, sizeof zip->error, "The archive's directory ran out of memory");
        oct_buf_free(&central);
        return OCT_E_NO_MEMORY;
    }
    char partial[ZIP_PATH_MAX + 16];
    int n = snprintf(partial, sizeof partial, "%s.partial", path);
    if (n <= 0 || (size_t)n >= sizeof partial) {
        say(zip->error, sizeof zip->error, "The path %s is too long", path);
        oct_buf_free(&central);
        return OCT_E_INVALID_ARGUMENT;
    }
    oct_status status = OCT_OK;
    int fd = open(partial, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        describe(zip->error, sizeof zip->error, "Creating", partial, errno);
        oct_buf_free(&central);
        return OCT_E_IO;
    }
    if (!write_all(fd, zip->body.data, zip->body.length) || !write_all(fd, central.data, central.length)) {
        describe(zip->error, sizeof zip->error, "Writing", partial, errno);
        status = OCT_E_IO;
    }
    if (status == OCT_OK && fsync(fd) != 0 && errno != EINVAL && errno != ENOTSUP) {
        describe(zip->error, sizeof zip->error, "Saving", partial, errno);
        status = OCT_E_IO;
    }
    if (close(fd) != 0 && status == OCT_OK) {
        describe(zip->error, sizeof zip->error, "Closing", partial, errno);
        status = OCT_E_IO;
    }
    if (status == OCT_OK && rename(partial, path) != 0) {
        int code = errno;
        say(zip->error, sizeof zip->error, "Renaming %s to %s: %s", partial, path, strerror(code));
        status = OCT_E_IO;
    }
    if (status != OCT_OK) unlink(partial);
    oct_buf_free(&central);
    return status;
}

oct_status oct_zip_prepare_directory(const char *directory, char *error, size_t capacity)
{
    if (error && capacity) error[0] = '\0';
    if (!directory || !directory[0]) {
        say(error, capacity, "No folder was given");
        return OCT_E_INVALID_ARGUMENT;
    }
    char path[ZIP_PATH_MAX];
    size_t length = strlen(directory);
    if (length >= sizeof path) {
        say(error, capacity, "The folder path is too long");
        return OCT_E_INVALID_ARGUMENT;
    }
    memcpy(path, directory, length + 1);
    while (length > 1 && path[length - 1] == '/') path[--length] = '\0';
    /* Each missing component, parents first. An existing one answers EEXIST; a file in the way, ENOTDIR below it. */
    for (char *p = path + 1;; p++) {
        if (*p != '/' && *p != '\0') continue;
        char kept = *p;
        *p = '\0';
        if (mkdir(path, 0755) != 0 && errno != EEXIST) {
            describe(error, capacity, "Creating", path, errno);
            return OCT_E_IO;
        }
        *p = kept;
        if (kept == '\0') break;
    }
    struct stat info;
    if (stat(path, &info) != 0) {
        describe(error, capacity, "Opening", path, errno);
        return OCT_E_IO;
    }
    if (!S_ISDIR(info.st_mode)) {
        describe(error, capacity, "Opening", path, ENOTDIR);
        return OCT_E_IO;
    }
    char probe[ZIP_PATH_MAX + 64];
    snprintf(probe, sizeof probe, "%s/.octopus-probe-%ld-%u", path, (long)getpid(),
             atomic_fetch_add(&g_probe_counter, 1u));
    int fd = open(probe, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        describe(error, capacity, "Creating a file in", path, errno);
        return OCT_E_IO;
    }
    close(fd);
    unlink(probe);
    return OCT_OK;
}

oct_status oct_zip_verify(const char *path, size_t *entries, char *error, size_t capacity)
{
    if (entries) *entries = 0;
    if (error && capacity) error[0] = '\0';
    if (!path || !path[0]) return OCT_E_INVALID_ARGUMENT;
    FILE *file = fopen(path, "rb");
    if (!file) {
        describe(error, capacity, "Reading back", path, errno);
        return OCT_E_IO;
    }
    long measured = fseek(file, 0, SEEK_END) == 0 ? ftell(file) : -1;
    if (measured < 22 || (unsigned long)measured > ZIP_VERIFY_LIMIT) {
        say(error, capacity, "Reading back %s: %ld bytes isn't an archive this writer makes", path, measured);
        fclose(file);
        return OCT_E_MALFORMED;
    }
    size_t size = (size_t)measured;
    uint8_t *bytes = malloc(size);
    bool read_all = bytes && fseek(file, 0, SEEK_SET) == 0 && fread(bytes, 1, size, file) == size;
    fclose(file);
    if (!read_all) {
        say(error, capacity, "Reading back %s: %s", path, bytes ? "the file changed while it was read" : "out of memory");
        free(bytes);
        return bytes ? OCT_E_IO : OCT_E_NO_MEMORY;
    }
    oct_status status = OCT_E_MALFORMED;
    const uint8_t *end = bytes + size - 22;
    size_t count = 0, directory = 0, directory_size = 0, offset = 0;
    if (rd32(end) != 0x06054b50u) {
        say(error, capacity, "%s has no end-of-archive record", path);
        goto done;
    }
    count = rd16(end + 10);
    directory_size = rd32(end + 12);
    directory = rd32(end + 16);
    if (directory > size - 22 || directory_size != size - 22 - directory) {
        say(error, capacity, "%s: its directory doesn't end where its end record starts", path);
        goto done;
    }
    offset = directory;
    for (size_t i = 0; i < count; i++) {
        if (offset + 46 > directory + directory_size || rd32(bytes + offset) != 0x02014b50u) {
            say(error, capacity, "%s: directory entry %zu is missing", path, i + 1);
            goto done;
        }
        const uint8_t *c = bytes + offset;
        uint16_t method = rd16(c + 10), name_length = rd16(c + 28);
        uint32_t crc = rd32(c + 16), stored = rd32(c + 20), length = rd32(c + 24);
        size_t local = rd32(c + 42), next = offset + 46u + name_length + rd16(c + 30) + rd16(c + 32);
        if (next > directory + directory_size || method != 0 || stored != length) {
            say(error, capacity, "%s: directory entry %zu is malformed", path, i + 1);
            goto done;
        }
        if (local + 30 > directory || rd32(bytes + local) != 0x04034b50u || rd16(bytes + local + 26) != name_length ||
            memcmp(bytes + local + 30, c + 46, name_length) != 0) {
            say(error, capacity, "%s: the file header of entry %zu doesn't match the directory", path, i + 1);
            goto done;
        }
        size_t data = local + 30u + name_length + rd16(bytes + local + 28);
        if (data + length > directory) {
            say(error, capacity, "%s: entry %zu runs past the directory", path, i + 1);
            goto done;
        }
        if (oct_crc32(bytes + data, length) != crc) {
            say(error, capacity, "%s: entry %.*s has the wrong CRC-32", path, (int)(name_length < 200 ? name_length : 200),
                (const char *)(c + 46));
            goto done;
        }
        offset = next;
    }
    if (offset != directory + directory_size) {
        say(error, capacity, "%s: its directory has bytes after the last entry", path);
        goto done;
    }
    if (entries) *entries = count;
    status = OCT_OK;
done:
    free(bytes);
    return status;
}

oct_status oct_zip_write_first(oct_zip *zip, const char *const *directories, size_t count, const char *file_name,
                               char *path, size_t capacity)
{
    if (!zip || !directories || count == 0 || !file_name || !file_name[0] || strchr(file_name, '/') || !path ||
        capacity == 0) {
        return OCT_E_INVALID_ARGUMENT;
    }
    char notes[sizeof zip->error] = "";
    path[0] = '\0';
    for (size_t i = 0; i < count; i++) {
        const char *directory = directories[i];
        if (!directory || !directory[0]) continue;
        char reason[1024] = "";
        char candidate[ZIP_PATH_MAX];
        int n = snprintf(candidate, sizeof candidate, "%s/%s", directory, file_name);
        oct_status status = OCT_OK;
        if (n <= 0 || (size_t)n >= sizeof candidate || (size_t)n >= capacity) {
            say(reason, sizeof reason, "%s: the path is too long", directory);
            status = OCT_E_INVALID_ARGUMENT;
        }
        if (status == OCT_OK) status = oct_zip_prepare_directory(directory, reason, sizeof reason);
        if (status == OCT_OK) {
            status = oct_zip_write(zip, candidate);
            if (status != OCT_OK) say(reason, sizeof reason, "%s", zip->error);
        }
        if (status == OCT_OK) {
            size_t found = 0;
            status = oct_zip_verify(candidate, &found, reason, sizeof reason);
            if (status == OCT_OK && found != zip->count) {
                say(reason, sizeof reason, "%s read back with %zu entries of %zu", candidate, found, zip->count);
                status = OCT_E_IO;
            }
            if (status != OCT_OK) unlink(candidate);
        }
        if (status == OCT_OK) {
            snprintf(path, capacity, "%s", candidate);
            snprintf(zip->error, sizeof zip->error, "%s", notes);
            return OCT_OK;
        }
        append(notes, sizeof notes, reason);
    }
    snprintf(zip->error, sizeof zip->error, "%s", notes[0] ? notes : "No folder was given");
    return OCT_E_IO;
}

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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define ZIP_NAME_MAX 255u
#define ZIP_MAX_ENTRIES 0xFFFFu
#define ZIP_BODY_LIMIT (0xFFFFFFFFull - 16u * 1024u * 1024u) /* room for the central directory */
#define ZIP_1980 315532800 /* 1980-01-01T00:00:00Z, the earliest DOS date */

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
};

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
    if (zip->body.failed) return OCT_E_NO_MEMORY;
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
        oct_buf_free(&central);
        return OCT_E_NO_MEMORY;
    }
    char partial[4096];
    int n = snprintf(partial, sizeof partial, "%s.partial", path);
    oct_status status = n > 0 && (size_t)n < sizeof partial ? OCT_OK : OCT_E_INVALID_ARGUMENT;
    int fd = status == OCT_OK ? open(partial, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644) : -1;
    if (status == OCT_OK && fd < 0) status = OCT_E_IO;
    if (status == OCT_OK && (!write_all(fd, zip->body.data, zip->body.length) ||
                             !write_all(fd, central.data, central.length))) {
        status = OCT_E_IO;
    }
    if (fd >= 0 && close(fd) != 0 && status == OCT_OK) status = OCT_E_IO;
    if (status == OCT_OK && rename(partial, path) != 0) status = OCT_E_IO;
    if (status != OCT_OK && fd >= 0) unlink(partial);
    oct_buf_free(&central);
    return status;
}

/* A small ZIP writer for the diagnostics export: stored (uncompressed) entries with UTF-8 names, as every unzip
 * tool reads. Archives stay under 4 GiB and 65,535 entries; there is no ZIP64. */
#ifndef OCT_ZIP_H
#define OCT_ZIP_H

#include "oct_store.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct oct_zip oct_zip;

oct_status oct_zip_create(oct_zip **out);
/* Adds a file. `name` separates folders with '/', with no leading '/', no empty, "." or ".." parts, and at most
 * 255 bytes. `modified` is seconds since 1970 UTC; times before 1980 are stored as 1980-01-01. */
oct_status oct_zip_add(oct_zip *zip, const char *name, const void *data, size_t length, int64_t modified);
size_t     oct_zip_count(const oct_zip *zip);
/* Writes the archive to `path` through a temporary file beside it, replacing what was there. */
oct_status oct_zip_write(oct_zip *zip, const char *path);
void       oct_zip_destroy(oct_zip *zip);
/* CRC-32 (IEEE 802.3), the checksum ZIP stores for each entry. */
uint32_t   oct_crc32(const void *data, size_t length);

#ifdef __cplusplus
}
#endif

#endif /* OCT_ZIP_H */

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
/* Writes the archive to `path` through a temporary file beside it, synced to storage and then renamed over what
 * was there, so a reader sees the old file or the whole new one. */
oct_status oct_zip_write(oct_zip *zip, const char *path);
/* Writes the archive as `file_name` into the first of `directories` that is ready (oct_zip_prepare_directory)
 * and reads back intact (oct_zip_verify), and puts the path in `path`. A folder that fails is left without the
 * archive and the next is tried; every refusal is kept in oct_zip_error, even when a later folder works. */
oct_status oct_zip_write_first(oct_zip *zip, const char *const *directories, size_t count, const char *file_name,
                               char *path, size_t capacity);
/* Why the last write failed: the step, the path and the system's error ("Creating /x/a.zip.partial: No such file
 * or directory"); after oct_zip_write_first, each folder's reason, separated by "; ". Empty when nothing failed. */
const char *oct_zip_error(const oct_zip *zip);
void       oct_zip_destroy(oct_zip *zip);

/* Makes sure `directory` exists, creating it and any missing parents, and that a file can be created in it, by
 * creating and removing a probe file. On failure `error` names the step, the path and the system's error. */
oct_status oct_zip_prepare_directory(const char *directory, char *error, size_t capacity);
/* Reads an archive back and checks it: the end record, the central directory, each local header, and every
 * entry's CRC-32. `entries` gets the number of entries; `error` says what was wrong. */
oct_status oct_zip_verify(const char *path, size_t *entries, char *error, size_t capacity);
/* CRC-32 (IEEE 802.3), the checksum ZIP stores for each entry. */
uint32_t   oct_crc32(const void *data, size_t length);

#ifdef __cplusplus
}
#endif

#endif /* OCT_ZIP_H */

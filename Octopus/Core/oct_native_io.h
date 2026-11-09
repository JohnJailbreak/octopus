#ifndef OCT_NATIVE_IO_H
#define OCT_NATIVE_IO_H
#include "oct_store.h"
#define OCT_NATIVE_DIRECTORY "/var/mobile/Library/Caches/Octopus/native"
#define OCT_NATIVE_LEASE_MS 15000u
/* Both peers are mobile. This is not an authentication boundary against other
 * processes with the same UID. Files contain no filenames or file contents. */
oct_status oct_native_io_read(const char *directory, const char *name, void *bytes, size_t capacity, size_t *length);
oct_status oct_native_io_write(const char *directory, const char *name, const void *bytes, size_t length);
oct_status oct_native_io_remove_lease(const char *directory);
oct_status oct_native_write_lease(const char *directory, const char *token, unsigned mode, uint64_t now_ms);
/* Exact canonical OCTN1 grammar; usable without filesystem access for protocol tests. */
unsigned oct_native_parse_lease(const void *bytes, size_t length, uint64_t now_ms, uint64_t *deadline, char token[33]);
unsigned oct_native_read_lease(const char *directory, uint64_t now_ms, uint64_t *deadline, char token[33]);
#endif

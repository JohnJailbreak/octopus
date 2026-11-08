/* SHA-256 (FIPS 180-4), for the diagnostics: the digest of a synthetic file that a host should receive. */
#ifndef OCT_SHA256_H
#define OCT_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct oct_sha256 {
    uint32_t state[8];
    uint64_t length; /* bytes hashed */
    uint8_t  block[64];
    size_t   used;
} oct_sha256;

void oct_sha256_init(oct_sha256 *context);
void oct_sha256_update(oct_sha256 *context, const void *data, size_t length);
void oct_sha256_final(oct_sha256 *context, uint8_t digest[32]);
/* The digest of `data` as 64 lowercase hex characters and a NUL. */
void oct_sha256_hex(const void *data, size_t length, char out[65]);

#ifdef __cplusplus
}
#endif

#endif /* OCT_SHA256_H */

/* oct_sha256.c - SHA-256, FIPS 180-4. See oct_sha256.h. */
#include "oct_sha256.h"

#include <stdio.h>
#include <string.h>

static const uint32_t k_round[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static uint32_t rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32u - n)); }

static void compress(oct_sha256 *c, const uint8_t *p)
{
    uint32_t w[64];
    for (unsigned i = 0; i < 16; i++) {
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | (uint32_t)p[4 * i + 3];
    }
    for (unsigned i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = c->state[0], b = c->state[1], d3 = c->state[2], d = c->state[3];
    uint32_t e = c->state[4], f = c->state[5], g = c->state[6], h = c->state[7];
    for (unsigned i = 0; i < 64; i++) {
        uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k_round[i] + w[i];
        uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & d3) ^ (b & d3));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = d3;
        d3 = b;
        b = a;
        a = t1 + t2;
    }
    c->state[0] += a;
    c->state[1] += b;
    c->state[2] += d3;
    c->state[3] += d;
    c->state[4] += e;
    c->state[5] += f;
    c->state[6] += g;
    c->state[7] += h;
}

void oct_sha256_init(oct_sha256 *c)
{
    static const uint32_t initial[8] = { 0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                         0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u };
    memcpy(c->state, initial, sizeof initial);
    c->length = 0;
    c->used = 0;
}

void oct_sha256_update(oct_sha256 *c, const void *data, size_t length)
{
    const uint8_t *p = data;
    c->length += length;
    while (length > 0) {
        size_t take = 64 - c->used < length ? 64 - c->used : length;
        memcpy(c->block + c->used, p, take);
        c->used += take;
        p += take;
        length -= take;
        if (c->used == 64) {
            compress(c, c->block);
            c->used = 0;
        }
    }
}

void oct_sha256_final(oct_sha256 *c, uint8_t digest[32])
{
    uint64_t bits = c->length * 8u;
    uint8_t pad = 0x80;
    uint8_t zero = 0;
    uint64_t total = c->length;
    oct_sha256_update(c, &pad, 1);
    while (c->used != 56) oct_sha256_update(c, &zero, 1);
    uint8_t length_bytes[8];
    for (unsigned i = 0; i < 8; i++) length_bytes[i] = (uint8_t)(bits >> (56 - 8 * i));
    oct_sha256_update(c, length_bytes, 8);
    c->length = total;
    for (unsigned i = 0; i < 8; i++) {
        digest[4 * i] = (uint8_t)(c->state[i] >> 24);
        digest[4 * i + 1] = (uint8_t)(c->state[i] >> 16);
        digest[4 * i + 2] = (uint8_t)(c->state[i] >> 8);
        digest[4 * i + 3] = (uint8_t)c->state[i];
    }
}

void oct_sha256_hex(const void *data, size_t length, char out[65])
{
    oct_sha256 c;
    uint8_t digest[32];
    oct_sha256_init(&c);
    oct_sha256_update(&c, data, length);
    oct_sha256_final(&c, digest);
    for (unsigned i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", digest[i]);
}

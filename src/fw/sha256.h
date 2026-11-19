/* SHA-256 (FIPS 180-4) for SSH (curve25519-sha256: the exchange hash, the keys, the SHA256: fingerprints):
   Monocypher has SHA-512 and BLAKE2b only. */
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t h[8];
    uint8_t buf[64];
    uint64_t len;          /* bytes so far */
} sha256_ctx;

void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *p, size_t n);
void sha256_final(sha256_ctx *c, uint8_t out[32]);
void sha256(uint8_t out[32], const void *p, size_t n);

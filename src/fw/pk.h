/* Signature checks of SSH host keys (docs/PLAN.md Ф6): RSA PKCS #1 v1.5 (rsa-sha2-256 / rsa-sha2-512, RFC 8332)
   and ECDSA on NIST P-256 / P-384 / P-521 (RFC 5656). Verification only (public data: no constant time).
   Numbers are big-endian byte strings (the bodies of SSH mpints: no sign byte). Returns 1 = valid. */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define PK_RSA_MIN_BITS 1024
#define PK_RSA_MAX_BITS 8192

/* the signature of msg (hashed here with SHA-256, or SHA-512 if sha512) under the key (n, e) */
int pk_rsa_verify(const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen, int sha512,
                  const uint8_t *msg, size_t mlen, const uint8_t *sig, size_t slen);

enum pk_curve { PK_P256, PK_P384, PK_P521, PK_CURVES };
size_t pk_curve_bytes(int curve);             /* 32, 48, 66: the size of a coordinate */
/* q: the public point 04 || X || Y; (r, s): the signature of msg (hashed here: SHA-256 / 384 / 512 by the curve) */
int pk_ecdsa_verify(int curve, const uint8_t *q, size_t qlen, const uint8_t *r, size_t rlen,
                    const uint8_t *s, size_t slen, const uint8_t *msg, size_t mlen);

void pk_sha384(uint8_t out[48], const uint8_t *m, size_t n);

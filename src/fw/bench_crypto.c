/* Ф0.2 bench: the crypto of an SSH session on rv32im (Monocypher 4.0.3, upstream/monocypher):
   kex curve25519-sha256 = x25519 public key + x25519 shared; host key ssh-ed25519 = one ed25519 check;
   user key auth = one ed25519 sign; cipher chacha20-poly1305@openssh = chacha20 (djb) + poly1305.
   Marks: 1 x25519 pub, 2 x25519 shared, 3 ed25519 check, 4 ed25519 sign, 5 chacha20 64 KB, 6 poly1305 64 KB,
   7 sha512 64 KB. Results printed so the host build can compare. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "monocypher.h"
#include "monocypher-ed25519.h"
#ifdef HOST
#define MARK(v) ((void)0)
#else
#include "mmio.h"
#define MARK(v) (MMIO_MARK = (v))
#endif

static uint8_t buf[65536];

static uint32_t fnv(const uint8_t *p, size_t n) { uint32_t h = 2166136261u; while (n--) h = (h ^ *p++) * 16777619u; return h; }

int main(void) {
    uint8_t sk[32], pk[32], peer_sk[32], peer_pk[32], shared[32], seed[32], esk[64], epk[32], sig[64], mac[16], h[64];
    for (int i = 0; i < 32; i++) { sk[i] = (uint8_t)(i * 7 + 1); peer_sk[i] = (uint8_t)(i * 13 + 5); seed[i] = (uint8_t)(i * 3 + 9); }
    for (size_t i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)(i * 31);
    crypto_x25519_public_key(peer_pk, peer_sk);
    crypto_ed25519_key_pair(esk, epk, seed);
    crypto_ed25519_sign(sig, esk, buf, 300); /* the host's signature of an exchange hash */
    MARK(0);
    crypto_x25519_public_key(pk, sk);
    MARK(1);
    crypto_x25519(shared, sk, peer_pk);
    MARK(2);
    int bad = crypto_ed25519_check(sig, epk, buf, 300);
    MARK(3);
    crypto_ed25519_sign(sig, esk, buf, 300);
    MARK(4);
    uint8_t key[32] = {1}, nonce[8] = {2};
    crypto_chacha20_djb(buf, buf, sizeof buf, key, nonce, 0);
    MARK(5);
    crypto_poly1305(mac, buf, sizeof buf, key);
    MARK(6);
    crypto_sha512(h, buf, sizeof buf);
    MARK(7);
    printf("x25519 %08x check %d sig %08x chacha %08x mac %08x sha %08x\n", fnv(shared, 32), bad, fnv(sig, 64),
           fnv(buf, sizeof buf), fnv(mac, 16), fnv(h, 64));
    return 0;
}

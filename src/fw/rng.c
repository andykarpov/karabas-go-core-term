/* see rng.h */
#include <string.h>
#include "rng.h"
#include "monocypher.h"
#include "mmio_hw.h"

#define READY_CREDIT 128           /* bits of events without a seed from the SD card */

static uint8_t pool[64];
static uint32_t ev[16];            /* events not hashed in yet (a hash per event is too slow) */
static int ev_n;
static int credit, seeded;
static uint64_t outputs;

static void absorb(const void *p, size_t n) {
    crypto_blake2b_ctx c;
    crypto_blake2b_init(&c, 64);
    crypto_blake2b_update(&c, pool, 64);
    crypto_blake2b_update(&c, p, n);
    crypto_blake2b_final(&c, pool);
}

static void flush_events(void) {
    if (!ev_n) return;
    absorb(ev, sizeof ev[0] * (size_t)ev_n);
    ev_n = 0;
}

void rng_event(int c) {
    ev[ev_n++] = IO_CYCLES ^ IO_US << 20;
    if (ev_n == 16) flush_events();
    credit += c;
}

void rng_mix(const void *p, size_t n, int c) {
    flush_events();
    absorb(p, n);
    credit += c;
}

void rng_bytes(uint8_t *out, size_t n) {
    rng_event(0);
    flush_events();
    uint8_t key[32], nonce[8] = { 0 };
    crypto_blake2b_ctx c;
    crypto_blake2b_init(&c, 32);
    crypto_blake2b_update(&c, pool, 64);
    crypto_blake2b_update(&c, (const uint8_t *)"out", 3);
    crypto_blake2b_update(&c, (const uint8_t *)&outputs, sizeof outputs);
    crypto_blake2b_final(&c, key);
    outputs++;
    crypto_chacha20_djb(out, NULL, n, key, nonce, 0);
    crypto_wipe(key, sizeof key);
    absorb("next", 4);
}

int rng_ready(void) { return seeded || credit >= READY_CREDIT; }
void rng_seeded(void) { seeded = 1; }

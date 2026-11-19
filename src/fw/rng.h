/* The random numbers of the term core (docs/PLAN.md Ф3: in the firmware only, no RNG in the RTL):
   a BLAKE2b pool fed with the 65 MHz cycle counter at the moments the outside world decides (HID reports
   of the keyboard, lines and frames of the ESP8266, ...), the power-up contents of the unused SRAM, and a
   seed kept on the SD card (the save file, store.c: read at start, replaced at once and at every
   connection). The output: ChaCha20 under a key hashed from the pool; the pool moves on after every
   output (an old output does not give the next one). */
#pragma once
#include <stdint.h>
#include <stddef.h>

void rng_event(int credit);                /* an outside event now: credit = bits it is assumed to give */
void rng_mix(const void *p, size_t n, int credit);
void rng_bytes(uint8_t *out, size_t n);
int rng_ready(void);                       /* seeded from the SD card, or enough events */
void rng_seeded(void);                     /* a seed from the SD card was mixed in */

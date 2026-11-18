/* Ф1 board test firmware (make hw HWDEFS=-DHWTEST BW=build-hw-test): SRAM test of 0x280000..0x2fffff
   (free: above the base glyphs, below the rest of the font) with byte / halfword / word stores and
   reads (cached and uncached through line fills), the result as text on the USB serial port, then a
   direct echo (a received byte goes back from a register, no memory) for the link test. */
#include <stdint.h>
#include "mmio_hw.h"

static void putc_(uint8_t c) { while (IO_SER_STAT & 2) {} IO_SER_TX = c; }
static void puts_(const char *s) { while (*s) putc_((uint8_t)*s++); }
static void puthex(uint32_t v) { for (int i = 28; i >= 0; i -= 4) putc_("0123456789abcdef"[v >> i & 15]); }

static uint32_t rnd = 1;
static uint32_t next(void) { rnd = rnd * 1664525u + 1013904223u; return rnd; }

#define BASE ((volatile uint8_t *)0x280000)
#define SIZE 0x80000u

static uint32_t check(const char *name, uint32_t seed) {
    uint32_t err = 0, first = 0xffffffffu, fv = 0, fe = 0;
    rnd = seed;
    for (uint32_t a = 0; a < SIZE; a++) {
        uint8_t e = (uint8_t)(next() >> 24), v = BASE[a];
        if (v != e) { if (!err) { first = a; fv = v; fe = e; } err++; }
    }
    puts_(name); puts_(" errors "); puthex(err);
    if (err) { puts_(" first at "); puthex(0x280000 + first); puts_(" got "); puthex(fv); puts_(" want "); puthex(fe); }
    puts_("\r\n");
    return err;
}

int main(void) {
    IO_DBG = 't';
    uint32_t go;
    while (!((go = IO_SER_RX) & 0x100)) {}            /* the host says go: 'e' = the echo only */
    for (int pass = 0; pass < 3 && (go & 0xff) != 'e'; pass++) {
        uint32_t seed = 12345 + pass;
        rnd = seed;                                   /* bytes */
        for (uint32_t a = 0; a < SIZE; a++) BASE[a] = (uint8_t)(next() >> 24);
        check("bytes", seed);
        rnd = seed + 100;                             /* halfwords */
        for (uint32_t a = 0; a < SIZE; a += 2) {
            uint16_t h = (uint16_t)(next() >> 24); h |= (uint16_t)((next() >> 24) << 8);
            *(volatile uint16_t *)(BASE + a) = h;
        }
        check("halves", seed + 100);
        rnd = seed + 200;                             /* words */
        for (uint32_t a = 0; a < SIZE; a += 4) {
            uint32_t w = 0;
            for (int k = 0; k < 4; k++) w |= (next() >> 24) << (8 * k);
            *(volatile uint32_t *)(BASE + a) = w;
        }
        check("words", seed + 200);
    }
    puts_("echo\r\n");
    IO_DBG = 'e';
    for (;;) {
        uint32_t r = IO_SER_RX;
        if (r & 0x100) putc_((uint8_t)r);
    }
}

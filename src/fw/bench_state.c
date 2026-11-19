/* Ф0.1 bench: libvterm state layer + our screen layer (tscreen.c), same streams and same packed-cell
   hash as bench_vterm.c (the libvterm screen layer = the oracle). Host build: -DHOST, ./a.out stream */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "vterm.h"
#include "tscreen.h"
#include "vtfeed.h"

#ifndef CHUNK
#define CHUNK 256
#endif
#ifdef HOST
#define MARK(v) ((void)0)
#else
#include "mmio.h"
#define MARK(v) (MMIO_MARK = (v))
#endif

static ts_cell ring[TS_RING * TS_COLS], alt[TS_ROWS * TS_COLS];
static tscreen ts;

static void out_cb(const char *s, size_t len, void *user) { (void)s; (void)len; (void)user; }

int main(int argc, char **argv) {
    const uint8_t *in;
    uint32_t n;
#ifdef HOST
    if (argc < 2) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    static uint8_t buf[8 << 20];
    n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    in = buf;
#else
    (void)argc; (void)argv;
    in = (const uint8_t *)0x11000000u;
    n = MMIO_INSIZE;
#endif
    MARK(0);
    VTerm *vt = vterm_new(TS_ROWS, TS_COLS);
    vterm_set_utf8(vt, 1);
    vterm_output_set_callback(vt, out_cb, NULL);
    VTermState *st = vterm_obtain_state(vt);
    tscreen_init(&ts, st, ring, alt);
    MARK(1);
    for (uint32_t i = 0; i < n; i += CHUNK) {
        uint32_t len = n - i < CHUNK ? n - i : CHUNK;
        { static struct vtfeed vf; vtfeed(&vf, vt, (const char *)in + i, len); }
    }
    MARK(2);
    uint32_t h = 2166136261u;
    for (int y = 0; y < TS_ROWS; y++) {
        const uint8_t *p = (const uint8_t *)tscreen_cell(&ts, y, 0);
        for (size_t k = 0; k < TS_COLS * sizeof(ts_cell); k++) h = (h ^ p[k]) * 16777619u;
    }
    printf("bytes %u put %u scroll %u rot %u moves %u erase %u hash %08x\n", (unsigned)n, (unsigned)ts.n_put,
           (unsigned)ts.n_scroll, (unsigned)ts.n_rot, (unsigned)ts.n_move, (unsigned)ts.n_erase, (unsigned)h);
    return 0;
}

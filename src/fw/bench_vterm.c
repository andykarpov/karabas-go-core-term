/* Ф0.1 bench: libvterm parse + screen update speed.
   The input stream (a recorded pty session, src/sim/rec.py) is fed to libvterm in UART-sized chunks;
   every damaged rect is repacked into a compact 8-byte cell buffer (the video RAM of the renderer),
   moverect = memmove of the packed rows (scroll). At the end: FNV-1a of the packed buffer, so the
   rv32 run (ISS) and the host build (same file, gcc) can be compared.
   Build: rv32 (Makefile, IN_BASE input) or host: cc -DHOST bench_vterm.c ... ; ./a.out stream */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vterm.h"
#include "vtfeed.h"
#include "compose.h"

#ifndef ROWS
#define ROWS 48
#endif
#ifndef COLS
#define COLS 128
#endif
#ifndef CHUNK
#define CHUNK 256
#endif

#ifdef HOST
#define MARK(v) ((void)0)
#else
#include "mmio.h"
#define MARK(v) (MMIO_MARK = (v))
#endif

/* the packed cell: w0 = codepoint (21 bits) | wide (bit 21) | attrs (bits 22..31);
   w1 = fg (low 16) | bg (high 16): bit 15 = RGB555, else bits 7..0 = palette index, 0x100 = default */
typedef struct { uint32_t w0, w1; } cell_t;
static cell_t vram[ROWS * COLS];
static VTermScreen *scr;
static uint32_t n_damage, n_cells, n_move;

static uint16_t pack_color(const VTermColor *c, int is_bg) {
    if ((is_bg && VTERM_COLOR_IS_DEFAULT_BG(c)) || (!is_bg && VTERM_COLOR_IS_DEFAULT_FG(c))) return 0x100;
    if (VTERM_COLOR_IS_INDEXED(c)) return c->indexed.idx;
    return 0x8000 | ((c->rgb.red >> 3) << 10) | ((c->rgb.green >> 3) << 5) | (c->rgb.blue >> 3);
}

static int on_damage(VTermRect r, void *user) {
    (void)user;
    n_damage++;
    VTermScreenCell c;
    for (int y = r.start_row; y < r.end_row; y++)
        for (int x = r.start_col; x < r.end_col; x++) {
            VTermPos p = { y, x };
            vterm_screen_get_cell(scr, p, &c);
            uint32_t a = c.attrs.bold | c.attrs.underline << 1 | c.attrs.italic << 3 | c.attrs.blink << 4 |
                         c.attrs.reverse << 5 | c.attrs.conceal << 6 | c.attrs.strike << 7;
            cell_t *d = &vram[y * COLS + x];
            if (c.chars[0] == (uint32_t)-1) { d->w0 = 0x1fffff; d->w1 = 0; n_cells++; continue; } /* right half of a wide glyph */
            d->w0 = (compose(c.chars, VTERM_MAX_CHARS_PER_CELL) & 0x1fffff) | (c.width == 2 ? 1u << 21 : 0) | a << 22;
            d->w1 = pack_color(&c.fg, 0) | (uint32_t)pack_color(&c.bg, 1) << 16;
            n_cells++;
        }
    return 1;
}

static int on_moverect(VTermRect dst, VTermRect src, void *user) {
    (void)user;
    n_move++;
    int w = src.end_col - src.start_col;
    if (dst.start_row < src.start_row)
        for (int y = 0; y < src.end_row - src.start_row; y++)
            memmove(&vram[(dst.start_row + y) * COLS + dst.start_col], &vram[(src.start_row + y) * COLS + src.start_col], w * sizeof(cell_t));
    else
        for (int y = src.end_row - src.start_row - 1; y >= 0; y--)
            memmove(&vram[(dst.start_row + y) * COLS + dst.start_col], &vram[(src.start_row + y) * COLS + src.start_col], w * sizeof(cell_t));
    return 1;
}

static const VTermScreenCallbacks cbs = { .damage = on_damage, .moverect = on_moverect };

static void out_cb(const char *s, size_t len, void *user) { (void)s; (void)len; (void)user; } /* replies to the host */

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
    in = IN_BASE;
    n = MMIO_INSIZE;
#endif
    MARK(0);
    VTerm *vt = vterm_new(ROWS, COLS);
    vterm_set_utf8(vt, 1);
    vterm_output_set_callback(vt, out_cb, NULL);
    scr = vterm_obtain_screen(vt);
    vterm_screen_set_callbacks(scr, &cbs, NULL);
    vterm_screen_set_damage_merge(scr, VTERM_DAMAGE_SCROLL);
    vterm_screen_enable_altscreen(scr, 1);
    vterm_screen_reset(scr, 1);
    MARK(1); /* init done */
    for (uint32_t i = 0; i < n; i += CHUNK) {
        uint32_t len = n - i < CHUNK ? n - i : CHUNK;
        { static struct vtfeed vf; vtfeed(&vf, vt, (const char *)in + i, len); }
        vterm_screen_flush_damage(scr);
    }
    MARK(2); /* the whole stream */
    uint32_t h = 2166136261u;
    const uint8_t *p = (const uint8_t *)vram;
    for (size_t k = 0; k < sizeof vram; k++) h = (h ^ p[k]) * 16777619u;
    printf("bytes %u damage %u cells %u moves %u hash %08x\n", (unsigned)n, (unsigned)n_damage, (unsigned)n_cells,
           (unsigned)n_move, (unsigned)h);
    return 0;
}

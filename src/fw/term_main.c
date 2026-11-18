/* term core firmware (Ф1): the USB serial port of the MCU (kg_mcu2 port 0) <-> libvterm (state layer) +
   our screen layer (tscreen.c) in the SRAM; the RTL (term_video.v) draws the rows of IO_ROWTAB.
   Keyboard: the HID boot report of the MCU (kbd.c) -> libvterm keyboard -> the host. */
#include <stdint.h>
#include <string.h>
#include "vterm.h"
#include "tscreen.h"
#include "mmio_hw.h"
#include "kbd.h"
#include "vtfeed.h"

static VTerm *vt;
static tscreen ts;
static uint16_t rowtab_shadow[TS_ROWS];
static uint32_t cursor_shadow = 0xffffffffu;

static void ser_putc(uint8_t c) {
    while (IO_SER_STAT & 2) {}
    IO_SER_TX = c;
}

/* libvterm -> the host (keys, replies to queries) */
static void out_cb(const char *s, size_t len, void *user) {
    (void)user;
    for (size_t i = 0; i < len; i++) ser_putc((uint8_t)s[i]);
}

/* the xterm 256-colour palette */
static void palette(void) {
    static const uint32_t base16[16] = {
        0x000000, 0xcd0000, 0x00cd00, 0xcdcd00, 0x0000ee, 0xcd00cd, 0x00cdcd, 0xe5e5e5,
        0x7f7f7f, 0xff0000, 0x00ff00, 0xffff00, 0x5c5cff, 0xff00ff, 0x00ffff, 0xffffff };
    for (int i = 0; i < 16; i++) IO_PAL(i) = base16[i];
    static const uint8_t lv[6] = { 0, 95, 135, 175, 215, 255 };
    for (int i = 0; i < 216; i++) IO_PAL(16 + i) = (uint32_t)lv[i / 36] << 16 | (uint32_t)lv[(i / 6) % 6] << 8 | lv[i % 6];
    for (int i = 0; i < 24; i++) { uint32_t g = 8 + 10 * i; IO_PAL(232 + i) = g << 16 | g << 8 | g; }
    IO_DEF_FG = 0xd0d0d0;
    IO_DEF_BG = 0x000000;
}

/* the row table and the cursor of the RTL from the screen layer */
static void push_screen(void) {
    for (int y = 0; y < TS_ROWS; y++) {
        uint16_t p = ts.row[y];
        uint16_t e = p >= TS_RING ? (uint16_t)((ALT_BASE >> 10) + (p - TS_RING)) : (uint16_t)((RING_BASE >> 10) + p);
        if (e != rowtab_shadow[y]) { rowtab_shadow[y] = e; IO_ROWTAB(y) = e; }
    }
    uint32_t shape = ts.cursor_shape == VTERM_PROP_CURSORSHAPE_UNDERLINE ? 1 :
                     ts.cursor_shape == VTERM_PROP_CURSORSHAPE_BAR_LEFT ? 2 : 0;
    uint32_t c = (uint32_t)(ts.cursor.col & 0x7f) | (uint32_t)(ts.cursor.row & 0x3f) << 8 |
                 (ts.cursor_visible ? 1u << 15 : 0) | shape << 16 | (ts.cursor_blink ? 1u << 24 : 0);
    if (c != cursor_shadow) { cursor_shadow = c; IO_CURSOR = c; }
}

/* FNV-1a of the visible cells (as bench_state.c): printed on the debug console when SW bit 31 rises
   (the sims set it; the oracle is bench_vterm on the host) */
static void print_hash(void) {
    uint32_t h = 2166136261u;
    for (int y = 0; y < TS_ROWS; y++) {
        const uint8_t *p = (const uint8_t *)tscreen_cell(&ts, y, 0);
        for (int k = 0; k < TS_COLS * (int)sizeof(ts_cell); k++) h = (h ^ p[k]) * 16777619u;
    }
    static const char hx[] = "0123456789abcdef";
    char line[16] = "HASH ";
    for (int i = 0; i < 8; i++) line[5 + i] = hx[h >> (28 - 4 * i) & 15];
    line[13] = '\n';
    for (int i = 0; i < 14; i++) IO_DBG = (uint8_t)line[i];
}

/* start-up stages on the debug port (DEBUG_ADDR low byte: kg_board.py log shows each change) */
#define STAGE(c) (IO_DBG = (c))

int main(void) {
    STAGE('a');
    vt = vterm_new(TS_ROWS, TS_COLS);
    STAGE('b');
    vterm_set_utf8(vt, 1);
    vterm_output_set_callback(vt, out_cb, NULL);
    tscreen_init(&ts, vterm_obtain_state(vt), (ts_cell *)RING_BASE, (ts_cell *)ALT_BASE);
    STAGE('c');
    for (int y = 0; y < TS_ROWS; y++) rowtab_shadow[y] = 0xffff;
    palette();
    push_screen();
    STAGE('d');
    IO_SHOW = 1;
    static const char hello[] = "\x1b[1mterm\x1b[0m: xterm-256color, 128 x 48, USB serial\r\n";
    vterm_input_write(vt, hello, sizeof hello - 1);
    push_screen();
    kbd_init(vt);
    STAGE('e');
    char buf[256];
    uint32_t sw_last = IO_SW;
    for (;;) {
        uint32_t sw = IO_SW;
        if ((sw & ~sw_last) >> 31) print_hash();
        sw_last = sw;
        int n = 0;
        uint32_t r;
        while (n < (int)sizeof buf && ((r = IO_SER_RX) & 0x100)) buf[n++] = (char)r;
        if (n) vtfeed(vt, buf, n);
        kbd_poll(vt);
        push_screen();
    }
}

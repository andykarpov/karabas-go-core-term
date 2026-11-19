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
#include <stdio.h>
#include <string.h>
#include "esp.h"
#include "telnet.h"
#include "charset.h"
#include "ui.h"
#include "book.h"

static VTerm *vt;
static tscreen ts;
static uint16_t rowtab_shadow[TS_ROWS];
static uint32_t cursor_shadow = 0xffffffffu;

static void ser_putc(uint8_t c) {
    while (IO_SER_STAT & 2) {}
    IO_SER_TX = c;
}

/* ------------------------------------------------------------------ the session (docs/PLAN.md Ф2) */

static struct conn cur;               /* the session: USB serial, telnet or raw TCP */
static int sess_on;                   /* a session is up (USB: always once chosen; TCP: connected) */

static size_t esp_tx(const uint8_t *p, size_t n) { return esp_send(p, n); }

/* libvterm -> the host (keys, replies to queries), in the charset of the session */
static void out_cb(const char *s, size_t len, void *user) {
    (void)user;
    if (ui_active() || !sess_on) return;
    uint8_t b[256];
    while (len) {
        size_t k = len > sizeof b ? sizeof b : len;
        size_t m = charset_out(cur.cs, (const uint8_t *)s, k, b);
        if (cur.proto == P_USB) for (size_t i = 0; i < m; i++) ser_putc(b[i]);
        else if (cur.proto == P_TELNET) telnet_out(b, m);
        else esp_send(b, m);
        s += k; len -= k;
    }
}

/* bytes of the host -> the screen; held while the connection screen is open */
#define HOLD 16384
static uint8_t hold[HOLD];
static size_t hold_n;

static void to_screen(const uint8_t *p, size_t n) {
    uint8_t u[1024];
    while (n) {
        size_t k = n > 256 ? 256 : n;
        size_t m = charset_in(cur.cs, p, k, u);
        vtfeed(vt, (const char *)u, m);
        p += k; n -= k;
    }
}

static void host_bytes(const uint8_t *p, size_t n) {
    if (ui_active()) { while (n-- && hold_n < HOLD) hold[hold_n++] = *p++; return; }
    to_screen(p, n);
}

static void on_tcp(const uint8_t *p, size_t n) {
    if (cur.proto == P_TELNET) {
        uint8_t d[256];
        size_t m = telnet_in(p, n, d);
        host_bytes(d, m);
    } else host_bytes(p, n);
}

static void back_to_session(void) {
    ui_close(vt);
    if (hold_n) { to_screen(hold, hold_n); hold_n = 0; }
}

static void start(const struct conn *c) {
    if (sess_on && cur.proto != P_USB) esp_close();
    cur = *c;
    sess_on = 0;
    hold_n = 0;
    if (c->proto == P_USB) {
        sess_on = 1;
        ui_session(&cur, 1);
        back_to_session();
        return;
    }
    if (!esp_ready()) { ui_status(vt, "The ESP8266 does not answer"); return; }
    if (cur.proto == P_TELNET) telnet_reset(TS_COLS, TS_ROWS, esp_tx);
    char s[128];
    snprintf(s, sizeof s, "Connecting to %s:%d ...", c->host, c->port);
    ui_status(vt, s);
    esp_connect(c->host, c->port);
}

static int key_hook(VTermKey key, uint32_t ch, VTermModifier mod, int gui) {
    (void)gui;                         /* Win = the Menu key of the firmware (Menu+Esc = OSD): not used */
    if (ch == KBD_SCROLL_LOCK) { if (ui_active() && sess_on) back_to_session(); else ui_open(vt); return 1; }
    if (ui_active() && key == VTERM_KEY_FUNCTION(8)) {
        if (sess_on && cur.proto != P_USB) esp_close();
        sess_on = 0;
        ui_session(&cur, 0);
        ui_open(vt);
        ui_status(vt, "Disconnected");
        return 1;
    }
    if (!ui_active()) return 0;
    struct conn c;
    switch (ui_key(vt, key, ch, mod, &c)) {
    case UI_CONNECT: start(&c); break;
    case UI_BACK: if (sess_on) back_to_session(); break;
    default: break;
    }
    return 1;
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

/* the state of the module and of the TCP session -> the connection screen */
static void poll_net(void) {
    static int was_ready, last_state = -1;
    static uint32_t ip_t;
    if (esp_ready() && !was_ready) {
        was_ready = 1;
        ui_status(vt, esp_ip()[0] ? "Ready" : "Ready: Wi-Fi has no IP (Setup Wi-Fi in the boot core)");
        /* the sims (src/sim/run_f2.py) set SW bit 30: telnet to 127.0.0.1:2323 at once (0 on the board:
           the core has two switch bytes) */
        if (IO_SW >> 30 & 1) { static const struct conn t = { P_TELNET, "127.0.0.1", 2323, 0 }; start(&t); }
    }
    ui_wifi(vt, esp_ip());
    if (esp_ready() && !esp_ip()[0] && (int32_t)(IO_MS - ip_t) > 5000) { ip_t = IO_MS; esp_refresh_ip(); }
    int s = esp_state();
    if (s == last_state) return;
    int prev = last_state;
    last_state = s;
    if (cur.proto == P_USB) return;
    if (s == ESP_CONNECTED) {
        sess_on = 1;
        ui_remember(&cur);                /* into the phonebook: only a connection that worked */
        ui_session(&cur, 1);
        ui_status(vt, "Connected");
        ui_close(vt);
        static const char clr[] = "\x1b[0m\x1b[2J\x1b[H";         /* a new session: a clean screen */
        vterm_input_write(vt, clr, sizeof clr - 1);
        back_to_session();
    } else if (s == ESP_IDLE && (prev == ESP_CONNECTED || prev == ESP_CONNECTING)) {
        int was = sess_on;
        sess_on = 0;
        ui_session(&cur, 0);
        static const char closed[] = "\r\n\x1b[0;7m[connection closed]\x1b[0m\r\n";
        if (was) vtfeed(vt, closed, sizeof closed - 1);
        ui_open(vt);
        char m[96];
        snprintf(m, sizeof m, "%s%s", was ? "Disconnected" : "Failed", esp_error()[0] ? ": " : "");
        if (esp_error()[0]) strncat(m, esp_error(), sizeof m - strlen(m) - 1);
        ui_status(vt, m);
    }
}

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
    static const char hello[] = "\x1b[1mterm\x1b[0m: xterm-256color, 128 x 48\r\n";
    vterm_input_write(vt, hello, sizeof hello - 1);
    push_screen();
    kbd_init(vt);
    kbd_hook = key_hook;
    esp_on_data = on_tcp;
    esp_init();
    book_init();
    ui_open(vt);
    ui_status(vt, "Looking for the ESP8266 ...");
    push_screen();
    STAGE('e');
    char buf[256];
    uint32_t sw_last = IO_SW;
    for (;;) {
        uint32_t sw = IO_SW;
        if ((sw & ~sw_last) >> 31) print_hash();
        sw_last = sw;
        int n = 0;
        uint32_t r;
        /* the USB serial port: read only while it is the session and the screen shows it */
        while (sess_on && cur.proto == P_USB && !ui_active() && n < (int)sizeof buf && ((r = IO_SER_RX) & 0x100)) buf[n++] = (char)r;
        if (n) to_screen((const uint8_t *)buf, (size_t)n);
        esp_poll();
        poll_net();
        { static int was; book_poll(); if (book_ready() != was) { was = book_ready(); ui_redraw(); } }
        kbd_poll(vt);
        push_screen();
    }
}

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
#include "ssh.h"
#include "rng.h"
#include "store.h"
#include "sshkeys.h"
#include "monocypher.h"

static VTerm *vt;
static tscreen ts;
static uint16_t rowtab_shadow[TS_ROWS];
static uint32_t cursor_shadow = 0xffffffffu;
/* the scrollback view (Shift+PgUp / Shift+PgDn on the primary screen): rows above the screen shown, 0 = none */
static int sb;
static uint32_t sb_top;               /* ts.top when sb was set: new output keeps the view where it is */
static void sb_scroll(int d);

static void ser_putc(uint8_t c) {
    while (IO_SER_STAT & 2) {}
    IO_SER_TX = c;
}

/* ------------------------------------------------------------------ the session (docs/PLAN.md Ф2) */

static struct conn cur;               /* the session: USB serial, telnet or raw TCP */
static int sess_on;                   /* a session is up (USB: always once chosen; TCP: connected) */

static size_t esp_tx(const uint8_t *p, size_t n) { return esp_send(p, n); }

/* SSH (Ф3): the keys of the user wait here for the window of the channel */
#define KQ 4096
static uint8_t kq[KQ];
static size_t kq_r, kq_w;
static void kq_put(const uint8_t *p, size_t n) { while (n-- && kq_w - kq_r < KQ) kq[kq_w++ % KQ] = *p++; }
static void line_byte(uint8_t c);
static int line_on;                   /* a question of ssh.c is being answered on the screen */

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
        else if (cur.proto == P_SSH) { if (line_on) for (size_t i = 0; i < m; i++) line_byte(b[i]); else kq_put(b, m); }
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
    rng_event(1);
    if (cur.proto == P_SSH) { ssh_in(p, n); return; }
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

/* ------------------------------------------------------------------ SSH */
static const struct ssh_io sio = { esp_tx, rng_bytes, host_bytes, sshkeys_host, sshkeys_user };
static char line[256];
static size_t line_n;
static int line_echo, line_esc;
static char ssh_end[128];             /* why the SSH session ended: for the status line */
static int ssh_ended;

/* a line for a question of ssh.c: typed on the session screen (no echo for secrets) */
static void line_byte(uint8_t c) {
    if (line_esc == 1) { line_esc = (c == '[' || c == 'O') ? 2 : 0; return; }   /* keys like the arrows: skipped */
    if (line_esc == 2) { if (c >= 0x40 && c <= 0x7e) line_esc = 0; return; }
    if (c == 0x1b) { line_esc = 1; return; }
    if (c == '\r' || c == '\n') {
        line[line_n] = 0;
        host_bytes((const uint8_t *)"\r\n", 2);
        line_on = 0;
        ssh_answer(line);
        crypto_wipe(line, sizeof line);
        line_n = 0;
        return;
    }
    if (c == 0x7f || c == 8) {
        if (!line_n) return;
        do line_n--; while (line_n && (line[line_n] & 0xc0) == 0x80);
        if (line_echo) host_bytes((const uint8_t *)"\b \b", 3);
        return;
    }
    if (c == 3) { host_bytes((const uint8_t *)"^C\r\n", 4); line_on = 0; crypto_wipe(line, sizeof line); line_n = 0; ssh_close(); return; }
    if (c == 0x15) { while (line_n) { line_n--; if (line_echo && (line[line_n] & 0xc0) != 0x80) host_bytes((const uint8_t *)"\b \b", 3); } return; }
    if (c < 0x20 || line_n >= sizeof line - 1) return;
    line[line_n++] = (char)c;
    if (line_echo) host_bytes(&c, 1);
}

static void ssh_service(void) {
    if (cur.proto != P_SSH || !sess_on) return;
    while (kq_r != kq_w) {
        size_t at = kq_r % KQ, n = kq_w - kq_r;
        if (n > KQ - at) n = KQ - at;
        size_t k = ssh_write(kq + at, n);
        if (!k) break;
        kq_r += k;
    }
    ssh_poll();
    const char *prompt;
    int echo;
    if (!line_on && ssh_ask(&prompt, &echo)) {
        host_bytes((const uint8_t *)prompt, strlen(prompt));
        line_on = 1; line_echo = echo; line_n = 0; line_esc = 0;
    }
    { static int was_open; int o = ssh_state() == SSH_OPEN; if (o && !was_open) ui_remember(&cur); was_open = o; }
    if (ssh_state() == SSH_CLOSED && !ssh_ended) {
        ssh_ended = 1;
        line_on = 0;
        snprintf(ssh_end, sizeof ssh_end, "%s", ssh_error());
        esp_close();
    }
}

/* F2 on the connection screen: the public key line typed into the session (echo ... >> authorized_keys) */
static void type_key(void) {
    if (!sess_on) { ui_status(vt, "No session to type the key into"); return; }
    if (!sshkeys_user()) { ui_status(vt, "Not enough randomness for a key yet: press some keys and try again"); return; }
    char l[160];
    ssh_pubkey_line(sshkeys_user_pub(), "karabas-term", l, sizeof l);
    back_to_session();
    vterm_keyboard_start_paste(vt);       /* ESC [200~ if the application asked for bracketed paste */
    out_cb(l, strlen(l), NULL);
    vterm_keyboard_end_paste(vt);
}

static void start(const struct conn *c) {
    if (sess_on && cur.proto == P_SSH) ssh_close();
    if (sess_on && cur.proto != P_USB) esp_close();
    if (c->proto == P_SSH && !rng_ready()) { ui_status(vt, "Not enough randomness yet: press some keys and try again"); return; }
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
    if (cur.proto == P_SSH) { sshkeys_reseed(); ssh_ended = 0; ssh_end[0] = 0; kq_r = kq_w = 0; line_on = 0; }
    char s[128];
    snprintf(s, sizeof s, "Connecting to %s:%d ...", c->host, c->port);
    ui_status(vt, s);
    esp_connect(c->host, c->port);
}

static int key_hook(VTermKey key, uint32_t ch, VTermModifier mod, int gui) {
    (void)gui;                         /* Win = the Menu key of the firmware (Menu+Esc = OSD): not used */
    if (ch == KBD_SCROLL_LOCK) { if (ui_active() && sess_on) back_to_session(); else ui_open(vt); return 1; }
    if (ui_active() && key == VTERM_KEY_FUNCTION(8)) {
        if (sess_on && cur.proto == P_SSH) { ssh_close(); ssh_poll(); }
        if (sess_on && cur.proto != P_USB) esp_close();
        sess_on = 0;
        line_on = 0;
        ui_session(&cur, 0);
        ui_open(vt);
        ui_status(vt, "Disconnected");
        return 1;
    }
    if (!ui_active()) {
        if ((key == VTERM_KEY_PAGEUP || key == VTERM_KEY_PAGEDOWN) && (mod & VTERM_MOD_SHIFT) && !ts.altscreen) {
            sb_scroll(key == VTERM_KEY_PAGEUP ? TS_ROWS / 2 : -TS_ROWS / 2);
            return 1;
        }
        if (sb) sb = 0;                 /* any other key: back to the screen */
        return 0;
    }
    struct conn c;
    switch (ui_key(vt, key, ch, mod, &c)) {
    case UI_CONNECT: start(&c); break;
    case UI_BACK: if (sess_on) back_to_session(); break;
    case UI_TYPE_KEY: type_key(); break;
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

static int sb_max(void) { int m = (int)(ts.top < TS_RING - TS_ROWS ? ts.top : TS_RING - TS_ROWS); return m; }
static void sb_scroll(int d) {
    sb += d;
    if (sb > sb_max()) sb = sb_max();
    if (sb < 0) sb = 0;
    sb_top = ts.top;
}

/* the mouse (xterm mouse tracking of libvterm, when the application asks for it): the pointer = the cell
   under it in reverse (the RTL draws no pointer) */
#define ATTR_REVERSE (32u << 22)          /* tscreen.c repack_pen: reverse = attr bit 5 */
static int mx = 512, my = 384, mrow = -1, mcol = -1, mbtn;
static volatile uint32_t *mptr;       /* the w0 of the cell with the pointer */
static uint32_t mshown;               /* its value with the pointer in it */

static void pointer_off(void) {
    if (mptr && *mptr == mshown) *mptr = mshown ^ ATTR_REVERSE;
    mptr = 0;
}
static void pointer_on(void) {
    volatile uint32_t *p = (volatile uint32_t *)&tscreen_cell(&ts, mrow, mcol)->w0;
    if (p == mptr && *p == mshown) return;
    if (p != mptr) pointer_off();
    mptr = p;
    mshown = *p ^ ATTR_REVERSE;
    *p = mshown;
}

static void poll_mouse(void) {
    uint32_t m = IO_MOUSE;
    int dx = (int32_t)(m << 20) >> 20, dy = (int32_t)(m << 8) >> 20, b = (int)(m >> 24) & 7;
    if (dx || dy || b != mbtn) rng_event(1);
    mx += dx; my += dy;
    if (mx < 0) mx = 0;
    if (mx > TS_COLS * 8 - 1) mx = TS_COLS * 8 - 1;
    if (my < 0) my = 0;
    if (my > TS_ROWS * 16 - 1) my = TS_ROWS * 16 - 1;
    int on = ts.mouse && !ui_active() && !sb && sess_on;
    if (!on) { pointer_off(); mbtn = b; mrow = -1; return; }
    int r = my / 16, c = mx / 8;
    if (r != mrow || c != mcol) { mrow = r; mcol = c; vterm_mouse_move(vt, r, c, VTERM_MOD_NONE); }
    static const int xb[3] = { 1, 3, 2 };    /* bit 0 left, 1 right, 2 middle -> xterm 1 left, 2 middle, 3 right */
    for (int i = 0; i < 3; i++)
        if (((b ^ mbtn) >> i) & 1) vterm_mouse_button(vt, xb[i], (b >> i) & 1, VTERM_MOD_NONE);
    mbtn = b;
    pointer_on();
}

/* the row table and the cursor of the RTL from the screen layer */
static void push_screen(void) {
    if (sb && (ts.altscreen || ui_active())) sb = 0;
    if (sb && ts.top != sb_top) sb_scroll((int)(ts.top - sb_top));
    for (int y = 0; y < TS_ROWS; y++) {
        uint16_t p = y < sb ? (uint16_t)((ts.row[0] + TS_RING * 2 - sb + y) % TS_RING) : ts.row[y - sb];
        uint16_t e = p >= TS_RING ? (uint16_t)((ALT_BASE >> 10) + (p - TS_RING)) : (uint16_t)((RING_BASE >> 10) + p);
        if (e != rowtab_shadow[y]) { rowtab_shadow[y] = e; IO_ROWTAB(y) = e; }
    }
    uint32_t shape = ts.cursor_shape == VTERM_PROP_CURSORSHAPE_UNDERLINE ? 1 :
                     ts.cursor_shape == VTERM_PROP_CURSORSHAPE_BAR_LEFT ? 2 : 0;
    uint32_t c = (uint32_t)(ts.cursor.col & 0x7f) | (uint32_t)(ts.cursor.row & 0x3f) << 8 |
                 (ts.cursor_visible && !sb ? 1u << 15 : 0) | shape << 16 | (ts.cursor_blink ? 1u << 24 : 0);
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
        if (IO_SW >> 30 & 1) { static const struct conn t = { P_TELNET, "127.0.0.1", 2323, 0, "" }; start(&t); }
        /* SW bit 29 (src/sim/run_f3.py): SSH to 127.0.0.1:2224 as "test" (the key and known_hosts: the save file of the sim) */
        if (IO_SW >> 29 & 1) { static const struct conn t = { P_SSH, "127.0.0.1", 2224, 0, "test" }; start(&t); }
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
        if (cur.proto != P_SSH) ui_remember(&cur);   /* into the phonebook: only a connection that worked (SSH: logged in) */
        ui_session(&cur, 1);
        ui_status(vt, "Connected");
        ui_close(vt);
        static const char clr[] = "\x1b[0m\x1b[2J\x1b[H";         /* a new session: a clean screen */
        vterm_input_write(vt, clr, sizeof clr - 1);
        back_to_session();
        if (cur.proto == P_SSH) ssh_start(&sio, cur.host, cur.port, cur.user, TS_COLS, TS_ROWS);
    } else if (s == ESP_IDLE && (prev == ESP_CONNECTED || prev == ESP_CONNECTING)) {
        int was = sess_on;
        sess_on = 0;
        ui_session(&cur, 0);
        static const char closed[] = "\r\n\x1b[0;7m[connection closed]\x1b[0m\r\n";
        if (was && cur.proto == P_SSH && ssh_end[0]) { char e[160]; snprintf(e, sizeof e, "\r\n\x1b[0;7m[ssh: %s]\x1b[0m", ssh_end); vtfeed(vt, e, strlen(e)); }
        if (was) vtfeed(vt, closed, sizeof closed - 1);
        ui_open(vt);
        char m[96];
        snprintf(m, sizeof m, "%s%s", was ? "Disconnected" : "Failed", esp_error()[0] ? ": " : "");
        if (cur.proto == P_SSH && ssh_end[0]) snprintf(m, sizeof m, "%s: %s", was ? "Disconnected" : "Failed", ssh_end);
        else if (esp_error()[0]) strncat(m, esp_error(), sizeof m - strlen(m) - 1);
        ui_status(vt, m);
    }
}

int main(void) {
    STAGE('a');
    {   /* the power-up contents of SRAM nobody loads (under the stack): some randomness after a power-up */
        extern char __heap_end[];
        rng_mix((const void *)((uintptr_t)__heap_end - 8192u), 8192, 0);   /* 64 KB took ~3 frames */
    }
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
    store_init();
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
        store_poll();
        sshkeys_poll();
        { static int was; book_poll(); if (book_ready() != was) { was = book_ready(); ui_redraw(); } }
        { static uint32_t kn; uint32_t k = IO_KBDN; if (k != kn) { kn = k; rng_event(2); } }
        ssh_service();
        { static const uint8_t *pk; if (sshkeys_user_pub() != pk) { pk = sshkeys_user_pub(); char fp[52] = ""; if (pk) ssh_fingerprint(pk, fp); ui_keyfp(fp); } }
        kbd_poll(vt);
        poll_mouse();
        { static uint32_t bells; if (ts.bells != bells) { bells = ts.bells; IO_BELL = 1; } }
        push_screen();
    }
}

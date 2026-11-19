/* term core firmware: up to NTAB tabs (docs/PLAN.md Ф5), each a session (USB serial of the MCU, telnet, raw TCP or
   SSH through the ESP8266) on its own libvterm (state layer) + our screen layer (tscreen.c) in the SRAM; the
   connection screen (ui.c) has its own libvterm too. The RTL (term_video.v) draws the rows of IO_ROWTAB: row 0 =
   the tab bar, rows 1..47 = the tab shown (or the connection screen).
   Keyboard: the HID boot report of the MCU (kbd.c) -> libvterm keyboard of the tab shown -> its host.
   Mouse: the pointer (IO_POINTER, drawn by the RTL), clicks on the tab bar, xterm mouse tracking of the
   application, the wheel (tracking: buttons 4 / 5; else the history or arrow keys). */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "vterm.h"
#include "tscreen.h"
#include "mmio_hw.h"
#include "kbd.h"
#include "vtfeed.h"
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

#define NTAB ESP_LINKS                 /* a tab = an ESP link id */
#define KQ   2048

#define A_BOLD    (1u << 22)              /* tscreen.c repack_pen: attr bits 22.. */
#define A_REVERSE (32u << 22)
#define COL(fg, bg) ((uint32_t)(fg) | (uint32_t)(bg) << 16)

struct tab {
    int used;
    int idx;
    VTerm *vt;
    tscreen ts;
    struct vtfeed vf;
    struct cs_out cso;
    int ui;                            /* the connection screen is shown on this tab */
    struct conn c;                     /* the session (or the last one) */
    int on;                            /* a session is up (USB: once chosen; TCP: connected) */
    int last_es;                       /* the ESP state of the link seen */
    struct telnet tn;
    struct ssh *ssh;
    char ssh_end[128];                 /* why the SSH session ended: for the status line */
    int ssh_ended, ssh_was_open;
    uint8_t kq[KQ];                    /* SSH: the keys of the user wait here for the window of the channel */
    size_t kq_r, kq_w;
    char line[256];                    /* a question of ssh.c answered on the screen */
    size_t line_n;
    int line_on, line_echo, line_esc;
    int sb;                            /* the history view (Shift+PgUp): rows above the screen shown */
    uint32_t sb_top;
    char status[128];                  /* the status line of the connection screen for this tab */
    uint32_t bells;
    int activity;                      /* output while not shown */
};
static struct tab tabs[NTAB];
static int curt;                       /* the tab shown */
#define T (&tabs[curt])

/* the screens in the program part of the SRAM: rows are 1 KB, the row table gives address / 1024 */
static ts_cell alt_cells[NTAB][TS_ROWS * TS_COLS] __attribute__((aligned(1024)));
static ts_cell ui_cells[TS_ROWS * TS_COLS] __attribute__((aligned(1024)));   /* the ring and the alt of the ui (it draws on the alt only) */
static ts_cell bar[TS_COLS] __attribute__((aligned(1024)));
static VTerm *ui_vt;
static tscreen ui_ts;

static uint16_t rowtab_shadow[TS_ROWS + 1];
static uint32_t cursor_shadow = 0xffffffffu, pointer_shadow = 0xffffffffu;
static int bar_dirty = 1;
static void sb_scroll(struct tab *t, int d);
static void switch_tab(int i);

static void ser_putc(uint8_t c) {
    while (IO_SER_STAT & 2) {}
    IO_SER_TX = c;
}

static void tab_status(struct tab *t, const char *s) {
    snprintf(t->status, sizeof t->status, "%s", s);
    if (t == T) ui_status(s);
}

static int usb_tab(void) {             /* the tab with the USB serial session, or -1 */
    for (int i = 0; i < NTAB; i++) if (tabs[i].used && tabs[i].on && tabs[i].c.proto == P_USB) return i;
    return -1;
}

/* ------------------------------------------------------------------ the sessions */

static size_t esp_tx(void *ctx, const uint8_t *p, size_t n) { return esp_send(((struct tab *)ctx)->idx, p, n); }
static void line_byte(struct tab *t, uint8_t c);
static void kq_put(struct tab *t, const uint8_t *p, size_t n) { while (n-- && t->kq_w - t->kq_r < KQ) t->kq[t->kq_w++ % KQ] = *p++; }

/* libvterm of a tab -> its host (keys, replies to queries), in the charset of the session */
static void out_cb(const char *s, size_t len, void *user) {
    struct tab *t = user;
    if (!t->on) return;
    uint8_t b[256];
    while (len) {
        size_t k = len > sizeof b ? sizeof b : len;
        size_t m = charset_out(t->c.cs, &t->cso, (const uint8_t *)s, k, b);
        if (t->c.proto == P_USB) for (size_t i = 0; i < m; i++) ser_putc(b[i]);
        else if (t->c.proto == P_TELNET) telnet_out(&t->tn, b, m);
        else if (t->c.proto == P_SSH) { if (t->line_on) for (size_t i = 0; i < m; i++) line_byte(t, b[i]); else kq_put(t, b, m); }
        else esp_send(t->idx, b, m);
        s += k; len -= k;
    }
}

/* bytes of the host -> the screen of the tab (shown or not) */
static void to_screen(struct tab *t, const uint8_t *p, size_t n) {
    uint8_t u[1024];
    while (n) {
        size_t k = n > 256 ? 256 : n;
        size_t m = charset_in(t->c.cs, p, k, u);
        vtfeed(&t->vf, t->vt, (const char *)u, m);
        p += k; n -= k;
    }
    if ((t != T || t->ui) && !t->activity) { t->activity = 1; bar_dirty = 1; }
}
static void host_bytes(void *ctx, const uint8_t *p, size_t n) { to_screen(ctx, p, n); }
static void say(struct tab *t, const char *s) { to_screen(t, (const uint8_t *)s, strlen(s)); }

static void on_tcp(int id, const uint8_t *p, size_t n) {
    rng_event(1);
    if (id < 0 || id >= NTAB || !tabs[id].used) return;
    struct tab *t = &tabs[id];
    if (t->c.proto == P_SSH) { if (t->ssh) ssh_in(t->ssh, p, n); return; }
    if (t->c.proto == P_TELNET) {
        uint8_t d[256];
        size_t m = telnet_in(&t->tn, p, n, d);
        to_screen(t, d, m);
    } else to_screen(t, p, n);
}

static void show_session(struct tab *t) { if (t->ui) { t->ui = 0; bar_dirty = 1; } }
static void show_ui(struct tab *t) {
    if (!t->ui) { t->ui = 1; bar_dirty = 1; }
    if (t == T) { ui_session(&t->c, t->on); ui_status(t->status); }
}

/* ------------------------------------------------------------------ SSH */
static const struct ssh_io sio = { esp_tx, rng_bytes, host_bytes, sshkeys_host, sshkeys_user };

/* a line for a question of ssh.c: typed on the session screen (no echo for secrets) */
static void line_byte(struct tab *t, uint8_t c) {
    if (t->line_esc == 1) { t->line_esc = (c == '[' || c == 'O') ? 2 : 0; return; }   /* keys like the arrows: skipped */
    if (t->line_esc == 2) { if (c >= 0x40 && c <= 0x7e) t->line_esc = 0; return; }
    if (c == 0x1b) { t->line_esc = 1; return; }
    if (c == '\r' || c == '\n') {
        t->line[t->line_n] = 0;
        say(t, "\r\n");
        t->line_on = 0;
        ssh_answer(t->ssh, t->line);
        crypto_wipe(t->line, sizeof t->line);
        t->line_n = 0;
        return;
    }
    if (c == 0x7f || c == 8) {
        if (!t->line_n) return;
        do t->line_n--; while (t->line_n && (t->line[t->line_n] & 0xc0) == 0x80);
        if (t->line_echo) say(t, "\b \b");
        return;
    }
    if (c == 3) { say(t, "^C\r\n"); t->line_on = 0; crypto_wipe(t->line, sizeof t->line); t->line_n = 0; ssh_close(t->ssh); return; }
    if (c == 0x15) { while (t->line_n) { t->line_n--; if (t->line_echo && (t->line[t->line_n] & 0xc0) != 0x80) say(t, "\b \b"); } return; }
    if (c < 0x20 || t->line_n >= sizeof t->line - 1) return;
    t->line[t->line_n++] = (char)c;
    if (t->line_echo) to_screen(t, &c, 1);
}

static void ssh_service(struct tab *t) {
    if (!t->used || t->c.proto != P_SSH || !t->on || !t->ssh) return;
    struct ssh *s = t->ssh;
    while (t->kq_r != t->kq_w) {
        size_t at = t->kq_r % KQ, n = t->kq_w - t->kq_r;
        if (n > KQ - at) n = KQ - at;
        size_t k = ssh_write(s, t->kq + at, n);
        if (!k) break;
        t->kq_r += k;
    }
    ssh_poll(s);
    const char *prompt;
    int echo;
    if (!t->line_on && ssh_ask(s, &prompt, &echo)) {
        say(t, prompt);
        t->line_on = 1; t->line_echo = echo; t->line_n = 0; t->line_esc = 0;
    }
    int o = ssh_state(s) == SSH_OPEN;
    if (o && !t->ssh_was_open) ui_remember(&t->c);
    t->ssh_was_open = o;
    if (ssh_state(s) == SSH_CLOSED && !t->ssh_ended) {
        t->ssh_ended = 1;
        t->line_on = 0;
        snprintf(t->ssh_end, sizeof t->ssh_end, "%s", ssh_error(s));
        esp_close(t->idx);
    }
}

/* F2 on the connection screen: the public key line typed into the session (echo ... >> authorized_keys) */
static void type_key(struct tab *t) {
    if (!t->on) { tab_status(t, "No session to type the key into"); return; }
    if (!sshkeys_user()) { tab_status(t, "Not enough randomness for a key yet: press some keys and try again"); return; }
    char l[160];
    ssh_pubkey_line(sshkeys_user_pub(), "karabas-term", l, sizeof l);
    show_session(t);
    vterm_keyboard_start_paste(t->vt);    /* ESC [200~ if the application asked for bracketed paste */
    out_cb(l, strlen(l), t);
    vterm_keyboard_end_paste(t->vt);
}

/* the session of a tab goes away (F8, a new connection on the tab, closing the tab) */
static void hang_up(struct tab *t) {
    if (t->on && t->c.proto == P_SSH && t->ssh) { ssh_close(t->ssh); ssh_poll(t->ssh); }
    if (t->c.proto != P_USB) esp_close(t->idx);
    t->on = 0;
    t->line_on = 0;
    bar_dirty = 1;
}

static void start(struct tab *t, const struct conn *c) {
    if (c->proto == P_USB) {
        int u = usb_tab();
        if (u >= 0 && u != t->idx) { char m[64]; snprintf(m, sizeof m, "The USB serial port is open in tab %d", u + 1); tab_status(t, m); return; }
    }
    if (c->proto == P_SSH && !rng_ready()) { tab_status(t, "Not enough randomness yet: press some keys and try again"); return; }
    if (t->on || esp_state(t->idx) != ESP_IDLE) hang_up(t);
    t->c = *c;
    t->on = 0;
    t->ssh_was_open = 0;
    bar_dirty = 1;
    if (c->proto == P_USB) {
        t->on = 1;
        if (t == T) ui_session(&t->c, 1);
        show_session(t);
        return;
    }
    if (!esp_ready()) { tab_status(t, "The ESP8266 does not answer"); return; }
    if (c->proto == P_TELNET) telnet_reset(&t->tn, TS_COLS, TS_ROWS, esp_tx, t);
    if (c->proto == P_SSH) {
        if (!t->ssh) t->ssh = ssh_new();
        if (!t->ssh) { tab_status(t, "No free SSH session"); return; }
        sshkeys_reseed(); t->ssh_ended = 0; t->ssh_end[0] = 0; t->kq_r = t->kq_w = 0; t->line_on = 0;
    }
    char s[128];
    snprintf(s, sizeof s, "Connecting to %s:%d ...", c->host, c->port);
    tab_status(t, s);
    esp_connect(t->idx, c->host, c->port);
}

/* ------------------------------------------------------------------ the tabs */

static void tab_init(struct tab *t) {
    t->used = 1;
    t->ui = 1;
    t->on = 0;
    t->last_es = ESP_IDLE;
    t->sb = 0;
    t->activity = 0;
    t->status[0] = 0;
    t->bells = 0;
    memset(&t->vf, 0, sizeof t->vf);
    memset(&t->cso, 0, sizeof t->cso);
    memset(&t->c, 0, sizeof t->c);
    tscreen_init(&t->ts, vterm_obtain_state(t->vt), (ts_cell *)(RING_BASE + (uint32_t)t->idx * RING_SIZE), alt_cells[t->idx]);
    t->bells = t->ts.bells;
    bar_dirty = 1;
}

static void new_tab(void) {
    for (int i = 0; i < NTAB; i++) {
        if (tabs[i].used) continue;
        tab_init(&tabs[i]);
        tab_status(&tabs[i], esp_ready() ? "New tab" : "Looking for the ESP8266 ...");
        switch_tab(i);
        return;
    }
    tab_status(T, "4 tabs at most");
    show_ui(T);
}

static void close_tab(int i) {
    struct tab *t = &tabs[i];
    hang_up(t);
    if (t->ssh) { ssh_free(t->ssh); t->ssh = 0; }
    t->used = 0;
    bar_dirty = 1;
    if (i != curt) return;
    for (int k = 1; k < NTAB; k++) {          /* the tab before it, else after it */
        int j = (i + NTAB - k) % NTAB;
        if (tabs[j].used) { switch_tab(j); return; }
    }
    tab_init(t);                               /* the last one: a new empty tab */
    tab_status(t, "Ready");
    switch_tab(i);
}

static void switch_tab(int i) {
    if (i < 0 || i >= NTAB || !tabs[i].used) return;
    curt = i;
    T->activity = 0;
    ui_session(&T->c, T->on);
    ui_status(T->status);
    bar_dirty = 1;
}

static void step_tab(int d) {
    for (int k = 1; k < NTAB; k++) {
        int j = (curt + NTAB + d * k) % NTAB;
        if (tabs[j].used) { switch_tab(j); return; }
    }
}

/* Ctrl+Shift+W on a tab with a session: asked in the tab bar */
static int confirm = -1;               /* the tab asked about, -1 none */
static void ask_close(void) {
    if (T->on || esp_state(curt) != ESP_IDLE) { confirm = curt; bar_dirty = 1; }
    else close_tab(curt);
}

/* ------------------------------------------------------------------ the tab bar (row 0) */

static int bx0[NTAB], bx1[NTAB], bxc[NTAB], bplus = -1, by = -1, bn = -1;   /* columns for the mouse */

static int bar_put(int x, const char *s, uint32_t attr, uint32_t col) {
    /* UTF-8 (2 / 3 bytes) of the narrow glyphs we use */
    while (*s && x < TS_COLS) {
        uint32_t c = (uint8_t)*s++;
        if (c >= 0xe0) { c = (c & 15) << 12 | ((uint8_t)s[0] & 63) << 6 | ((uint8_t)s[1] & 63); s += 2; }
        else if (c >= 0xc0) { c = (c & 31) << 6 | ((uint8_t)s[0] & 63); s += 1; }
        bar[x].w0 = c | attr;
        bar[x].w1 = col;
        x++;
    }
    return x;
}

static void tab_label(const struct tab *t, char *s, size_t n) {
    if (t->c.proto == P_USB && t->on) { snprintf(s, n, "USB serial"); return; }
    if (!t->on && esp_state(t->idx) == ESP_IDLE) { snprintf(s, n, "%s", t->c.host[0] ? t->c.host : "new"); return; }
    snprintf(s, n, "%.20s%s", t->c.host, t->on ? "" : "\xe2\x80\xa6");    /* … while connecting */
}

static void draw_bar(void) {
    const uint32_t base = COL(250, 236);
    for (int x = 0; x < TS_COLS; x++) { bar[x].w0 = ' '; bar[x].w1 = base; }
    by = bn = bplus = -1;
    for (int i = 0; i < NTAB; i++) bx0[i] = bx1[i] = bxc[i] = -1;
    int x = 0;
    if (confirm >= 0) {
        char s[96], l[48];
        tab_label(&tabs[confirm], l, sizeof l);
        snprintf(s, sizeof s, " Close tab %d (%s)? The session ends. ", confirm + 1, l);
        x = bar_put(0, s, A_BOLD, COL(255, 52));
        by = x;
        x = bar_put(x, " Y yes ", A_BOLD, COL(16, 214));
        x = bar_put(x, " ", 0, base);
        bn = x;
        bar_put(x, " N no ", A_BOLD, COL(16, 250));
        return;
    }
    for (int i = 0; i < NTAB; i++) {
        const struct tab *t = &tabs[i];
        if (!t->used) continue;
        int act = i == curt;
        uint32_t col = act ? COL(255, 24) : COL(250, 238);
        char s[48], l[40];
        tab_label(t, l, sizeof l);
        snprintf(s, sizeof s, " %d %s", i + 1, l);
        bx0[i] = x;
        x = bar_put(x, s, act ? A_BOLD : 0, col);
        x = bar_put(x, t->activity ? "\xe2\x80\xa2" : " ", A_BOLD, act ? col : COL(214, 238));   /* • new output */
        bxc[i] = x;
        x = bar_put(x, "\xc3\x97 ", 0, act ? COL(153, 24) : COL(244, 238));                   /* × closes */
        bx1[i] = x;
        x = bar_put(x, " ", 0, base);
    }
    int used = 0;
    for (int i = 0; i < NTAB; i++) used += tabs[i].used;
    if (used < NTAB) { bplus = x; x = bar_put(x, " + ", A_BOLD, COL(114, 238)); }
    char w[64];
    snprintf(w, sizeof w, "Wi-Fi %s ", esp_ip()[0] ? esp_ip() : "no IP");
    int r = TS_COLS - (int)strlen(w);
    if (r > x) bar_put(r, w, 0, COL(244, 236));
}

static void bar_click(int col) {
    if (confirm >= 0) {
        if (by >= 0 && col >= by && col < by + 7) { int i = confirm; confirm = -1; close_tab(i); }
        else if (bn >= 0 && col >= bn && col < bn + 6) confirm = -1;
        bar_dirty = 1;
        return;
    }
    for (int i = 0; i < NTAB; i++) {
        if (bx0[i] < 0 || col < bx0[i] || col >= bx1[i]) continue;
        if (col >= bxc[i]) { switch_tab(i); ask_close(); }
        else switch_tab(i);
        return;
    }
    if (bplus >= 0 && col >= bplus && col < bplus + 3) new_tab();
}

/* ------------------------------------------------------------------ the keys */

static int key_hook(VTermKey key, uint32_t ch, VTermModifier mod, int gui);

static int pvis;                        /* the pointer is shown */
static uint32_t p_t;                    /* ms of the last move */

static int key_hook(VTermKey key, uint32_t ch, VTermModifier mod, int gui) {
    (void)gui;                         /* Win = the Menu key of the firmware (Menu+Esc = OSD): not used */
    pvis = 0;
    struct tab *t = T;
    if (confirm >= 0) {
        int i = confirm;
        confirm = -1;
        bar_dirty = 1;
        if (ch == 'y' || ch == 'Y' || key == VTERM_KEY_ENTER) close_tab(i);
        return 1;
    }
    int ctrl = (mod & VTERM_MOD_CTRL) != 0, shift = (mod & VTERM_MOD_SHIFT) != 0;
    if (ctrl && shift && key == VTERM_KEY_NONE) {
        if (ch == 'T' || ch == 't') { new_tab(); return 1; }
        if (ch == 'W' || ch == 'w') { ask_close(); return 1; }
        static const char nums[] = "!@#$";               /* Shift+1..4 (Ctrl+key: the US layout) */
        for (int i = 0; i < NTAB; i++) if (ch == (uint32_t)nums[i]) { switch_tab(i); return 1; }
    }
    if (ctrl && !shift && (key == VTERM_KEY_PAGEUP || key == VTERM_KEY_PAGEDOWN)) { step_tab(key == VTERM_KEY_PAGEUP ? -1 : 1); return 1; }
    if (ch == KBD_SCROLL_LOCK) { if (t->ui && t->on) show_session(t); else show_ui(t); return 1; }
    if (t->ui && key == VTERM_KEY_FUNCTION(8)) {
        int was = t->on || esp_state(t->idx) != ESP_IDLE;
        hang_up(t);
        ui_session(&t->c, 0);
        tab_status(t, was ? "Disconnected" : "No session");
        return 1;
    }
    if (!t->ui) {
        if ((key == VTERM_KEY_PAGEUP || key == VTERM_KEY_PAGEDOWN) && shift && !t->ts.altscreen) {
            sb_scroll(t, key == VTERM_KEY_PAGEUP ? TS_ROWS / 2 : -TS_ROWS / 2);
            return 1;
        }
        if (t->sb) t->sb = 0;           /* any other key: back to the screen */
        return 0;
    }
    struct conn c;
    switch (ui_key(key, ch, mod, &c)) {
    case UI_CONNECT: start(t, &c); break;
    case UI_BACK: if (t->on) show_session(t); break;
    case UI_TYPE_KEY: type_key(t); break;
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

static int sb_max(const struct tab *t) { return (int)(t->ts.top < TS_RING - TS_ROWS ? t->ts.top : TS_RING - TS_ROWS); }
static void sb_scroll(struct tab *t, int d) {
    t->sb += d;
    if (t->sb > sb_max(t)) t->sb = sb_max(t);
    if (t->sb < 0) t->sb = 0;
    t->sb_top = t->ts.top;
}

/* ------------------------------------------------------------------ the mouse */
static int mx = 512, my = 384, mbtn, grab;   /* grab: a press went to the application (its release goes there too) */

static void poll_mouse(void) {
    uint32_t m = IO_MOUSE;
    int dx = (int32_t)(m << 20) >> 20, dy = (int32_t)(m << 8) >> 20, b = (int)(m >> 24) & 7;
    int wh = (int8_t)(IO_WHEEL & 0xff);
    uint32_t now = IO_MS;
    if (dx || dy || b != mbtn || wh) { rng_event(1); pvis = 1; p_t = now; }
    if (pvis && (int32_t)(now - p_t) > 3000) pvis = 0;
    mx += dx; my += dy;
    if (mx < 0) mx = 0;
    if (mx > TS_COLS * 8 - 1) mx = TS_COLS * 8 - 1;
    if (my < 0) my = 0;
    if (my > (TS_ROWS + 1) * 16 - 1) my = (TS_ROWS + 1) * 16 - 1;
    uint32_t p = (pvis ? 1u << 31 : 0) | (uint32_t)my << 16 | (uint32_t)mx;
    if (p != pointer_shadow) { pointer_shadow = p; IO_POINTER = p; }
    int r = my / 16, c = mx / 8, press = b & ~mbtn;
    struct tab *t = T;
    int track = t->on && !t->ui && !t->sb && t->ts.mouse;
    static const int xb[3] = { 1, 3, 2 };    /* bit 0 left, 1 right, 2 middle -> xterm 1 left, 2 middle, 3 right */
    if (grab || (track && r >= 1)) {
        static int lr = -1, lc = -1;
        int rr = r < 1 ? 0 : r - 1;
        if (track && (rr != lr || c != lc)) { lr = rr; lc = c; vterm_mouse_move(t->vt, rr, c, VTERM_MOD_NONE); }
        for (int i = 0; i < 3; i++)
            if (((b ^ mbtn) >> i) & 1) vterm_mouse_button(t->vt, xb[i], (b >> i) & 1, VTERM_MOD_NONE);
        grab = b != 0 && track;
        if (r >= 1 && track) for (; wh; wh += wh > 0 ? -1 : 1) vterm_mouse_button(t->vt, wh > 0 ? 4 : 5, 1, VTERM_MOD_NONE);
    } else if (r == 0) {
        if (press & 1) bar_click(c);
        if (wh) step_tab(wh > 0 ? -1 : 1);            /* the wheel on the tab bar: the tabs */
        wh = 0;
    }
    for (; wh; wh += wh > 0 ? -1 : 1) {                 /* the wheel, the application not tracking it */
        int up = wh > 0;
        if (t->ui) { struct conn cc; ui_key(up ? VTERM_KEY_UP : VTERM_KEY_DOWN, 0, VTERM_MOD_NONE, &cc); }
        else if (!t->ts.altscreen) sb_scroll(t, up ? 3 : -3);
        else if (t->on) for (int k = 0; k < 3; k++) vterm_keyboard_key(t->vt, up ? VTERM_KEY_UP : VTERM_KEY_DOWN, VTERM_MOD_NONE);
    }
    mbtn = b;
}

/* ------------------------------------------------------------------ the RTL: row table, cursor */
static void push_screen(void) {
    struct tab *t = T;
    tscreen *s = t->ui ? &ui_ts : &t->ts;
    int sb = t->ui ? 0 : t->sb;
    if (t->sb && (t->ts.altscreen || t->ui)) t->sb = sb = 0;
    if (sb && t->ts.top != t->sb_top) { sb_scroll(t, (int)(t->ts.top - t->sb_top)); sb = t->sb; }
    if (bar_dirty) { bar_dirty = 0; draw_bar(); }
    uint16_t e0 = (uint16_t)((uintptr_t)bar >> 10);
    if (e0 != rowtab_shadow[0]) { rowtab_shadow[0] = e0; IO_ROWTAB(0) = e0; }
    for (int y = 0; y < TS_ROWS; y++) {
        uint16_t p = y < sb ? (uint16_t)((s->row[0] + TS_RING * 2 - sb + y) % TS_RING) : s->row[y - sb];
        uint16_t e = p >= TS_RING ? (uint16_t)(((uintptr_t)s->alt >> 10) + (p - TS_RING)) : (uint16_t)(((uintptr_t)s->ring >> 10) + p);
        if (e != rowtab_shadow[y + 1]) { rowtab_shadow[y + 1] = e; IO_ROWTAB(y + 1) = e; }
    }
    uint32_t shape = s->cursor_shape == VTERM_PROP_CURSORSHAPE_UNDERLINE ? 1 :
                     s->cursor_shape == VTERM_PROP_CURSORSHAPE_BAR_LEFT ? 2 : 0;
    uint32_t c = (uint32_t)(s->cursor.col & 0x7f) | (uint32_t)((s->cursor.row + 1) & 0x3f) << 8 |
                 (s->cursor_visible && !sb && confirm < 0 ? 1u << 15 : 0) | shape << 16 | (s->cursor_blink ? 1u << 24 : 0);
    if (c != cursor_shadow) { cursor_shadow = c; IO_CURSOR = c; }
}

/* FNV-1a of the visible cells of the tab shown (as bench_state.c): printed on the debug console when SW bit 31
   rises (the sims set it; the oracle is bench_vterm on the host) */
static void print_hash(void) {
    uint32_t h = 2166136261u;
    tscreen *s = T->ui ? &ui_ts : &T->ts;
    for (int y = 0; y < TS_ROWS; y++) {
        const uint8_t *p = (const uint8_t *)tscreen_cell(s, y, 0);
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

/* the module and the TCP links of the tabs -> the tabs */
static void poll_net(void) {
    static int was_ready;
    static uint32_t ip_t;
    static char ip_seen[20];
    if (strcmp(ip_seen, esp_ip())) { snprintf(ip_seen, sizeof ip_seen, "%s", esp_ip()); bar_dirty = 1; }
    if (esp_ready() && !was_ready) {
        was_ready = 1;
        for (int i = 0; i < NTAB; i++)
            if (tabs[i].used && !tabs[i].on) tab_status(&tabs[i], esp_ip()[0] ? "Ready" : "Ready: Wi-Fi has no IP (Setup Wi-Fi in the boot core)");
        /* the sims set SW bits (0 on the board: the core has two switch bytes):
           30 (run_f2.py) telnet to 127.0.0.1:2323; 29 (run_f3.py) SSH to 127.0.0.1:2224 as "test";
           28 (run_f5.py) telnet to 127.0.0.1:2323 in tab 1 and to 127.0.0.1:2325 in a new tab 2 */
        uint32_t sw = IO_SW;
        if (sw >> 30 & 1) { static const struct conn c = { P_TELNET, "127.0.0.1", 2323, 0, "" }; start(T, &c); }
        if (sw >> 29 & 1) { static const struct conn c = { P_SSH, "127.0.0.1", 2224, 0, "test" }; start(T, &c); }
        if (sw >> 28 & 1) {
            static const struct conn a = { P_TELNET, "127.0.0.1", 2323, 0, "" }, b = { P_TELNET, "127.0.0.1", 2325, 0, "" };
            start(T, &a);
            new_tab();
            start(T, &b);
        }
    }
    if (esp_ready() && !esp_ip()[0] && (int32_t)(IO_MS - ip_t) > 5000) { ip_t = IO_MS; esp_refresh_ip(); }
    for (int i = 0; i < NTAB; i++) {
        struct tab *t = &tabs[i];
        if (!t->used) continue;
        int s = esp_state(i);
        if (s == t->last_es) continue;
        int prev = t->last_es;
        t->last_es = s;
        bar_dirty = 1;
        if (t->c.proto == P_USB) continue;
        if (s == ESP_CONNECTED) {
            t->on = 1;
            if (t->c.proto != P_SSH) ui_remember(&t->c);   /* into the phonebook: only a connection that worked (SSH: logged in) */
            if (t == T) ui_session(&t->c, 1);
            tab_status(t, "Connected");
            static const char clr[] = "\x1b[0m\x1b[2J\x1b[H";         /* a new session: a clean screen */
            vterm_input_write(t->vt, clr, sizeof clr - 1);
            show_session(t);
            if (t->c.proto == P_SSH) ssh_start(t->ssh, &sio, t, t->c.host, t->c.port, t->c.user, TS_COLS, TS_ROWS);
        } else if (s == ESP_IDLE && (prev == ESP_CONNECTED || prev == ESP_CONNECTING || (prev == ESP_CLOSING && t->on))) {
            /* CLOSING with the session still on: we closed it for ssh.c (the shell exited); F8 clears on first */
            int was = t->on;
            t->on = 0;
            t->line_on = 0;
            if (t == T) ui_session(&t->c, 0);
            static const char closed[] = "\r\n\x1b[0;7m[connection closed]\x1b[0m\r\n";
            if (was && t->c.proto == P_SSH && t->ssh_end[0]) { char e[160]; snprintf(e, sizeof e, "\r\n\x1b[0;7m[ssh: %s]\x1b[0m", t->ssh_end); say(t, e); }
            if (was) say(t, closed);
            char m[128];
            snprintf(m, sizeof m, "%s%s", was ? "Disconnected" : "Failed", esp_error(i)[0] ? ": " : "");
            if (t->c.proto == P_SSH && t->ssh_end[0]) snprintf(m, sizeof m, "%s: %s", was ? "Disconnected" : "Failed", t->ssh_end);
            else if (esp_error(i)[0]) strncat(m, esp_error(i), sizeof m - strlen(m) - 1);
            tab_status(t, m);
            show_ui(t);
        }
    }
}

int main(void) {
    STAGE('a');
    {   /* the power-up contents of SRAM nobody loads (under the stack): some randomness after a power-up */
        extern char __heap_end[];
        rng_mix((const void *)((uintptr_t)__heap_end - 8192u), 8192, 0);   /* 64 KB took ~3 frames */
    }
    for (int i = 0; i < NTAB; i++) {
        tabs[i].idx = i;
        tabs[i].vt = vterm_new(TS_ROWS, TS_COLS);
        vterm_set_utf8(tabs[i].vt, 1);
        vterm_output_set_callback(tabs[i].vt, out_cb, &tabs[i]);
    }
    ui_vt = vterm_new(TS_ROWS, TS_COLS);
    vterm_set_utf8(ui_vt, 1);
    tscreen_init(&ui_ts, vterm_obtain_state(ui_vt), ui_cells, ui_cells);
    STAGE('b');
    tab_init(&tabs[0]);
    curt = 0;
    STAGE('c');
    for (int y = 0; y <= TS_ROWS; y++) rowtab_shadow[y] = 0xffff;
    palette();
    IO_POINTER = 0;
    push_screen();
    STAGE('d');
    IO_SHOW = 1;
    static const char hello[] = "\x1b[1mterm\x1b[0m: xterm-256color, 128 x 47\r\n";
    vterm_input_write(T->vt, hello, sizeof hello - 1);
    kbd_init(T->vt);
    kbd_hook = key_hook;
    esp_on_data = on_tcp;
    esp_init();
    store_init();
    book_init();
    ui_init(ui_vt);
    tab_status(T, "Looking for the ESP8266 ...");
    /* SW bit 27 (src/sim/run_f1.sh): the USB serial session in tab 1 at once */
    if (IO_SW >> 27 & 1) { static const struct conn u = { P_USB, "", 0, 0, "" }; start(T, &u); }
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
        /* the USB serial port: read while a tab has its session (shown or not) */
        int u = usb_tab();
        while (u >= 0 && n < (int)sizeof buf && ((r = IO_SER_RX) & 0x100)) buf[n++] = (char)r;
        if (n) to_screen(&tabs[u], (const uint8_t *)buf, (size_t)n);
        esp_poll();
        poll_net();
        store_poll();
        sshkeys_poll();
        { static int was; book_poll(); if (book_ready() != was) { was = book_ready(); ui_redraw(); } }
        { static uint32_t kn; uint32_t k = IO_KBDN; if (k != kn) { kn = k; rng_event(2); } }
        for (int i = 0; i < NTAB; i++) ssh_service(&tabs[i]);
        { static const uint8_t *pk; if (sshkeys_user_pub() != pk) { pk = sshkeys_user_pub(); char fp[52] = ""; if (pk) ssh_fingerprint(pk, fp); ui_keyfp(fp); } }
        kbd_poll(T->vt);
        poll_mouse();
        for (int i = 0; i < NTAB; i++) {
            struct tab *t = &tabs[i];
            if (!t->used || t->ts.bells == t->bells) continue;
            t->bells = t->ts.bells;
            if (t == T && !t->ui) IO_BELL = 1;
            else if (!t->activity) { t->activity = 1; bar_dirty = 1; }
        }
        push_screen();
    }
}

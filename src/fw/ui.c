/* see ui.h */
#include <stdio.h>
#include <string.h>
#include "ui.h"
#include "charset.h"
#include "book.h"

#define RECENT 24                          /* shown of the phonebook */

static int open_;
static struct conn f = { P_TELNET, "", 23, 0 };   /* the fields */
static int focus;                                  /* 0 proto, 1 host, 2 port, 3 charset, 4 connect, 5.. recent */
static char status[128] = "";
static char ip[20] = "";
static struct conn sess;
static int sess_on;

static const char *pname[P_N] = { "USB serial", "telnet", "raw TCP" };
const char *ui_proto_name(int p) { return p >= 0 && p < P_N ? pname[p] : "?"; }

static VTerm *g_vt;
static char out[8192];
static size_t on;

static void put(const char *s) { size_t n = strlen(s); if (on + n < sizeof out) { memcpy(out + on, s, n); on += n; } }
static void putf(const char *fmt, const char *a) { char b[256]; snprintf(b, sizeof b, fmt, a); put(b); }
static void at(int row, int col) { char b[16]; snprintf(b, sizeof b, "\x1b[%d;%dH", row, col); put(b); }

static int nrec(void) { int k = book_count(); return k > RECENT ? RECENT : k; }
static int fields_n(void) { return 5 + nrec(); }

static void field(int idx, int row, const char *label, const char *value, int choice) {
    at(row, 4);
    put("\x1b[0;38;5;250m");
    putf("%-10s", label);
    put(focus == idx ? "\x1b[0;1;38;5;16;48;5;117m" : "\x1b[0;38;5;255;48;5;238m");
    char b[80];
    if (choice) snprintf(b, sizeof b, " \xe2\x97\x80 %-14s \xe2\x96\xb6 ", value);  /* ◀ ▶ */
    else snprintf(b, sizeof b, " %-40s ", value);
    put(b);
    put("\x1b[0m");
}

static void draw(void) {
    if (!open_ || !g_vt) return;
    on = 0;
    put("\x1b[0m\x1b[2J\x1b[?25l");
    /* the title bar */
    at(1, 1);
    put("\x1b[0;1;38;5;255;48;5;24m term \x1b[0;38;5;255;48;5;24m\xe2\x80\x94 Karabas terminal");
    put("\x1b[K");
    char w[64];
    snprintf(w, sizeof w, "Wi-Fi: %s ", ip[0] ? ip : "no IP");
    char b[32];
    snprintf(b, sizeof b, "\x1b[1;%dH", 129 - (int)strlen(w));
    put(b);
    put(w);
    put("\x1b[0m");
    /* the session */
    at(3, 2);
    if (sess_on) {
        char s[128];
        if (sess.proto == P_USB) snprintf(s, sizeof s, "Session: USB serial, %s  (Esc: back to it)", charset_name(sess.cs));
        else snprintf(s, sizeof s, "Session: %s %s:%d, %s  (Esc: back to it)", ui_proto_name(sess.proto), sess.host, sess.port, charset_name(sess.cs));
        put("\x1b[38;5;114m"); put(s); put("\x1b[0m");
    } else put("\x1b[38;5;244mNo session\x1b[0m");
    /* the new connection */
    at(5, 2); put("\x1b[1mNew connection\x1b[0m");
    field(0, 7, "Protocol", ui_proto_name(f.proto), 1);
    if (f.proto != P_USB) {
        field(1, 8, "Host", f.host, 0);
        char p[8]; snprintf(p, sizeof p, "%d", f.port);
        field(2, 9, "Port", p, 0);
    } else {
        at(8, 4); put("\x1b[38;5;244mthe USB serial port of the board (a getty on the PC)\x1b[0m");
    }
    field(3, 10, "Charset", charset_name(f.cs), 1);
    at(12, 14);
    put(focus == 4 ? "\x1b[0;1;38;5;16;48;5;214m" : "\x1b[0;1;38;5;16;48;5;250m");
    put("  Connect  \x1b[0m");
    /* the recent list */
    at(14, 2); put("\x1b[1mPhonebook\x1b[0m");
    put(book_ready() ? "\x1b[38;5;244m  (on the SD card; Del removes)\x1b[0m" : "\x1b[38;5;244m  (not on the SD card yet: kept until the core restarts)\x1b[0m");
    if (!nrec()) { at(15, 4); put("\x1b[38;5;244m(empty: a connection made goes here)\x1b[0m"); }
    for (int i = 0; i < nrec(); i++) {
        char s[128];
        const struct conn *r = book_get(i);
        if (r->proto == P_USB) snprintf(s, sizeof s, " %-10s %-46s %-10s ", "USB", "USB serial", charset_name(r->cs));
        else { char hp[80]; snprintf(hp, sizeof hp, "%s:%d", r->host, r->port); snprintf(s, sizeof s, " %-10s %-46s %-10s ", ui_proto_name(r->proto), hp, charset_name(r->cs)); }
        at(15 + i, 4);
        put(focus == 5 + i ? "\x1b[0;38;5;16;48;5;117m" : "\x1b[0m");
        put(s);
        put("\x1b[0m");
    }
    /* help and status */
    at(46, 2);
    put("\x1b[38;5;244m\xe2\x86\x91\xe2\x86\x93 field   \xe2\x86\x90\xe2\x86\x92 choice   Enter connect   Esc back   "
        "Scroll Lock this screen   F8 disconnect\x1b[0m");
    at(48, 1);
    put("\x1b[0;38;5;255;48;5;236m ");
    put(status);
    put("\x1b[K\x1b[0m");
    /* the text cursor in Host / Port */
    if (focus == 1 && f.proto != P_USB) { at(8, 16 + (int)strlen(f.host)); put("\x1b[?25h"); }
    if (focus == 2 && f.proto != P_USB) { char p[8]; snprintf(p, sizeof p, "%d", f.port); at(9, 16 + (int)strlen(p)); put("\x1b[?25h"); }
    vterm_input_write(g_vt, out, on);
}

void ui_open(VTerm *vt) {
    g_vt = vt;
    if (open_) return;
    open_ = 1;
    vterm_input_write(vt, "\x1b[?1049h", 8);
    if (f.proto != P_USB && !f.host[0]) focus = 1;
    draw();
}

void ui_close(VTerm *vt) {
    if (!open_) return;
    open_ = 0;
    vterm_input_write(vt, "\x1b[0m\x1b[?1049l\x1b[?25h", 18);
}

int ui_active(void) { return open_; }
void ui_status(VTerm *vt, const char *s) { (void)vt; snprintf(status, sizeof status, "%s", s); draw(); }
void ui_wifi(VTerm *vt, const char *s) { (void)vt; if (strcmp(ip, s)) { snprintf(ip, sizeof ip, "%s", s); draw(); } }
void ui_session(const struct conn *c, int on_) { if (c) sess = *c; sess_on = on_; draw(); }

void ui_remember(const struct conn *c) { if (c->proto != P_USB) book_put(c); }
void ui_redraw(void) { draw(); }

enum ui_action ui_key(VTerm *vt, VTermKey key, uint32_t ch, VTermModifier mod, struct conn *c) {
    (void)mod;
    g_vt = vt;
    int n = fields_n();
    switch (key) {
    case VTERM_KEY_UP: do focus = (focus + n - 1) % n; while (f.proto == P_USB && (focus == 1 || focus == 2)); break;
    case VTERM_KEY_DOWN: case VTERM_KEY_TAB: do focus = (focus + 1) % n; while (f.proto == P_USB && (focus == 1 || focus == 2)); break;
    case VTERM_KEY_LEFT: case VTERM_KEY_RIGHT: {
        int d = key == VTERM_KEY_LEFT ? -1 : 1;
        if (focus == 0) { f.proto = (f.proto + P_N + d) % P_N; if (f.proto == P_TELNET && f.port == 0) f.port = 23; }
        if (focus == 3) f.cs = (f.cs + charset_count() + d) % charset_count();
        break;
    }
    case VTERM_KEY_BACKSPACE:
        if (focus == 1 && f.host[0]) f.host[strlen(f.host) - 1] = 0;
        if (focus == 2) f.port /= 10;
        break;
    case VTERM_KEY_ESCAPE: return UI_BACK;
    case VTERM_KEY_DEL:
        if (focus >= 5) { book_del(focus - 5); if (focus >= fields_n()) focus = fields_n() - 1; }
        break;
    case VTERM_KEY_ENTER:
        if (focus >= 5) { f = *book_get(focus - 5); }
        if (f.proto != P_USB && (!f.host[0] || f.port <= 0)) { ui_status(vt, "Host and port, please"); focus = !f.host[0] ? 1 : 2; return UI_NONE; }
        *c = f;
        return UI_CONNECT;
    case VTERM_KEY_NONE:
        if (focus == 1 && ch > ' ' && ch < 0x7f) { size_t l = strlen(f.host); if (l < sizeof f.host - 1) { f.host[l] = (char)ch; f.host[l + 1] = 0; } }
        else if (focus == 2 && ch >= '0' && ch <= '9') { if (f.port < 6554) f.port = f.port * 10 + (int)(ch - '0'); }
        break;
    default: break;
    }
    draw();
    return UI_NONE;
}

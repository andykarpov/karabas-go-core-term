/* see ui.h */
#include <stdio.h>
#include <string.h>
#include "ui.h"
#include "charset.h"
#include "book.h"

#define RECENT 24                          /* shown of the phonebook */

/* the fields */
enum { F_PROTO, F_HOST, F_PORT, F_USER, F_CS, F_CONNECT, F_RECENT };

static int open_;
static struct conn f = { P_TELNET, "", 23, 0, "" };
static int focus;                                  /* F_*, F_RECENT + i: an entry of the phonebook */
static char status[128] = "";
static char ip[20] = "";
static struct conn sess;
static int sess_on;
static char keyfp[64] = "";                        /* the fingerprint of the SSH key of the user */

static const char *pname[P_N] = { "USB serial", "telnet", "raw TCP", "SSH" };
const char *ui_proto_name(int p) { return p >= 0 && p < P_N ? pname[p] : "?"; }
static int default_port(int p) { return p == P_TELNET ? 23 : p == P_SSH ? 22 : 0; }

static VTerm *g_vt;
static char out[8192];
static size_t on;

static void put(const char *s) { size_t n = strlen(s); if (on + n < sizeof out) { memcpy(out + on, s, n); on += n; } }
static void putf(const char *fmt, const char *a) { char b[256]; snprintf(b, sizeof b, fmt, a); put(b); }
static void at(int row, int col) { char b[16]; snprintf(b, sizeof b, "\x1b[%d;%dH", row, col); put(b); }

static int nrec(void) { int k = book_count(); return k > RECENT ? RECENT : k; }
static int fields_n(void) { return F_RECENT + nrec(); }
/* a field not shown for this protocol */
static int hidden(int i) { return (f.proto == P_USB && (i == F_HOST || i == F_PORT)) || (f.proto != P_SSH && i == F_USER); }

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

static void conn_text(const struct conn *c, char *s, size_t n) {
    if (c->proto == P_USB) snprintf(s, n, "USB serial");
    else if (c->proto == P_SSH && c->user[0]) snprintf(s, n, "%s@%s:%d", c->user, c->host, c->port);
    else snprintf(s, n, "%s:%d", c->host, c->port);
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
        char s[160], t[100];
        conn_text(&sess, t, sizeof t);
        snprintf(s, sizeof s, "Session: %s %s, %s  (Esc: back to it)", ui_proto_name(sess.proto), sess.proto == P_USB ? "" : t, charset_name(sess.cs));
        put("\x1b[38;5;114m"); put(s); put("\x1b[0m");
    } else put("\x1b[38;5;244mNo session\x1b[0m");
    /* the new connection */
    at(5, 2); put("\x1b[1mNew connection\x1b[0m");
    field(F_PROTO, 7, "Protocol", ui_proto_name(f.proto), 1);
    if (f.proto != P_USB) {
        field(F_HOST, 8, "Host", f.host, 0);
        char p[8]; snprintf(p, sizeof p, "%d", f.port);
        field(F_PORT, 9, "Port", p, 0);
    } else {
        at(8, 4); put("\x1b[38;5;244mthe USB serial port of the board (a getty on the PC)\x1b[0m");
    }
    if (f.proto == P_SSH) {
        field(F_USER, 10, "User", f.user, 0);
        if (!f.user[0]) { at(10, 58); put("\x1b[38;5;244mempty: asked at the login\x1b[0m"); }
    }
    field(F_CS, 11, "Charset", charset_name(f.cs), 1);
    at(13, 14);
    put(focus == F_CONNECT ? "\x1b[0;1;38;5;16;48;5;214m" : "\x1b[0;1;38;5;16;48;5;250m");
    put("  Connect  \x1b[0m");
    /* the recent list */
    at(15, 2); put("\x1b[1mPhonebook\x1b[0m");
    put(book_ready() ? "\x1b[38;5;244m  (on the SD card; Del removes)\x1b[0m" : "\x1b[38;5;244m  (not on the SD card yet: kept until the core restarts)\x1b[0m");
    if (!nrec()) { at(16, 4); put("\x1b[38;5;244m(empty: a connection made goes here)\x1b[0m"); }
    for (int i = 0; i < nrec(); i++) {
        char s[128], t[100];
        const struct conn *r = book_get(i);
        conn_text(r, t, sizeof t);
        snprintf(s, sizeof s, " %-10s %-46s %-10s ", r->proto == P_USB ? "USB" : ui_proto_name(r->proto), t, charset_name(r->cs));
        at(16 + i, 4);
        put(focus == F_RECENT + i ? "\x1b[0;38;5;16;48;5;117m" : "\x1b[0m");
        put(s);
        put("\x1b[0m");
    }
    /* the SSH key of the user */
    at(44, 2);
    if (keyfp[0]) { put("\x1b[38;5;250mYour SSH key: ED25519 "); put(keyfp); put("   \x1b[38;5;244mF2 types the public key into the session (for authorized_keys)\x1b[0m"); }
    else put("\x1b[38;5;244mYour SSH key: made at the first SSH connection\x1b[0m");
    /* help and status */
    at(46, 2);
    put("\x1b[38;5;244m\xe2\x86\x91\xe2\x86\x93 field   \xe2\x86\x90\xe2\x86\x92 choice   Enter connect   Esc back   "
        "Scroll Lock this screen   F8 disconnect\x1b[0m");
    at(48, 1);
    put("\x1b[0;38;5;255;48;5;236m ");
    put(status);
    put("\x1b[K\x1b[0m");
    /* the text cursor in Host / Port / User */
    if (focus == F_HOST && !hidden(F_HOST)) { at(8, 16 + (int)strlen(f.host)); put("\x1b[?25h"); }
    if (focus == F_PORT && !hidden(F_PORT)) { char p[8]; snprintf(p, sizeof p, "%d", f.port); at(9, 16 + (int)strlen(p)); put("\x1b[?25h"); }
    if (focus == F_USER && !hidden(F_USER)) { at(10, 16 + (int)strlen(f.user)); put("\x1b[?25h"); }
    vterm_input_write(g_vt, out, on);
}

void ui_open(VTerm *vt) {
    g_vt = vt;
    if (open_) return;
    open_ = 1;
    vterm_input_write(vt, "\x1b[?1049h", 8);
    if (f.proto != P_USB && !f.host[0]) focus = F_HOST;
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
void ui_keyfp(const char *fp) { if (strcmp(keyfp, fp)) { snprintf(keyfp, sizeof keyfp, "%s", fp); draw(); } }

void ui_remember(const struct conn *c) { if (c->proto != P_USB) book_put(c); }
void ui_redraw(void) { draw(); }

static void type_into(char *s, size_t size, uint32_t ch) {
    size_t l = strlen(s);
    if (ch > ' ' && ch < 0x7f && l < size - 1) { s[l] = (char)ch; s[l + 1] = 0; }
}

enum ui_action ui_key(VTerm *vt, VTermKey key, uint32_t ch, VTermModifier mod, struct conn *c) {
    (void)mod;
    g_vt = vt;
    int n = fields_n();
    switch (key) {
    case VTERM_KEY_UP: do focus = (focus + n - 1) % n; while (hidden(focus)); break;
    case VTERM_KEY_DOWN: case VTERM_KEY_TAB: do focus = (focus + 1) % n; while (hidden(focus)); break;
    case VTERM_KEY_LEFT: case VTERM_KEY_RIGHT: {
        int d = key == VTERM_KEY_LEFT ? -1 : 1;
        if (focus == F_PROTO) {
            int was = f.proto;
            f.proto = (f.proto + P_N + d) % P_N;
            if (f.port == default_port(was) && default_port(f.proto)) f.port = default_port(f.proto);
        }
        if (focus == F_CS) f.cs = (f.cs + charset_count() + d) % charset_count();
        break;
    }
    case VTERM_KEY_BACKSPACE:
        if (focus == F_HOST && f.host[0]) f.host[strlen(f.host) - 1] = 0;
        if (focus == F_USER && f.user[0]) f.user[strlen(f.user) - 1] = 0;
        if (focus == F_PORT) f.port /= 10;
        break;
    case VTERM_KEY_ESCAPE: return UI_BACK;
    case VTERM_KEY_DEL:
        if (focus >= F_RECENT) { book_del(focus - F_RECENT); if (focus >= fields_n()) focus = fields_n() - 1; }
        break;
    case VTERM_KEY_ENTER:
        if (focus >= F_RECENT) { f = *book_get(focus - F_RECENT); }
        if (f.proto != P_USB && (!f.host[0] || f.port <= 0)) { ui_status(vt, "Host and port, please"); focus = !f.host[0] ? F_HOST : F_PORT; return UI_NONE; }
        *c = f;
        return UI_CONNECT;
    case VTERM_KEY_NONE:
        if (focus == F_HOST) type_into(f.host, sizeof f.host, ch);
        else if (focus == F_USER) type_into(f.user, sizeof f.user, ch);
        else if (focus == F_PORT && ch >= '0' && ch <= '9') { if (f.port < 6554) f.port = f.port * 10 + (int)(ch - '0'); }
        break;
    default:
        if (key == VTERM_KEY_FUNCTION(2)) return UI_TYPE_KEY;
        break;
    }
    draw();
    return UI_NONE;
}

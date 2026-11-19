/* see ui.h */
#include <stdio.h>
#include <string.h>
#include "ui.h"
#include "charset.h"
#include "book.h"

#define RECENT 24                          /* shown of the phonebook */

/* the fields */
enum { F_PROTO, F_HOST, F_PORT, F_USER, F_CS, F_CONNECT, F_RECENT };

static struct conn f = { P_TELNET, "", 23, 0, "" };
static int focus;                                  /* F_*, F_RECENT + i: an entry of the phonebook */
static char status[128] = "";
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

void ui_conn_text(const struct conn *c, char *s, size_t n) {
    if (c->proto == P_USB) snprintf(s, n, "USB serial");
    else if (c->proto == P_SSH && c->user[0]) snprintf(s, n, "%s@%s:%d", c->user, c->host, c->port);
    else snprintf(s, n, "%s:%d", c->host, c->port);
}

/* 47 rows under the tab bar (docs/PLAN.md Ф5); Wi-Fi is in the tab bar */
static void draw(void) {
    if (!g_vt) return;
    on = 0;
    put("\x1b[0m\x1b[2J\x1b[?25l");
    /* the session of this tab */
    at(2, 2);
    if (sess_on) {
        char s[160], t[100];
        ui_conn_text(&sess, t, sizeof t);
        snprintf(s, sizeof s, "Session: %s %s, %s  (Esc: back to it)", ui_proto_name(sess.proto), sess.proto == P_USB ? "" : t, charset_name(sess.cs));
        put("\x1b[38;5;114m"); put(s); put("\x1b[0m");
    } else put("\x1b[38;5;244mNo session\x1b[0m");
    /* the new connection */
    at(4, 2); put("\x1b[1mNew connection\x1b[0m");
    field(F_PROTO, 6, "Protocol", ui_proto_name(f.proto), 1);
    if (f.proto != P_USB) {
        field(F_HOST, 7, "Host", f.host, 0);
        char p[8]; snprintf(p, sizeof p, "%d", f.port);
        field(F_PORT, 8, "Port", p, 0);
    } else {
        at(7, 4); put("\x1b[38;5;244mthe USB serial port of the board (a getty on the PC)\x1b[0m");
    }
    if (f.proto == P_SSH) {
        field(F_USER, 9, "User", f.user, 0);
        if (!f.user[0]) { at(9, 58); put("\x1b[38;5;244mempty: asked at the login\x1b[0m"); }
    }
    field(F_CS, 10, "Charset", charset_name(f.cs), 1);
    at(12, 14);
    put(focus == F_CONNECT ? "\x1b[0;1;38;5;16;48;5;214m" : "\x1b[0;1;38;5;16;48;5;250m");
    put("  Connect  \x1b[0m");
    /* the recent list */
    at(14, 2); put("\x1b[1mPhonebook\x1b[0m");
    put(book_ready() ? "\x1b[38;5;244m  (on the SD card; Del removes)\x1b[0m" : "\x1b[38;5;244m  (not on the SD card yet: kept until the core restarts)\x1b[0m");
    if (!nrec()) { at(15, 4); put("\x1b[38;5;244m(empty: a connection made goes here)\x1b[0m"); }
    for (int i = 0; i < nrec(); i++) {
        char s[128], t[100];
        const struct conn *r = book_get(i);
        ui_conn_text(r, t, sizeof t);
        snprintf(s, sizeof s, " %-10s %-46s %-10s ", r->proto == P_USB ? "USB" : ui_proto_name(r->proto), t, charset_name(r->cs));
        at(15 + i, 4);
        put(focus == F_RECENT + i ? "\x1b[0;38;5;16;48;5;117m" : "\x1b[0m");
        put(s);
        put("\x1b[0m");
    }
    /* the SSH key of the user */
    at(42, 2);
    if (keyfp[0]) { put("\x1b[38;5;250mYour SSH key: "); put(keyfp); put(" (ED25519)   \x1b[38;5;244mF2: type it into the session\x1b[0m"); }
    else put("\x1b[38;5;244mYour SSH key: made at the first SSH connection\x1b[0m");
    /* help and status */
    at(44, 2);
    put("\x1b[38;5;244m\xe2\x86\x91\xe2\x86\x93 field   \xe2\x86\x90\xe2\x86\x92 choice   Enter connect   Esc back   "
        "Scroll Lock this screen   F8 disconnect\x1b[0m");
    at(45, 2);
    put("\x1b[38;5;244mCtrl+Shift+T new tab   Ctrl+Shift+W close tab   Ctrl+PgUp / PgDn, Ctrl+Shift+1..4 tabs   "
        "Shift+PgUp / PgDn history\x1b[0m");
    at(47, 1);
    put("\x1b[0;38;5;255;48;5;236m ");
    put(status);
    put("\x1b[K\x1b[0m");
    /* the text cursor in Host / Port / User */
    if (focus == F_HOST && !hidden(F_HOST)) { at(7, 16 + (int)strlen(f.host)); put("\x1b[?25h"); }
    if (focus == F_PORT && !hidden(F_PORT)) { char p[8]; snprintf(p, sizeof p, "%d", f.port); at(8, 16 + (int)strlen(p)); put("\x1b[?25h"); }
    if (focus == F_USER && !hidden(F_USER)) { at(9, 16 + (int)strlen(f.user)); put("\x1b[?25h"); }
    vterm_input_write(g_vt, out, on);
}

void ui_init(VTerm *vt) {
    g_vt = vt;
    vterm_input_write(vt, "\x1b[?1049h", 8);      /* its own terminal: the alternate screen, never scrolls */
    if (f.proto != P_USB && !f.host[0]) focus = F_HOST;
    draw();
}

void ui_status(const char *s) { if (strcmp(status, s)) { snprintf(status, sizeof status, "%s", s); draw(); } }
void ui_session(const struct conn *c, int on_) { if (c) sess = *c; sess_on = on_; draw(); }
void ui_keyfp(const char *fp) { if (strcmp(keyfp, fp)) { snprintf(keyfp, sizeof keyfp, "%s", fp); draw(); } }

void ui_remember(const struct conn *c) { if (c->proto != P_USB) book_put(c); }
void ui_redraw(void) { draw(); }

static void type_into(char *s, size_t size, uint32_t ch) {
    size_t l = strlen(s);
    if (ch > ' ' && ch < 0x7f && l < size - 1) { s[l] = (char)ch; s[l + 1] = 0; }
}

enum ui_action ui_key(VTermKey key, uint32_t ch, VTermModifier mod, struct conn *c) {
    (void)mod;
    int n = fields_n();
    switch (key) {
    case VTERM_KEY_UP: do focus = (focus + n - 1) % n; while (hidden(focus)); break;
    case VTERM_KEY_DOWN: case VTERM_KEY_TAB: do focus = (focus + 1) % n; while (hidden(focus)); break;
    case VTERM_KEY_LEFT: case VTERM_KEY_RIGHT: {
        int d = key == VTERM_KEY_LEFT ? -1 : 1;
        if (focus == F_PROTO) {
            f.proto = (f.proto + P_N + d) % P_N;
            /* a port the user did not type (0, 22, 23) follows the protocol */
            if ((f.port == 0 || f.port == 22 || f.port == 23) && default_port(f.proto)) f.port = default_port(f.proto);
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
        if (f.proto != P_USB && (!f.host[0] || f.port <= 0)) { ui_status("Host and port, please"); focus = !f.host[0] ? F_HOST : F_PORT; return UI_NONE; }
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

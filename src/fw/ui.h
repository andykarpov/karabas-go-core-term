/* the connection screen of the term core (Ф2, Ф5): its own libvterm (the sessions of the tabs go on under it),
   shown for a tab. Scroll Lock opens it (and goes back), Esc goes back to the session, F8 disconnects,
   F2 types the SSH public key of the user into the session. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "vterm.h"

enum proto { P_USB, P_TELNET, P_RAW, P_SSH, P_N };      /* the numbers are in the phonebook */

struct conn {
    int proto;
    char host[64];
    int port;
    int cs;                 /* charset.h */
    char user[32];          /* SSH: "" = asked at the login */
};

enum ui_action { UI_NONE, UI_CONNECT, UI_BACK, UI_TYPE_KEY };   /* UI_TYPE_KEY: F2, the SSH public key into the session */

void ui_init(VTerm *vt);                         /* the own terminal of the screen (TS_ROWS rows) */
/* a key of the user while the screen is shown; UI_CONNECT: *c is the connection to make */
enum ui_action ui_key(VTermKey key, uint32_t ch, VTermModifier mod, struct conn *c);
void ui_status(const char *s);                   /* the status line */
void ui_session(const struct conn *c, int on);   /* the session of the tab shown (at the top) */
void ui_remember(const struct conn *c);          /* into the phonebook (book.c) */
void ui_redraw(void);
void ui_keyfp(const char *fp);                   /* the fingerprint of the SSH key of the user ("" none) */
void ui_conn_text(const struct conn *c, char *s, size_t n);   /* "user@host:port" */
const char *ui_proto_name(int p);

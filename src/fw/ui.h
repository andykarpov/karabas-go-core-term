/* the connection screen of the term core (Ф2): drawn into libvterm on its alternate screen (the session
   screen stays under it). Win+Esc opens it, Esc goes back to the session. */
#pragma once
#include <stdint.h>
#include "vterm.h"

enum proto { P_USB, P_TELNET, P_RAW, P_N };

struct conn {
    int proto;
    char host[64];
    int port;
    int cs;                 /* charset.h */
};

enum ui_action { UI_NONE, UI_CONNECT, UI_BACK };

void ui_open(VTerm *vt);
void ui_close(VTerm *vt);
int ui_active(void);
/* a key of the user while the screen is open; UI_CONNECT: *c is the connection to make */
enum ui_action ui_key(VTerm *vt, VTermKey key, uint32_t ch, VTermModifier mod, struct conn *c);
void ui_status(VTerm *vt, const char *s);        /* the status line (redraws when open) */
void ui_wifi(VTerm *vt, const char *ip);          /* "" = no IP */
void ui_session(const struct conn *c, int on);    /* the active session (shown at the top) */
void ui_remember(const struct conn *c);           /* into the recent list */
const char *ui_proto_name(int p);

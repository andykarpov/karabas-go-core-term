/* see telnet.h */
#include <string.h>
#include "telnet.h"

enum { IAC = 255, DONT = 254, DO = 253, WONT = 252, WILL = 251, SB = 250, SE = 240 };
enum { O_BINARY = 0, O_ECHO = 1, O_SGA = 3, O_TTYPE = 24, O_NAWS = 31 };


static void tx(struct telnet *t, const uint8_t *p, size_t n) { t->send(t->ctx, p, n); }
static void send3(struct telnet *t, uint8_t a, uint8_t b) { uint8_t p[3] = { IAC, a, b }; tx(t, p, 3); }

static void naws(struct telnet *t) {
    uint8_t p[9] = { IAC, SB, O_NAWS, (uint8_t)(t->cols >> 8), (uint8_t)t->cols, (uint8_t)(t->rows >> 8), (uint8_t)t->rows, IAC, SE };
    tx(t, p, 9);
}

void telnet_reset(struct telnet *t, int cols, int rows, size_t (*send)(void *, const uint8_t *, size_t), void *ctx) {
    memset(t, 0, sizeof *t);
    t->send = send; t->ctx = ctx; t->cols = cols; t->rows = rows;
}

static void negotiate(struct telnet *t, uint8_t v, uint8_t o) {
    int want_us = o == O_TTYPE || o == O_NAWS || o == O_BINARY || o == O_SGA;
    int want_them = o == O_ECHO || o == O_SGA || o == O_BINARY;
    switch (v) {
    case DO:
        if (want_us) { if (!t->us[o]) { t->us[o] = 1; send3(t, WILL, o); } if (o == O_NAWS) naws(t); }
        else send3(t, WONT, o);
        break;
    case DONT: if (t->us[o]) { t->us[o] = 0; send3(t, WONT, o); } break;
    case WILL:
        if (want_them) { if (!t->them[o]) { t->them[o] = 1; send3(t, DO, o); } }
        else send3(t, DONT, o);
        break;
    case WONT: if (t->them[o]) { t->them[o] = 0; send3(t, DONT, o); } break;
    }
}

static void subneg(struct telnet *t) {
    if (t->sb_n >= 2 && t->sb[0] == O_TTYPE && t->sb[1] == 1) {          /* SEND */
        static const char tt[] = "XTERM-256COLOR";
        uint8_t p[6 + sizeof tt];
        size_t k = 0;
        p[k++] = IAC; p[k++] = SB; p[k++] = O_TTYPE; p[k++] = 0;  /* IS */
        memcpy(p + k, tt, sizeof tt - 1); k += sizeof tt - 1;
        p[k++] = IAC; p[k++] = SE;
        tx(t, p, k);
    }
}

size_t telnet_in(struct telnet *t, const uint8_t *in, size_t n, uint8_t *out) {
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = in[i];
        switch (t->st) {
        case 0: if (c == IAC) t->st = 1; else out[k++] = c; break;
        case 1:
            if (c == IAC) { out[k++] = IAC; t->st = 0; }
            else if (c >= WILL) { t->verb = c; t->st = 2; }
            else if (c == SB) { t->sb_n = 0; t->st = 3; }
            else t->st = 0;                                         /* NOP, GA, ... */
            break;
        case 2: negotiate(t, t->verb, c); t->st = 0; break;
        case 3: if (c == IAC) t->st = 4; else if (t->sb_n < sizeof t->sb) t->sb[t->sb_n++] = c; break;
        case 4:
            if (c == SE) { subneg(t); t->st = 0; }
            else { if (t->sb_n < sizeof t->sb) t->sb[t->sb_n++] = c; t->st = 3; }
            break;
        }
    }
    return k;
}

void telnet_out(struct telnet *t, const uint8_t *p, size_t n) {
    uint8_t b[512];
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (k > sizeof b - 4) { tx(t, b, k); k = 0; }
        b[k++] = p[i];
        if (p[i] == IAC) b[k++] = IAC;
        else if (p[i] == '\r' && !t->us[O_BINARY]) b[k++] = 0;
    }
    if (k) tx(t, b, k);
}

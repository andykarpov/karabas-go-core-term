/* see telnet.h */
#include <string.h>
#include "telnet.h"

enum { IAC = 255, DONT = 254, DO = 253, WONT = 252, WILL = 251, SB = 250, SE = 240 };
enum { O_BINARY = 0, O_ECHO = 1, O_SGA = 3, O_TTYPE = 24, O_NAWS = 31 };

static size_t (*tx)(const uint8_t *, size_t);
static int cols_, rows_;
static uint8_t us[256], them[256];    /* the options on: us = we WILL, them = they WILL */
static int st;                        /* 0 data, 1 IAC, 2 verb, 3 SB, 4 SB IAC */
static uint8_t verb;
static uint8_t sb[64];
static size_t sb_n;

static void send3(uint8_t a, uint8_t b) { uint8_t p[3] = { IAC, a, b }; tx(p, 3); }

static void naws(void) {
    uint8_t p[9] = { IAC, SB, O_NAWS, (uint8_t)(cols_ >> 8), (uint8_t)cols_, (uint8_t)(rows_ >> 8), (uint8_t)rows_, IAC, SE };
    tx(p, 9);
}

void telnet_reset(int cols, int rows, size_t (*send)(const uint8_t *, size_t)) {
    tx = send; cols_ = cols; rows_ = rows;
    memset(us, 0, sizeof us); memset(them, 0, sizeof them);
    st = 0; sb_n = 0;
}

static void negotiate(uint8_t v, uint8_t o) {
    int want_us = o == O_TTYPE || o == O_NAWS || o == O_BINARY || o == O_SGA;
    int want_them = o == O_ECHO || o == O_SGA || o == O_BINARY;
    switch (v) {
    case DO:
        if (want_us) { if (!us[o]) { us[o] = 1; send3(WILL, o); } if (o == O_NAWS) naws(); }
        else send3(WONT, o);
        break;
    case DONT: if (us[o]) { us[o] = 0; send3(WONT, o); } break;
    case WILL:
        if (want_them) { if (!them[o]) { them[o] = 1; send3(DO, o); } }
        else send3(DONT, o);
        break;
    case WONT: if (them[o]) { them[o] = 0; send3(DONT, o); } break;
    }
}

static void subneg(void) {
    if (sb_n >= 2 && sb[0] == O_TTYPE && sb[1] == 1) {          /* SEND */
        static const char t[] = "XTERM-256COLOR";
        uint8_t p[6 + sizeof t];
        size_t k = 0;
        p[k++] = IAC; p[k++] = SB; p[k++] = O_TTYPE; p[k++] = 0;  /* IS */
        memcpy(p + k, t, sizeof t - 1); k += sizeof t - 1;
        p[k++] = IAC; p[k++] = SE;
        tx(p, k);
    }
}

size_t telnet_in(const uint8_t *in, size_t n, uint8_t *out) {
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = in[i];
        switch (st) {
        case 0: if (c == IAC) st = 1; else out[k++] = c; break;
        case 1:
            if (c == IAC) { out[k++] = IAC; st = 0; }
            else if (c >= WILL) { verb = c; st = 2; }
            else if (c == SB) { sb_n = 0; st = 3; }
            else st = 0;                                         /* NOP, GA, ... */
            break;
        case 2: negotiate(verb, c); st = 0; break;
        case 3: if (c == IAC) st = 4; else if (sb_n < sizeof sb) sb[sb_n++] = c; break;
        case 4:
            if (c == SE) { subneg(); st = 0; }
            else { if (sb_n < sizeof sb) sb[sb_n++] = c; st = 3; }
            break;
        }
    }
    return k;
}

void telnet_out(const uint8_t *p, size_t n) {
    uint8_t b[512];
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (k > sizeof b - 4) { tx(b, k); k = 0; }
        b[k++] = p[i];
        if (p[i] == IAC) b[k++] = IAC;
        else if (p[i] == '\r' && !us[O_BINARY]) b[k++] = 0;
    }
    if (k) tx(b, k);
}

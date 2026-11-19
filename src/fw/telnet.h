/* telnet (RFC 854) on a TCP connection: IAC stripped and answered; options ECHO, SGA, BINARY (both ways),
   TTYPE ("XTERM-256COLOR"), NAWS (the screen size); others refused. */
#pragma once
#include <stdint.h>
#include <stddef.h>

/* one connection (a tab each, docs/PLAN.md Ф5) */
struct telnet {
    size_t (*send)(void *ctx, const uint8_t *, size_t);
    void *ctx;
    int cols, rows;
    uint8_t us[256], them[256];    /* the options on: us = we WILL, them = they WILL */
    int st;                        /* 0 data, 1 IAC, 2 verb, 3 SB, 4 SB IAC */
    uint8_t verb;
    uint8_t sb[64];
    size_t sb_n;
};

void telnet_reset(struct telnet *t, int cols, int rows, size_t (*send)(void *, const uint8_t *, size_t), void *ctx);
/* bytes of the host: the data part into out (n at most); the bytes written */
size_t telnet_in(struct telnet *t, const uint8_t *in, size_t n, uint8_t *out);
/* bytes of the user to the host (IAC doubled, CR as CR NUL unless BINARY) */
void telnet_out(struct telnet *t, const uint8_t *p, size_t n);

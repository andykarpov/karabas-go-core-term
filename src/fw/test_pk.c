/* Ф6 host test of pk.c (src/sim/run_f6_pk.sh): vectors on stdin from src/tools/pk_vectors.py (Python cryptography),
   a line each:
     rsa <n> <e> <sha512 0|1> <msg> <sig> <expected 0|1>
     ec <curve 0..2> <q> <r> <s> <msg> <expected 0|1>
     sha384 <msg> <hash>
   (hex; "-" = empty). Prints the lines that differ; exit 0 if none. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pk.h"

static size_t unhex(const char *h, uint8_t *o, size_t max) {
    if (!strcmp(h, "-")) return 0;
    size_t n = strlen(h) / 2;
    if (n > max) { fprintf(stderr, "too long\n"); exit(2); }
    for (size_t i = 0; i < n; i++) { unsigned v; sscanf(h + 2 * i, "%2x", &v); o[i] = (uint8_t)v; }
    return n;
}

static char tok[8][70000];

int main(void) {
    static uint8_t b[8][35000];
    size_t l[8];
    int bad = 0, n = 0;
    char kind[16];
    while (scanf("%15s", kind) == 1) {
        int nt = !strcmp(kind, "rsa") ? 6 : !strcmp(kind, "ec") ? 6 : 2;
        for (int i = 0; i < nt; i++) if (scanf("%69999s", tok[i]) != 1) return 2;
        int got, want;
        if (!strcmp(kind, "rsa")) {
            for (int i = 0; i < 5; i++) if (i != 2) l[i] = unhex(tok[i], b[i], sizeof b[i]);
            want = atoi(tok[5]);
            got = pk_rsa_verify(b[0], l[0], b[1], l[1], atoi(tok[2]), b[3], l[3], b[4], l[4]);
        } else if (!strcmp(kind, "ec")) {
            for (int i = 1; i < 5; i++) l[i] = unhex(tok[i], b[i], sizeof b[i]);
            want = atoi(tok[5]);
            got = pk_ecdsa_verify(atoi(tok[0]), b[1], l[1], b[2], l[2], b[3], l[3], b[4], l[4]);
        } else {
            uint8_t h[48];
            l[0] = unhex(tok[0], b[0], sizeof b[0]);
            l[1] = unhex(tok[1], b[1], sizeof b[1]);
            pk_sha384(h, b[0], l[0]);
            got = !memcmp(h, b[1], 48); want = 1;
        }
        n++;
        if (got != want) { bad++; printf("FAIL %s line %d: got %d want %d\n", kind, n, got, want); }
    }
    printf("%d vectors, %d failed\n", n, bad);
    return bad != 0;
}

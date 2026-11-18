/* see charset.h */
#include "charset.h"
#include "charset_tab.h"

int charset_count(void) { return CHARSET_N; }
const char *charset_name(int cs) { return cs >= 0 && cs < CHARSET_N ? charset_names[cs] : "?"; }

static size_t put_utf8(uint32_t c, uint8_t *o) {
    if (c < 0x80) { o[0] = (uint8_t)c; return 1; }
    if (c < 0x800) { o[0] = (uint8_t)(0xc0 | c >> 6); o[1] = (uint8_t)(0x80 | (c & 63)); return 2; }
    o[0] = (uint8_t)(0xe0 | c >> 12); o[1] = (uint8_t)(0x80 | (c >> 6 & 63)); o[2] = (uint8_t)(0x80 | (c & 63));
    return 3;
}

size_t charset_in(int cs, const uint8_t *in, size_t n, uint8_t *out) {
    if (cs <= 0 || cs >= CHARSET_N) { for (size_t i = 0; i < n; i++) out[i] = in[i]; return n; }
    size_t k = 0;
    for (size_t i = 0; i < n; i++) k += in[i] < 0x80 ? (out[k] = in[i], 1) : put_utf8(charset_tab[cs - 1][in[i] - 0x80], out + k);
    return k;
}

static uint32_t acc;
static int need;

size_t charset_out(int cs, const uint8_t *in, size_t n, uint8_t *out) {
    if (cs <= 0 || cs >= CHARSET_N) { for (size_t i = 0; i < n; i++) out[i] = in[i]; return n; }
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t b = in[i];
        uint32_t c;
        if (need) {
            if ((b & 0xc0) != 0x80) { need = 0; out[k++] = '?'; }
            else { acc = acc << 6 | (b & 63); if (--need) continue; c = acc; goto have; }
        }
        if (b < 0x80) { out[k++] = b; continue; }
        if ((b & 0xe0) == 0xc0) { acc = b & 31; need = 1; continue; }
        if ((b & 0xf0) == 0xe0) { acc = b & 15; need = 2; continue; }
        if ((b & 0xf8) == 0xf0) { acc = b & 7; need = 3; continue; }
        out[k++] = '?';
        continue;
    have: {
            uint8_t r = '?';
            for (int j = 0; j < 128; j++) if (charset_tab[cs - 1][j] == c) { r = (uint8_t)(0x80 + j); break; }
            out[k++] = r;
        }
    }
    return k;
}

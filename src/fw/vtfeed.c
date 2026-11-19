/* see vtfeed.h */
#include <string.h>
#include "vtfeed.h"

/* bytes at the end of b[0..n) that start a UTF-8 sequence not complete yet */
static size_t incomplete(const unsigned char *b, size_t n) {
    for (size_t k = 1; k <= 3 && k <= n; k++) {
        unsigned char c = b[n - k];
        if ((c & 0xc0) == 0x80) continue;          /* a continuation byte: look further back */
        size_t len = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
        return len > k ? k : 0;
    }
    return 0;
}

void vtfeed(struct vtfeed *f, VTerm *vt, const char *buf, size_t n) {
    if (f->ntail) {
        /* complete the held sequence from the front of buf */
        char t[8];
        size_t need = 0, len;
        unsigned char c = (unsigned char)f->tail[0];
        len = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : 2;
        while (f->ntail + need < len && need < n && ((unsigned char)buf[need] & 0xc0) == 0x80) need++;
        memcpy(t, f->tail, f->ntail);
        memcpy(t + f->ntail, buf, need);
        if (f->ntail + need < len && need == n) {    /* still not complete: keep it */
            memcpy(f->tail, t, f->ntail + need);
            f->ntail += need;
            return;
        }
        vterm_input_write(vt, t, f->ntail + need);   /* whole, or broken (libvterm shows U+FFFD) */
        f->ntail = 0;
        buf += need;
        n -= need;
    }
    size_t k = incomplete((const unsigned char *)buf, n);
    if (n - k) vterm_input_write(vt, buf, n - k);
    memcpy(f->tail, buf + n - k, k);
    f->ntail = k;
}

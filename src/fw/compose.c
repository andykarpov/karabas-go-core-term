/* see compose.h */
#include "compose.h"
#include "compose_tab.h"

static uint32_t pair(uint32_t base, uint32_t mark) {
    int lo = 0, hi = (int)(sizeof compose_tab / sizeof compose_tab[0]) - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        uint32_t km = compose_tab[m][0], kb = compose_tab[m][1];
        if (km == mark && kb == base) return compose_tab[m][2];
        if (km < mark || (km == mark && kb < base)) lo = m + 1;
        else hi = m - 1;
    }
    return 0;
}

uint32_t compose(const uint32_t *chars, int n) {
    uint32_t c = chars[0];
    for (int i = 1; i < n && chars[i]; i++) {
        if (c > 0xffff || chars[i] > 0xffff) break;
        uint32_t p = pair(c, chars[i]);
        if (p) c = p;
    }
    return c;
}

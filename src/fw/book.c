/* see book.h */
#include <string.h>
#include "book.h"
#include "store.h"

#define SECTORS   17                         /* S_BOOK.. of store.h */

static struct conn book[BOOK_MAX];
static int n, state;                        /* 0 waiting for the store, 1 loaded, 2 no file */

static int rd(uint32_t s, uint8_t *b) { return store_rd(S_BOOK + s, b); }
static int wr(uint32_t s, const uint8_t *b) { return state == 1 ? store_wr(S_BOOK + s, b) : -1; }

static void save(void) {
    uint8_t b[512];
    memset(b, 0, sizeof b);
    memcpy(b, "TERMBOOK", 8);
    b[8] = 1;
    b[9] = (uint8_t)n;
    if (wr(0, b)) return;
    for (int s = 1; s < SECTORS; s++) {
        memset(b, 0, sizeof b);
        for (int k = 0; k < 4; k++) {
            int i = (s - 1) * 4 + k;
            if (i >= n) break;
            uint8_t *e = b + 128 * k;
            e[0] = (uint8_t)book[i].proto;
            e[1] = (uint8_t)book[i].cs;
            e[2] = (uint8_t)book[i].port; e[3] = (uint8_t)(book[i].port >> 8);
            memcpy(e + 4, book[i].host, sizeof book[i].host);
            memcpy(e + 68, book[i].user, sizeof book[i].user);
        }
        if (wr((uint32_t)s, b)) return;
        if ((s - 1) * 4 + 4 >= n) break;             /* the rest is old: count says what is valid */
    }
}

static void load(void) {
    uint8_t b[512];
    n = 0;
    if (rd(0, b) || memcmp(b, "TERMBOOK", 8) || b[8] != 1) return;   /* a new file: empty */
    int cnt = b[9] > BOOK_MAX ? BOOK_MAX : b[9];
    for (int s = 1; s < SECTORS && n < cnt; s++) {
        if (rd((uint32_t)s, b)) return;
        for (int k = 0; k < 4 && n < cnt; k++) {
            const uint8_t *e = b + 128 * k;
            struct conn *c = &book[n];
            c->proto = e[0] < P_N ? e[0] : P_TELNET;
            c->cs = e[1];
            c->port = e[2] | e[3] << 8;
            memcpy(c->host, e + 4, sizeof c->host);
            c->host[sizeof c->host - 1] = 0;
            memcpy(c->user, e + 68, sizeof c->user);          /* 0 in the entries of Ф2 */
            c->user[sizeof c->user - 1] = 0;
            n++;
        }
    }
}

void book_init(void) { state = 0; }

void book_poll(void) {
    if (state != 0) return;
    if (store_state() == 1) {                            /* mounted */
        state = 1;
        struct conn mem[BOOK_MAX];
        int m = n;
        memcpy(mem, book, sizeof mem);                   /* entries made before the mount */
        load();
        for (int i = m - 1; i >= 0; i--) book_put(&mem[i]);
        if (m == 0 && n == 0) save();                    /* a new file gets its header */
    } else if (store_state() == 2) state = 2;            /* no card / no answer: RAM only */
}

int book_ready(void) { return state == 1; }
int book_count(void) { return n; }
const struct conn *book_get(int i) { return i >= 0 && i < n ? &book[i] : 0; }

void book_put(const struct conn *c) {
    int i;
    for (i = 0; i < n; i++)
        if (book[i].proto == c->proto && book[i].port == c->port && !strcmp(book[i].host, c->host) && !strcmp(book[i].user, c->user)) break;
    if (i == n) { if (n < BOOK_MAX) n++; i = n - 1; }
    memmove(&book[1], &book[0], (size_t)i * sizeof book[0]);
    book[0] = *c;
    save();
}

void book_del(int i) {
    if (i < 0 || i >= n) return;
    memmove(&book[i], &book[i + 1], (size_t)(n - i - 1) * sizeof book[0]);
    n--;
    save();
}

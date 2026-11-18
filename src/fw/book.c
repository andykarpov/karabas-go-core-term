/* see book.h */
#include <string.h>
#include "book.h"
#include "mmio_hw.h"

#define SD_SECTOR IO(0xc0)
#define SD_CMD    IO(0xc4)
#define SD_STAT   IO(0xc8)
#define SD_SIZE   IO(0xcc)
#define SD_BKREQ  IO(0xd0)
#define SD_BUF(i) IO(0x1000 + 4 * (i))
#define SECTORS   17

static struct conn book[BOOK_MAX];
static int n, state;                        /* 0 asked, 1 loaded, 2 no file */
static uint32_t t0;

static int wait_idle(void) {
    uint32_t t = IO_MS;
    while (SD_STAT & 1) if ((int32_t)(IO_MS - t) > 2000) return -1;
    return (SD_STAT & 2) ? -1 : 0;
}
static int rd(uint32_t s, uint8_t *b) {
    SD_SECTOR = s; SD_CMD = 1;
    if (wait_idle()) return -1;
    for (int i = 0; i < 512; i++) b[i] = (uint8_t)SD_BUF(i);
    return 0;
}
static int wr(uint32_t s, const uint8_t *b) {
    if (state != 1) return -1;
    for (int i = 0; i < 512; i++) SD_BUF(i) = b[i];
    SD_SECTOR = s; SD_CMD = 2;
    return wait_idle();
}

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
            n++;
        }
    }
}

void book_init(void) { SD_BKREQ = SECTORS; state = 0; t0 = IO_MS; }

void book_poll(void) {
    if (state != 0) return;
    if (SD_STAT & 4) {                                   /* mounted */
        state = 1;
        struct conn mem[BOOK_MAX];
        int m = n;
        memcpy(mem, book, sizeof mem);                   /* entries made before the mount */
        load();
        for (int i = m - 1; i >= 0; i--) book_put(&mem[i]);
        if (m == 0 && n == 0) save();                    /* a new file gets its header */
    } else if ((int32_t)(IO_MS - t0) > 10000) state = 2; /* no card / no answer: RAM only */
}

int book_ready(void) { return state == 1; }
int book_count(void) { return n; }
const struct conn *book_get(int i) { return i >= 0 && i < n ? &book[i] : 0; }

void book_put(const struct conn *c) {
    int i;
    for (i = 0; i < n; i++)
        if (book[i].proto == c->proto && book[i].port == c->port && !strcmp(book[i].host, c->host)) break;
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

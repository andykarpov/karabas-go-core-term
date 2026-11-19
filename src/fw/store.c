/* see store.h */
#include "store.h"
#include "mmio_hw.h"

#define SD_SECTOR IO(0xc0)
#define SD_CMD    IO(0xc4)
#define SD_STAT   IO(0xc8)
#define SD_SIZE   IO(0xcc)
#define SD_BKREQ  IO(0xd0)
#define SD_BUF(i) IO(0x1000 + 4 * (i))

static int state;
static uint32_t t0;

static int wait_idle(void) {
    uint32_t t = IO_MS;
    while (SD_STAT & 1) if ((int32_t)(IO_MS - t) > 2000) return -1;
    return (SD_STAT & 2) ? -1 : 0;
}

int store_rd(uint32_t s, uint8_t *b) {
    if (state != 1) return -1;
    SD_SECTOR = s; SD_CMD = 1;
    if (wait_idle()) return -1;
    for (int i = 0; i < 512; i++) b[i] = (uint8_t)SD_BUF(i);
    return 0;
}

int store_wr(uint32_t s, const uint8_t *b) {
    if (state != 1) return -1;
    for (int i = 0; i < 512; i++) SD_BUF(i) = b[i];
    SD_SECTOR = s; SD_CMD = 2;
    return wait_idle();
}

void store_init(void) { SD_BKREQ = STORE_SECTORS; state = 0; t0 = IO_MS; }

void store_poll(void) {
    if (state != 0) return;
    if (SD_STAT & 4) state = 1;                          /* mounted */
    else if ((int32_t)(IO_MS - t0) > 10000) state = 2;  /* no card / no answer: RAM only */
}

int store_state(void) { return state; }

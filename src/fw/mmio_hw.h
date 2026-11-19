/* MMIO of the board SoC (src/rtl/term_soc.v). SRAM 4 MB at 0: program 0..1 MB, row ring 0x100000,
   alternate screen 0x1c0000, font 0x200000 (codepoint * 32). MMIO at 0x80000000 (uncached). */
#pragma once
#include <stdint.h>

#define IO(o)        (*(volatile uint32_t *)(0x80000000u + (o)))
#define IO_DBG       IO(0x00)   /* W: a debug console byte */
#define IO_EXIT      IO(0x08)   /* W: exit code */
#define IO_CYCLES    IO(0x0c)
#define IO_MS        IO(0x1c)
#define IO_KBD0      IO(0x28)   /* { key 3, key 2, key 1, modifiers } */
#define IO_KBD1      IO(0x2c)   /* { 0, key 6, key 5, key 4 } */
#define IO_KBDN      IO(0x30)   /* reports seen */
#define IO_MOUSE     IO(0x34)
#define IO_US        IO(0x40)
#define IO_TCMP      IO(0x44)
#define IO_TEN       IO(0x48)
#define IO_SW        IO(0x50)
#define IO_SER_RX    IO(0x60)   /* R: bit 8 = a byte (taken), 7..0 */
#define IO_SER_TX    IO(0x64)   /* W: a byte to the host */
#define IO_SER_STAT  IO(0x68)   /* bit 0 rx waiting, bit 1 tx full */
#define IO_SER_CFG   IO(0x6c)
#define IO_ESP_RX    IO(0x70)   /* R: bit 8 = a byte (taken), 7..0: the ESP8266 UART */
#define IO_ESP_TX    IO(0x74)   /* W: a byte to the module */
#define IO_ESP_STAT  IO(0x78)   /* bit 0 rx waiting, bit 1 tx busy, bit 2 rx overrun, 27..16 rx count */
#define IO_DEF_FG    IO(0x80)
#define IO_DEF_BG    IO(0x84)
#define IO_CURSOR    IO(0x88)   /* { blink 24, shape 17..16, on 15, y 13..8, x 6..0 } */
#define IO_SHOW      IO(0x8c)
#define IO_FRAMES    IO(0x90)
#define IO_BELL      IO(0x94)   /* W: a short tone (880 Hz, 120 ms) */
#define IO_ROWTAB(n) IO(0x100 + 4 * (n))  /* SRAM byte address of the row / 1024 */
#define IO_PAL(n)    IO(0x400 + 4 * (n))  /* 0x00RRGGBB */

#define RING_BASE  0x00100000u
#define ALT_BASE   0x001c0000u
#define FONT_BASE  0x00200000u

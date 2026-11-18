/* Ф0 bench MMIO (src/sim/iss.cpp). SRAM 4 MB at 0x00000000, SDRAM 32 MB at 0x10000000, MMIO at 0x80000000. */
#pragma once
#include <stdint.h>

#define MMIO_REG(o)  (*(volatile uint32_t *)(0x80000000u + (o)))
#define MMIO_UART    MMIO_REG(0x00) /* write: console byte */
#define MMIO_EXIT    MMIO_REG(0x08) /* write: stop with this code */
#define MMIO_INSTR   MMIO_REG(0x0c) /* read: retired instructions, low 32 bits */
#define MMIO_MARK    MMIO_REG(0x10) /* write: the ISS prints the counters since the previous mark */
#define MMIO_INSIZE  MMIO_REG(0x20) /* read: size of the input at IN_BASE */
#define IN_BASE      ((const uint8_t *)0x11000000u)

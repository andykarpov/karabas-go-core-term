/* The save file of the term core on the SD card (term_sd.v, the backup RAM request of link v2:
   /term/font.sav; the firmware makes it, or extends an older one, to the size asked). Sectors:
     0..16   the phonebook (book.c)
     17      the SSH keys: the user key, the seed of rng.c (sshkeys.c)
     18..33  known_hosts (sshkeys.c)
   No card / no answer: everything lives in RAM only. */
#pragma once
#include <stdint.h>

#define STORE_SECTORS 34
#define S_BOOK   0
#define S_KEYS   17
#define S_HOSTS  18
#define S_HOSTS_N 16

void store_init(void);                      /* ask the firmware for the file */
void store_poll(void);                      /* call often: 0 -> mounted (1) or none (2) */
int store_state(void);                      /* 0 asked, 1 mounted, 2 no file (RAM only) */
int store_rd(uint32_t sector, uint8_t *b);  /* 512 bytes; 0 ok */
int store_wr(uint32_t sector, const uint8_t *b);

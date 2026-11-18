/* the phonebook of the term core on the SD card: the save file of the core (term_sd.v, the backup RAM
   request of link v2: /term/font.sav, 17 sectors). Sector 0: "TERMBOOK" + version + count; sectors
   1..16: 4 entries of 128 bytes each (proto, charset, port, host). Not mounted yet / no card: the list
   lives in RAM only. */
#pragma once
#include "ui.h"

#define BOOK_MAX 64

void book_init(void);                       /* ask the firmware for the file */
void book_poll(void);                       /* mount, then load once: call often */
int book_ready(void);                       /* the file is there (loaded) */
int book_count(void);
const struct conn *book_get(int i);
void book_put(const struct conn *c);        /* to the front (an equal one moves), saved */
void book_del(int i);                       /* saved */

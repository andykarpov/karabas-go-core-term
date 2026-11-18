/* the input of libvterm in pieces of any size: a UTF-8 sequence split between two vterm_input_write
   calls is held back and written whole with the next piece. libvterm 0.3.3 turns a combining mark
   whose bytes are split ("e" "\xcc" | "\x81") into U+FFFD + one more cell (seen in Ф1: the serial
   port gives a few bytes at a time). */
#pragma once
#include <stddef.h>
#include "vterm.h"

void vtfeed(VTerm *vt, const char *buf, size_t n);

/* the charset of a connection: 0 UTF-8 (as it is), 1.. 8-bit sets (charset_tab.h) translated to and from
   the UTF-8 of libvterm */
#pragma once
#include <stdint.h>
#include <stddef.h>

int charset_count(void);
const char *charset_name(int cs);
/* bytes of the host -> UTF-8 into out (4 x n at most); the bytes written */
size_t charset_in(int cs, const uint8_t *in, size_t n, uint8_t *out);
/* UTF-8 of libvterm -> the bytes of the host into out (n at most); the bytes written. A codepoint the
   charset lacks goes out as '?'. A UTF-8 sequence split between calls is kept in *st (a terminal each, zeroed). */
struct cs_out { uint32_t acc; int need; };
size_t charset_out(int cs, struct cs_out *st, const uint8_t *in, size_t n, uint8_t *out);

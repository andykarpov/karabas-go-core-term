/* telnet (RFC 854) on a TCP connection: IAC stripped and answered; options ECHO, SGA, BINARY (both ways),
   TTYPE ("XTERM-256COLOR"), NAWS (the screen size); others refused. */
#pragma once
#include <stdint.h>
#include <stddef.h>

void telnet_reset(int cols, int rows, size_t (*send)(const uint8_t *, size_t));
/* bytes of the host: the data part into out (n at most); the bytes written */
size_t telnet_in(const uint8_t *in, size_t n, uint8_t *out);
/* bytes of the user to the host (IAC doubled, CR as CR NUL unless BINARY) */
void telnet_out(const uint8_t *p, size_t n);

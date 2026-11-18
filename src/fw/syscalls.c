/* Ф0 bench newlib syscalls: heap from the linker script, console = MMIO_UART, exit = MMIO_EXIT. */
#include <errno.h>
#include <stddef.h>
#include <sys/stat.h>
#include "mmio.h"

extern char __heap_start[], __heap_end[];
static char *brk = __heap_start;

void *_sbrk(ptrdiff_t incr) {
    if (brk + incr > __heap_end) { errno = ENOMEM; return (void *)-1; }
    char *p = brk; brk += incr; return p;
}
int _write(int fd, const char *buf, int len) { (void)fd; for (int i = 0; i < len; i++) MMIO_UART = (uint8_t)buf[i]; return len; }
int _read(int fd, char *buf, int len) { (void)fd; (void)buf; (void)len; return 0; }
int _open(const char *n, int f, int m) { (void)n; (void)f; (void)m; errno = ENOENT; return -1; }
int _close(int fd) { (void)fd; return -1; }
int _fstat(int fd, struct stat *st) { (void)fd; st->st_mode = S_IFCHR; return 0; }
int _isatty(int fd) { (void)fd; return 1; }
int _lseek(int fd, int o, int w) { (void)fd; (void)o; (void)w; return 0; }
int _getpid(void) { return 1; }
int _kill(int p, int s) { (void)p; (void)s; errno = EINVAL; return -1; }
void _exit(int code) { MMIO_EXIT = (uint32_t)code; for (;;) {} }

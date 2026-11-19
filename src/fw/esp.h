/* The ESP8266 (stock NonOS AT 1.7 firmware) on its UART (term_uart.v), non-blocking (docs/PLAN.md Ф0.3, Ф2, Ф5):
   CIPMODE 0 (+IPD frames: 8-bit clean, CLOSED seen); links 0..ESP_LINKS-1 with CIPMUX 1 while any is open,
   CIPMUX 0 when none is (as the module is left for the other cores); nothing stored in the module.
   esp_poll() runs the engine: call it often. */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define ESP_LINKS 4                        /* a tab each (the module has 5) */

enum esp_state { ESP_IDLE, ESP_CONNECTING, ESP_CONNECTED, ESP_CLOSING };

void esp_init(void);                       /* sync with the module: AT, ATE0, CIPMODE 0, CIPCLOSE, CIPMUX 0 */
void esp_poll(void);
int esp_ready(void);                       /* the module answered the init */
const char *esp_ip(void);                  /* "" until AT+CIFSR answered with an IP */
void esp_refresh_ip(void);                 /* ask AT+CIFSR again */
/* link id: 0 asked; the state goes CONNECTING -> CONNECTED or IDLE (also while the old link of id closes) */
int esp_connect(int id, const char *host, int port);
void esp_close(int id);
enum esp_state esp_state(int id);
const char *esp_error(int id);             /* the last failure of the link, "" if none */
size_t esp_send(int id, const uint8_t *p, size_t n);   /* queue bytes for the remote host: the bytes taken */
int esp_links(void);                       /* links not idle */

/* bytes from the remote host of a link (set by the user of the engine) */
extern void (*esp_on_data)(int id, const uint8_t *p, size_t n);

/* The ESP8266 (stock NonOS AT 1.7 firmware) on its UART (term_uart.v), non-blocking (docs/PLAN.md Ф0.3, Ф2):
   CIPMODE 0 (+IPD frames: 8-bit clean, CLOSED seen), CIPMUX 0, nothing stored in the module.
   esp_poll() runs the engine: call it often. */
#pragma once
#include <stdint.h>
#include <stddef.h>

enum esp_state { ESP_IDLE, ESP_CONNECTING, ESP_CONNECTED, ESP_CLOSING };

void esp_init(void);                       /* sync with the module: AT, ATE0, CIPMODE 0, CIPMUX 0, CIPCLOSE */
void esp_poll(void);
int esp_ready(void);                       /* the module answered the init */
const char *esp_ip(void);                  /* "" until AT+CIFSR answered with an IP */
void esp_refresh_ip(void);                 /* ask AT+CIFSR again */
int esp_connect(const char *host, int port);   /* 0: asked; the state goes CONNECTING -> CONNECTED or IDLE */
void esp_close(void);
enum esp_state esp_state(void);
const char *esp_error(void);               /* the last failure, "" if none */
size_t esp_send(const uint8_t *p, size_t n);   /* queue bytes for the remote host: the bytes taken */

/* bytes from the remote host (set by the user of the engine) */
extern void (*esp_on_data)(const uint8_t *p, size_t n);

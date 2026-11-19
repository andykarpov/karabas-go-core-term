/* see esp.h. One command at a time: a command line goes out, the engine waits for its final line
   (OK / ERROR / FAIL / SEND OK / ...), +IPD frames and CLOSED can come at any time in between. */
#include <string.h>
#include <stdio.h>
#include "esp.h"
#include "mmio_hw.h"

void (*esp_on_data)(const uint8_t *p, size_t n);

#define TXQ 4096
static uint8_t txq[TXQ];                   /* bytes for the remote host */
static size_t txq_r, txq_w;
static size_t txq_n(void) { return txq_w - txq_r; }

static enum esp_state st = ESP_IDLE;
static char ip[20], err[48];
static int ready;

/* the current command */
enum cmd { C_NONE, C_INIT, C_IP, C_START, C_SEND, C_SENDDATA, C_CLOSE };
static enum cmd cur = C_NONE;
static int init_step;
static uint32_t cmd_t;                     /* ms the command went out */
static size_t send_n;                      /* bytes of the CIPSEND in flight */
static uint32_t txq_t;                     /* ms the oldest queued byte came */
static int want_ip, want_close;
static char conn_cmd[160];
static int want_connect;

/* receive: lines, "> ", "+IPD,n:" + n bytes */
static char line[256];
static size_t line_n;
static size_t ipd_left;
static uint8_t ipd_buf[256];
static size_t ipd_n;

static void tx(uint8_t c) { while (IO_ESP_STAT & 2) {} IO_ESP_TX = c; }
static void txs(const char *s) { while (*s) tx((uint8_t)*s++); }

static void command(enum cmd c, const char *s) {
    cur = c;
    cmd_t = IO_MS;
    txs(s);
    txs("\r\n");
}

static const char *init_cmds[] = { "AT", "ATE0", "AT+CIPMODE=0", "AT+CIPMUX=0", "AT+CIPCLOSE", "AT+CIFSR" };
#define INIT_N (int)(sizeof init_cmds / sizeof init_cmds[0])

void esp_init(void) {
    ready = 0;
    init_step = 0;
    st = ESP_IDLE;
    txq_r = txq_w = 0;
    command(C_INIT, init_cmds[0]);
}

int esp_ready(void) { return ready; }
const char *esp_ip(void) { return ip; }
const char *esp_error(void) { return err; }
enum esp_state esp_state(void) { return st; }
void esp_refresh_ip(void) { want_ip = 1; }

int esp_connect(const char *host, int port) {
    if (st != ESP_IDLE || !ready) return -1;
    snprintf(conn_cmd, sizeof conn_cmd, "AT+CIPSTART=\"TCP\",\"%s\",%d", host, port);
    want_connect = 1;
    st = ESP_CONNECTING;
    err[0] = 0;
    txq_r = txq_w = 0;
    return 0;
}

void esp_close(void) { if (st == ESP_CONNECTED || st == ESP_CONNECTING) want_close = 1; }

size_t esp_send(const uint8_t *p, size_t n) {
    if (st != ESP_CONNECTED) return 0;
    size_t k = 0;
    if (!txq_n()) txq_t = IO_MS;
    while (k < n && txq_n() < TXQ) txq[txq_w++ % TXQ] = p[k++];
    return k;
}

/* a whole line came (no CR LF) */
static void on_line(const char *l) {
    if (!strcmp(l, "CLOSED") || !strcmp(l, "0,CLOSED")) {
        if (st == ESP_CONNECTED) strcpy(err, "closed by the remote host");
        st = ESP_IDLE;
        txq_r = txq_w = 0;
        if (cur == C_SEND || cur == C_SENDDATA) cur = C_NONE;
    }
    if (!strncmp(l, "+CIFSR:STAIP,\"", 14)) {
        const char *a = l + 14;
        size_t k = 0;
        while (a[k] && a[k] != '"' && k < sizeof ip - 1) { ip[k] = a[k]; k++; }
        ip[k] = 0;
        if (!strcmp(ip, "0.0.0.0")) ip[0] = 0;
    }
    if (!strcmp(l, "CONNECT") && cur == C_START) st = ESP_CONNECTED;
    if (!strcmp(l, "DNS Fail")) strcpy(err, "DNS: name not found");
    if (!strcmp(l, "no ip")) strcpy(err, "Wi-Fi not connected (Setup Wi-Fi in the boot core)");

    int ok = !strcmp(l, "OK") || !strcmp(l, "SEND OK");
    int fail = !strcmp(l, "ERROR") || !strcmp(l, "FAIL") || !strcmp(l, "SEND FAIL");
    if (!ok && !fail) return;
    switch (cur) {
    case C_INIT:
        if (init_step == 0 && fail) { command(C_INIT, init_cmds[0]); break; }
        if (++init_step < INIT_N) command(C_INIT, init_cmds[init_step]);
        else { cur = C_NONE; ready = 1; }
        break;
    case C_IP: cur = C_NONE; break;
    case C_START:
        cur = C_NONE;
        if (fail || st != ESP_CONNECTED) { st = ESP_IDLE; if (!err[0]) strcpy(err, "connection failed"); }
        break;
    case C_SEND:
        /* "OK" of AT+CIPSEND=n: the "> " prompt follows (handled in the byte loop) */
        if (fail) { cur = C_NONE; strcpy(err, "send failed"); }
        break;
    case C_SENDDATA:
        if (!strcmp(l, "SEND OK") || fail) cur = C_NONE;
        break;
    case C_CLOSE: cur = C_NONE; st = ESP_IDLE; break;
    default: break;
    }
}

static void on_byte(uint8_t c) {
    if (ipd_left) {
        ipd_buf[ipd_n++] = c;
        if (--ipd_left == 0 || ipd_n == sizeof ipd_buf) {
            if (esp_on_data) esp_on_data(ipd_buf, ipd_n);
            ipd_n = 0;
        }
        return;
    }
    if (c == '\n') {
        if (line_n && line[line_n - 1] == '\r') line_n--;
        line[line_n] = 0;
        if (line_n) on_line(line);
        line_n = 0;
        return;
    }
    if (line_n < sizeof line - 1) line[line_n++] = (char)c;
    /* "+IPD,n:" ends without a line end */
    if (c == ':' && line_n > 5 && !strncmp(line, "+IPD,", 5)) {
        line[line_n] = 0;
        int n = 0;
        for (size_t i = 5; i < line_n - 1; i++) if (line[i] >= '0' && line[i] <= '9') n = n * 10 + line[i] - '0';
        ipd_left = (size_t)n;
        ipd_n = 0;
        line_n = 0;
        return;
    }
    /* the prompt of CIPSEND: "> " (no line end) */
    if (cur == C_SEND && line_n == 1 && c == '>') {
        line_n = 0;
        cur = C_SENDDATA;
        cmd_t = IO_MS;
        for (size_t i = 0; i < send_n; i++) tx(txq[txq_r++ % TXQ]);
    }
}

void esp_poll(void) {
    uint32_t r;
    int k = 0;
    while (k++ < 512 && ((r = IO_ESP_RX) & 0x100)) on_byte((uint8_t)r);
    uint32_t now = IO_MS;
    if (cur != C_NONE) {
        /* a module that does not answer: give up the command (a send waits longer) */
        if ((int32_t)(now - cmd_t) > (cur == C_START ? 15000 : cur == C_INIT ? 1000 : 5000)) {
            if (cur == C_INIT) { command(C_INIT, init_cmds[init_step]); return; }
            if (cur == C_START) { st = ESP_IDLE; strcpy(err, "connection timed out"); }
            if (cur == C_SEND || cur == C_SENDDATA) { strcpy(err, "the module stopped answering"); }
            cur = C_NONE;
        }
        return;
    }
    if (!ready) return;
    if (want_close) {
        want_close = 0;
        want_connect = 0;
        st = ESP_CLOSING;
        command(C_CLOSE, "AT+CIPCLOSE");
        return;
    }
    if (want_connect) {
        want_connect = 0;
        command(C_START, conn_cmd);
        return;
    }
    if (want_ip) { want_ip = 0; command(C_IP, "AT+CIFSR"); return; }
    /* the queued bytes go out after 10 ms (keys of a burst together) or when there are many */
    if (st == ESP_CONNECTED && txq_n() && (txq_n() >= 512 || (int32_t)(now - txq_t) >= 10)) {
        char c[24];
        send_n = txq_n() > 2048 ? 2048 : txq_n();
        snprintf(c, sizeof c, "AT+CIPSEND=%u", (unsigned)send_n);
        command(C_SEND, c);
        txq_t = now;
    }
}

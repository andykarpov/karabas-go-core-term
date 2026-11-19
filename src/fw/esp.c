/* see esp.h. One command at a time: a command line goes out, the engine waits for its final line
   (OK / ERROR / FAIL / SEND OK / ...), +IPD frames and <id>,CLOSED can come at any time in between (the bench:
   docs/PLAN.md Ф0.3, Ф5.0). CIPMUX=1 is set before the first link opens and CIPMUX=0 after the last closes. */
#include <string.h>
#include <stdio.h>
#include "esp.h"
#include "mmio_hw.h"
#include "rng.h"

void (*esp_on_data)(int id, const uint8_t *p, size_t n);

#define TXQ 4096
struct link {
    enum esp_state st;
    uint8_t txq[TXQ];                      /* bytes for the remote host */
    size_t txq_r, txq_w;
    uint32_t txq_t;                        /* ms the oldest queued byte came */
    char err[96];
    char conn_cmd[160];                    /* AT+CIPSTART=<id>,... */
    int want_connect, want_close;
    uint32_t close_t;                      /* ms esp_close() was asked: the queued bytes go first (300 ms at most) */
    int connect_after_close;               /* esp_connect() while the old connection is being closed */
};
static struct link L[ESP_LINKS];
static size_t txq_n(const struct link *l) { return l->txq_w - l->txq_r; }

static char ip[20];
static int ready;
static int mux;                            /* the CIPMUX of the module (0 after the init) */
static int want_mux;                       /* -1 none, else the CIPMUX to set */

/* the current command */
enum cmd { C_NONE, C_INIT, C_IP, C_MUX, C_START, C_SEND, C_SENDDATA, C_CLOSE };
static enum cmd cur = C_NONE;
static int cur_id;                         /* the link of the command */
static int init_step;
static uint32_t cmd_t;                     /* ms the command went out */
static size_t send_n;                      /* bytes of the CIPSEND in flight */
static int want_ip;
static int rr;                             /* the next link to send for (round robin) */

/* receive: lines, "> ", "+IPD,id,n:" + n bytes */
static char line[256];
static size_t line_n;
static size_t ipd_left;
static int ipd_id;
static uint8_t ipd_buf[256];
static size_t ipd_n;

static void tx(uint8_t c) { while (IO_ESP_STAT & 2) {} IO_ESP_TX = c; }
static void txs(const char *s) { while (*s) tx((uint8_t)*s++); }

static void command(enum cmd c, int id, const char *s) {
    cur = c;
    cur_id = id;
    cmd_t = IO_MS;
    txs(s);
    txs("\r\n");
}

/* a term left with links open (CIPMUX 1) or one link (CIPMUX 0): both closes, errors do not matter */
static const char *init_cmds[] = { "AT", "ATE0", "AT+CIPMODE=0", "AT+CIPCLOSE=5", "AT+CIPCLOSE", "AT+CIPMUX=0", "AT+CIFSR" };
#define INIT_N (int)(sizeof init_cmds / sizeof init_cmds[0])

static int valid(int id) { return id >= 0 && id < ESP_LINKS; }

void esp_init(void) {
    ready = 0;
    init_step = 0;
    mux = 0;
    want_mux = -1;
    memset(L, 0, sizeof L);
    command(C_INIT, 0, init_cmds[0]);
}

int esp_ready(void) { return ready; }
const char *esp_ip(void) { return ip; }
const char *esp_error(int id) { return valid(id) ? L[id].err : ""; }
enum esp_state esp_state(int id) { return valid(id) ? L[id].st : ESP_IDLE; }
void esp_refresh_ip(void) { want_ip = 1; }

int esp_links(void) {
    int n = 0;
    for (int i = 0; i < ESP_LINKS; i++) n += L[i].st != ESP_IDLE || L[i].want_connect || L[i].connect_after_close;
    return n;
}

static void start_link(struct link *l) { l->want_connect = 1; l->st = ESP_CONNECTING; l->txq_r = l->txq_w = 0; }

int esp_connect(int id, const char *host, int port) {
    if (!ready || !valid(id)) return -1;
    struct link *l = &L[id];
    int closing = l->want_close || l->st == ESP_CLOSING;
    if (l->st != ESP_IDLE && !closing) return -1;
    snprintf(l->conn_cmd, sizeof l->conn_cmd, "AT+CIPSTART=%d,\"TCP\",\"%s\",%d", id, host, port);
    l->err[0] = 0;
    if (closing) { l->connect_after_close = 1; return 0; }
    start_link(l);
    return 0;
}

void esp_close(int id) {
    if (!valid(id)) return;
    struct link *l = &L[id];
    l->connect_after_close = 0;
    if (l->st == ESP_CONNECTING && l->want_connect) { l->want_connect = 0; l->st = ESP_IDLE; return; }  /* not sent yet */
    if (l->st == ESP_CONNECTED || l->st == ESP_CONNECTING) { if (!l->want_close) l->close_t = IO_MS; l->want_close = 1; }
}

size_t esp_send(int id, const uint8_t *p, size_t n) {
    if (!valid(id) || L[id].st != ESP_CONNECTED) return 0;
    struct link *l = &L[id];
    size_t k = 0;
    if (!txq_n(l)) l->txq_t = IO_MS;
    while (k < n && txq_n(l) < TXQ) l->txq[l->txq_w++ % TXQ] = p[k++];
    return k;
}

/* the link went away (closed by either side, or its connect failed) */
static void link_down(int id) {
    struct link *l = &L[id];
    l->st = ESP_IDLE;
    l->want_close = 0;
    l->txq_r = l->txq_w = 0;
    if ((cur == C_SEND || cur == C_SENDDATA) && cur_id == id) cur = C_NONE;
    if (l->connect_after_close) { l->connect_after_close = 0; start_link(l); }
}

/* "<id>,WORD" */
static int id_line(const char *l, const char *word) {
    return l[0] >= '0' && l[0] <= '4' && l[1] == ',' && !strcmp(l + 2, word) ? l[0] - '0' : -1;
}

/* a whole line came (no CR LF) */
static void on_line(const char *l) {
    rng_event(1);                          /* when a line comes: decided by the network */
    int id;
    if ((id = id_line(l, "CLOSED")) >= 0 || (!strcmp(l, "CLOSED") && (id = 0) == 0)) {
        if (valid(id)) {
            if (L[id].st == ESP_CONNECTED) strcpy(L[id].err, "closed by the remote host");
            else if (L[id].st == ESP_CONNECTING && !L[id].err[0]) strcpy(L[id].err, "connection failed");
            if (!(cur == C_START && cur_id == id)) link_down(id);     /* a failing CIPSTART: its ERROR follows */
            else L[id].st = ESP_IDLE;
        }
    }
    if (!strncmp(l, "+CIFSR:STAIP,\"", 14)) {
        const char *a = l + 14;
        size_t k = 0;
        while (a[k] && a[k] != '"' && k < sizeof ip - 1) { ip[k] = a[k]; k++; }
        ip[k] = 0;
        if (!strcmp(ip, "0.0.0.0")) ip[0] = 0;
    }
    if ((id = id_line(l, "CONNECT")) >= 0 && cur == C_START && id == cur_id) L[id].st = ESP_CONNECTED;
    if (cur == C_START) {
        if (!strcmp(l, "DNS Fail")) strcpy(L[cur_id].err, "DNS: name not found");
        if (!strcmp(l, "no ip")) strcpy(L[cur_id].err, "Wi-Fi not connected (Setup Wi-Fi in the boot core)");
    }

    int ok = !strcmp(l, "OK") || !strcmp(l, "SEND OK");
    int fail = !strcmp(l, "ERROR") || !strcmp(l, "FAIL") || !strcmp(l, "SEND FAIL");
    if (!ok && !fail) return;
    switch (cur) {
    case C_INIT:
        if (init_step == 0 && fail) { command(C_INIT, 0, init_cmds[0]); break; }
        if (++init_step < INIT_N) command(C_INIT, 0, init_cmds[init_step]);
        else { cur = C_NONE; ready = 1; }
        break;
    case C_IP: cur = C_NONE; break;
    case C_MUX:
        cur = C_NONE;
        if (ok) mux = want_mux;
        want_mux = -1;
        break;
    case C_START:
        cur = C_NONE;
        if (fail || L[cur_id].st != ESP_CONNECTED) {
            if (!L[cur_id].err[0]) strcpy(L[cur_id].err, "connection failed");
            link_down(cur_id);
        }
        break;
    case C_SEND:
        /* "OK" of AT+CIPSEND=id,n: the "> " prompt follows (handled in the byte loop) */
        if (fail) { cur = C_NONE; strcpy(L[cur_id].err, "send failed"); }
        break;
    case C_SENDDATA:
        if (!strcmp(l, "SEND OK") || fail) cur = C_NONE;
        break;
    case C_CLOSE:
        cur = C_NONE;
        link_down(cur_id);
        break;
    default: break;
    }
}

static void ipd_flush(void) {
    if (ipd_n && esp_on_data) esp_on_data(ipd_id, ipd_buf, ipd_n);
    ipd_n = 0;
}

static void on_byte(uint8_t c) {
    if (ipd_left) {
        ipd_buf[ipd_n++] = c;
        if (--ipd_left == 0 || ipd_n == sizeof ipd_buf) ipd_flush();
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
    /* "+IPD,id,n:" (CIPMUX 1) or "+IPD,n:" ends without a line end */
    if (c == ':' && line_n > 5 && !strncmp(line, "+IPD,", 5)) {
        line[line_n] = 0;
        const char *p = line + 5;
        int id = 0, n = 0;
        const char *comma = strchr(p, ',');
        if (comma) { id = *p - '0'; p = comma + 1; }
        for (; *p >= '0' && *p <= '9'; p++) n = n * 10 + *p - '0';
        ipd_id = valid(id) ? id : 0;
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
        struct link *l = &L[cur_id];
        for (size_t i = 0; i < send_n; i++) tx(l->txq[l->txq_r++ % TXQ]);
    }
}

void esp_poll(void) {
    uint32_t r;
    int k = 0;
    while (k++ < 512 && ((r = IO_ESP_RX) & 0x100)) on_byte((uint8_t)r);
    uint32_t now = IO_MS;
    if (cur != C_NONE) {
        /* a module that does not answer: give up the command (a connect waits longer) */
        if ((int32_t)(now - cmd_t) > (cur == C_START ? 15000 : cur == C_INIT ? 1000 : 5000)) {
            if (cur == C_INIT) { command(C_INIT, 0, init_cmds[init_step]); return; }
            if (cur == C_START) { strcpy(L[cur_id].err, "connection timed out"); link_down(cur_id); }
            if (cur == C_SEND || cur == C_SENDDATA) strcpy(L[cur_id].err, "the module stopped answering");
            if (cur == C_CLOSE) link_down(cur_id);   /* no answer to CIPCLOSE: idle anyway */
            if (cur == C_MUX) want_mux = -1;
            cur = C_NONE;
        }
        return;
    }
    if (!ready) return;
    char c[200];
    for (int i = 0; i < ESP_LINKS; i++) {
        struct link *l = &L[i];
        if (!l->want_close) continue;
        if (l->st == ESP_CONNECTED && txq_n(l) && (int32_t)(now - l->close_t) < 300) continue;   /* e.g. the DISCONNECT of SSH */
        l->want_close = 0;
        l->want_connect = 0;
        l->st = ESP_CLOSING;
        snprintf(c, sizeof c, "AT+CIPCLOSE=%d", i);
        command(C_CLOSE, i, c);
        return;
    }
    int connects = 0;
    for (int i = 0; i < ESP_LINKS; i++) connects += L[i].want_connect;
    if (connects && !mux) { want_mux = 1; command(C_MUX, 0, "AT+CIPMUX=1"); return; }
    for (int i = 0; i < ESP_LINKS; i++) {
        struct link *l = &L[i];
        if (!l->want_connect) continue;
        l->want_connect = 0;
        command(C_START, i, l->conn_cmd);
        return;
    }
    if (mux && !esp_links()) { want_mux = 0; command(C_MUX, 0, "AT+CIPMUX=0"); return; }
    if (want_ip) { want_ip = 0; command(C_IP, 0, "AT+CIFSR"); return; }
    /* the queued bytes go out after 10 ms (keys of a burst together) or when there are many; a link a turn */
    for (int j = 0; j < ESP_LINKS; j++) {
        int i = (rr + j) % ESP_LINKS;
        struct link *l = &L[i];
        if (l->st != ESP_CONNECTED || !txq_n(l) || (txq_n(l) < 512 && (int32_t)(now - l->txq_t) < 10)) continue;
        send_n = txq_n(l) > 2048 ? 2048 : txq_n(l);
        snprintf(c, sizeof c, "AT+CIPSEND=%d,%u", i, (unsigned)send_n);
        command(C_SEND, i, c);
        l->txq_t = now;
        rr = i + 1;
        return;
    }
}

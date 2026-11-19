/* see ssh.h. RFC 4253 (transport), 4252 (auth), 4254 (connection), 4256 (keyboard-interactive),
   8709 (ssh-ed25519), 8731 (curve25519-sha256), OpenSSH PROTOCOL.chacha20poly1305 and PROTOCOL (strict kex). */
#include <string.h>
#include <stdio.h>
#include "ssh.h"
#include "sha256.h"
#include "monocypher.h"
#include "monocypher-ed25519.h"

#define VERSION "SSH-2.0-KarabasTerm_0.3"

enum {
    MSG_DISCONNECT = 1, MSG_IGNORE, MSG_UNIMPLEMENTED, MSG_DEBUG, MSG_SERVICE_REQUEST, MSG_SERVICE_ACCEPT,
    MSG_EXT_INFO, MSG_KEXINIT = 20, MSG_NEWKEYS, MSG_KEX_ECDH_INIT = 30, MSG_KEX_ECDH_REPLY,
    MSG_USERAUTH_REQUEST = 50, MSG_USERAUTH_FAILURE, MSG_USERAUTH_SUCCESS, MSG_USERAUTH_BANNER,
    MSG_USERAUTH_60 = 60, MSG_USERAUTH_INFO_RESPONSE,
    MSG_GLOBAL_REQUEST = 80, MSG_REQUEST_SUCCESS, MSG_REQUEST_FAILURE,
    MSG_CHANNEL_OPEN = 90, MSG_CHANNEL_OPEN_CONFIRMATION, MSG_CHANNEL_OPEN_FAILURE, MSG_CHANNEL_WINDOW_ADJUST,
    MSG_CHANNEL_DATA, MSG_CHANNEL_EXTENDED_DATA, MSG_CHANNEL_EOF, MSG_CHANNEL_CLOSE, MSG_CHANNEL_REQUEST,
    MSG_CHANNEL_SUCCESS, MSG_CHANNEL_FAILURE
};

#define LMAXPKT 16384                 /* the max packet of our channel (was 32768: 4 sessions, docs/PLAN.md Ф5) */
#define RXCAP   (LMAXPKT + 2048)      /* a channel packet of LMAXPKT data + headers; others are small (RFC 4253 6.1
                                         wants 35000: no server sends that much outside the channel in practice) */
#define TXP     8192                  /* our biggest packet: a channel data packet of 4096 bytes, auth */
#define OUTQ    16384                 /* encrypted bytes for the TCP connection */
#define SSH_LWIN (1u << 20)            /* the window we give the server */
#define IS_MAX  4096                  /* the KEXINIT of the server we keep for the exchange hash */

struct dir {
    int on;
    uint8_t key[64];          /* main key 0..31, header (length) key 32..63 */
    uint32_t seq;
};
enum askq { A_NONE, A_HOST_NEW, A_HOST_CHANGED, A_USER, A_PASS, A_KBI };

/* one session (docs/PLAN.md Ф5: a tab each); the public functions set S */
struct ssh {
    int used;
    const struct ssh_io *io;
    void *ctx;                            /* of the user: given to io->send / io->data */
    enum ssh_state st;
    char err[128];
    char host[64], user[64];
    int port, cols, rows;
    uint8_t outq[OUTQ];                   /* encrypted bytes for the TCP connection */
    size_t oq_r, oq_w;
    int kex_n, strict;
    struct dir tx, rx;
    struct dir tx_next, rx_next;          /* the keys of a kex, used after NEWKEYS */
    char vs[256];                         /* the version line of the server */
    uint8_t ic[512]; size_t ic_n;
    uint8_t is_[IS_MAX]; size_t is_n;
    uint8_t session_id[32]; int have_session;
    uint8_t x_sk[32], q_c[32];
    uint8_t host_pk[32];                  /* the host key of the first kex (a rekey must show the same) */
    int kx;                               /* 0 none, 1 KEXINIT sent, 2 ECDH_INIT sent, 3 NEWKEYS sent */
    int we_sent_kexinit, got_newkeys, sent_newkeys;
    const char *kex_name;
    enum askq ask;
    char ask_text[800];
    int ask_echo;
    int tried_pk, pw_tries, kbi_tries;
    const char *last_method;
    int kbi_n, kbi_i;                     /* the prompts of an INFO_REQUEST, the one asked */
    uint8_t kbi_buf[2048]; size_t kbi_len;   /* the answers so far */
    struct { char text[256]; int echo; } kbi_p[8];
    uint32_t rchan, rwin, rmaxpkt, lconsumed;
    int chan_open, chan_closed, shell_asked, replies_wanted;
    uint32_t exit_status; int have_exit;
    uint8_t rxb[RXCAP];
    size_t rx_n;
    int vdone;                            /* the version line came */
    int vlines;
    size_t vn;
    uint32_t rx_plen;                     /* the length of the packet in rxb (decrypted), 0 = not known yet */
};
static struct ssh pool[SSH_MAX];
static struct ssh *S;

struct ssh *ssh_new(void) {
    for (int i = 0; i < SSH_MAX; i++) if (!pool[i].used) { memset(&pool[i], 0, sizeof pool[i]); pool[i].used = 1; pool[i].last_method = ""; return &pool[i]; }
    return 0;
}
void ssh_free(struct ssh *s) { if (s) { crypto_wipe(s, sizeof *s); s->used = 0; } }


/* ------------------------------------------------------------------ the queue to the TCP connection */
static size_t oq_free(void) { return OUTQ - (S->oq_w - S->oq_r); }
static void oq_put(const uint8_t *p, size_t n) { while (n--) S->outq[S->oq_w++ % OUTQ] = *p++; }

void ssh_poll(struct ssh *s) {
    S = s;
    while (S->oq_w != S->oq_r) {
        size_t at = S->oq_r % OUTQ, n = S->oq_w - S->oq_r;
        if (n > OUTQ - at) n = OUTQ - at;
        size_t k = S->io->send(S->ctx, S->outq + at, n);
        if (!k) break;
        S->oq_r += k;
    }
}

/* ------------------------------------------------------------------ errors */
static void fail(const char *m) {
    if (S->st == SSH_CLOSED) return;
    snprintf(S->err, sizeof S->err, "%s", m);
    S->st = SSH_CLOSED;
}
enum ssh_state ssh_state(struct ssh *s) { return s->st; }
int ssh_kexes(struct ssh *s) { return s->kex_n; }
int ssh_strict(struct ssh *s) { return s->strict; }
const char *ssh_error(struct ssh *s) { return s->err; }

static void say(const char *s) { S->io->data(S->ctx, (const uint8_t *)s, strlen(s)); }
/* text of the server (banner, instruction): LF -> CR LF, no control bytes but TAB */
static void say_text(const uint8_t *p, size_t n) {
    uint8_t b[256];
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = p[i];
        if (k + 2 > sizeof b) { S->io->data(S->ctx, b, k); k = 0; }
        if (c == '\n') { b[k++] = '\r'; b[k++] = '\n'; }
        else if (c >= 0x20 || c == '\t') b[k++] = c;
    }
    if (k) S->io->data(S->ctx, b, k);
}

/* ------------------------------------------------------------------ the cipher state */

static void nonce_of(uint32_t seq, uint8_t n[8]) { memset(n, 0, 4); n[4] = (uint8_t)(seq >> 24); n[5] = (uint8_t)(seq >> 16); n[6] = (uint8_t)(seq >> 8); n[7] = (uint8_t)seq; }

/* ------------------------------------------------------------------ packets out */
static uint8_t txp[TXP];
static size_t txn;
static int txbad;

static void p_byte(uint8_t b) { if (txn < TXP) txp[txn++] = b; else txbad = 1; }
static void p_u32(uint32_t v) { p_byte((uint8_t)(v >> 24)); p_byte((uint8_t)(v >> 16)); p_byte((uint8_t)(v >> 8)); p_byte((uint8_t)v); }
static void p_raw(const void *p, size_t n) { if (txn + n <= TXP) { memcpy(txp + txn, p, n); txn += n; } else txbad = 1; }
static void p_str(const void *p, size_t n) { p_u32((uint32_t)n); p_raw(p, n); }
static void p_cstr(const char *s) { p_str(s, strlen(s)); }
static void p_begin(uint8_t type) { txn = 0; txbad = 0; p_byte(type); }

static void p_send(void) {
    if (txbad) { fail("internal: packet too big"); return; }
    uint8_t pkt[TXP + 64];
    size_t pad = S->tx.on ? 8 - (1 + txn) % 8 : 8 - (5 + txn) % 8;
    if (pad < 4) pad += 8;
    uint32_t len = (uint32_t)(1 + txn + pad);
    pkt[0] = (uint8_t)(len >> 24); pkt[1] = (uint8_t)(len >> 16); pkt[2] = (uint8_t)(len >> 8); pkt[3] = (uint8_t)len;
    pkt[4] = (uint8_t)pad;
    memcpy(pkt + 5, txp, txn);
    S->io->random(pkt + 5 + txn, pad);
    size_t total = 4 + len;
    if (S->tx.on) {
        uint8_t nonce[8], polykey[32];
        nonce_of(S->tx.seq, nonce);
        crypto_chacha20_djb(pkt, pkt, 4, S->tx.key + 32, nonce, 0);
        crypto_chacha20_djb(polykey, NULL, 32, S->tx.key, nonce, 0);
        crypto_chacha20_djb(pkt + 4, pkt + 4, len, S->tx.key, nonce, 1);
        crypto_poly1305(pkt + total, pkt, total, polykey);
        crypto_wipe(polykey, sizeof polykey);
        total += 16;
    }
    crypto_wipe(txp, txn);                 /* a password may be in it */
    if (oq_free() < total) { fail("the output queue is full"); return; }
    oq_put(pkt, total);
    S->tx.seq++;
}

/* ------------------------------------------------------------------ parsing */
struct rd { const uint8_t *p; size_t n; int bad; };
static uint8_t g_byte(struct rd *r) { if (r->n < 1) { r->bad = 1; return 0; } r->n--; return *r->p++; }
static uint32_t g_u32(struct rd *r) {
    if (r->n < 4) { r->bad = 1; r->n = 0; return 0; }
    uint32_t v = (uint32_t)r->p[0] << 24 | (uint32_t)r->p[1] << 16 | (uint32_t)r->p[2] << 8 | r->p[3];
    r->p += 4; r->n -= 4;
    return v;
}
static const uint8_t *g_str(struct rd *r, size_t *len) {
    uint32_t n = g_u32(r);
    if (r->bad || n > r->n) { r->bad = 1; r->n = 0; *len = 0; return (const uint8_t *)""; }
    const uint8_t *s = r->p;
    r->p += n; r->n -= n;
    *len = n;
    return s;
}
/* a name is in a comma list */
static int in_list(const uint8_t *l, size_t n, const char *name) {
    size_t k = strlen(name), i = 0;
    while (i <= n) {
        size_t j = i;
        while (j < n && l[j] != ',') j++;
        if (j - i == k && !memcmp(l + i, name, k)) return 1;
        i = j + 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ kex */


static void send_kexinit(void) {
    p_begin(MSG_KEXINIT);
    uint8_t cookie[16];
    S->io->random(cookie, 16);
    p_raw(cookie, 16);
    p_cstr(S->kex_n ? "curve25519-sha256,curve25519-sha256@libssh.org"
                 : "curve25519-sha256,curve25519-sha256@libssh.org,kex-strict-c-v00@openssh.com");
    p_cstr("ssh-ed25519");
    p_cstr("chacha20-poly1305@openssh.com");
    p_cstr("chacha20-poly1305@openssh.com");
    p_cstr("hmac-sha2-256,hmac-sha1");
    p_cstr("hmac-sha2-256,hmac-sha1");
    p_cstr("none");
    p_cstr("none");
    p_cstr("");
    p_cstr("");
    p_byte(0);
    p_u32(0);
    S->ic_n = txn;
    memcpy(S->ic, txp, txn);
    p_send();
    S->we_sent_kexinit = 1;
    if (S->kx == 0) S->kx = 1;
}

static void on_kexinit(struct rd *r, const uint8_t *payload, size_t n) {
    if (n > IS_MAX) { fail("the KEXINIT of the server is too big"); return; }
    memcpy(S->is_, payload, n);
    S->is_n = n;
    r->p += 16; if (r->n < 16) { r->bad = 1; return; } r->n -= 16;
    size_t l[10]; const uint8_t *s[10];
    for (int i = 0; i < 10; i++) s[i] = g_str(r, &l[i]);
    if (r->bad) { fail("a bad KEXINIT"); return; }
    if (in_list(s[0], l[0], "curve25519-sha256")) S->kex_name = "curve25519-sha256";
    else if (in_list(s[0], l[0], "curve25519-sha256@libssh.org")) S->kex_name = "curve25519-sha256@libssh.org";
    else { fail("the server has no curve25519-sha256 key exchange"); return; }
    if (!in_list(s[1], l[1], "ssh-ed25519")) { fail("the server has no ssh-ed25519 host key (RSA / ECDSA are not supported)"); return; }
    if (!in_list(s[2], l[2], "chacha20-poly1305@openssh.com") || !in_list(s[3], l[3], "chacha20-poly1305@openssh.com")) {
        fail("the server has no chacha20-poly1305@openssh.com cipher"); return;
    }
    if (!in_list(s[6], l[6], "none") || !in_list(s[7], l[7], "none")) { fail("the server wants compression"); return; }
    if (S->kex_n == 0) S->strict = in_list(s[0], l[0], "kex-strict-s-v00@openssh.com");
    if (!S->we_sent_kexinit) send_kexinit();            /* a rekey of the server */
    /* our ephemeral key */
    S->io->random(S->x_sk, 32);
    crypto_x25519_public_key(S->q_c, S->x_sk);
    p_begin(MSG_KEX_ECDH_INIT);
    p_str(S->q_c, 32);
    p_send();
    S->kx = 2;
}

static void h_str(sha256_ctx *c, const void *p, size_t n) {
    uint8_t l[4] = { (uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n };
    sha256_update(c, l, 4);
    sha256_update(c, p, n);
}
/* RFC 8731: the 32 bytes of X25519 as an unsigned big-endian number, as an mpint */
static void h_mpint(sha256_ctx *c, const uint8_t k[32]) {
    size_t i = 0;
    while (i < 32 && !k[i]) i++;
    uint8_t b[33];
    size_t n = 0;
    if (i < 32 && (k[i] & 0x80)) b[n++] = 0;
    memcpy(b + n, k + i, 32 - i);
    h_str(c, b, n + 32 - i);
}
static void derive(uint8_t out[64], const uint8_t k[32], const uint8_t h[32], char letter) {
    sha256_ctx c;
    sha256_init(&c); h_mpint(&c, k); sha256_update(&c, h, 32); sha256_update(&c, &letter, 1); sha256_update(&c, S->session_id, 32); sha256_final(&c, out);
    sha256_init(&c); h_mpint(&c, k); sha256_update(&c, h, 32); sha256_update(&c, out, 32); sha256_final(&c, out + 32);
}

/* questions */

static void send_newkeys(void) {
    p_begin(MSG_NEWKEYS);
    p_send();
    S->tx = S->tx_next;
    S->tx.on = 1;
    S->tx.seq = S->strict ? 0 : S->tx.seq;
    S->sent_newkeys = 1;
    S->kx = 3;
}

static void kex_done(void);

static void on_ecdh_reply(struct rd *r) {
    if (S->kx != 2) { fail("an unexpected KEX_ECDH_REPLY"); return; }
    size_t ks_n, qs_n, sig_n;
    const uint8_t *ks = g_str(r, &ks_n), *qs = g_str(r, &qs_n), *sig = g_str(r, &sig_n);
    if (r->bad || qs_n != 32) { fail("a bad KEX_ECDH_REPLY"); return; }
    struct rd kr = { ks, ks_n, 0 }, sr = { sig, sig_n, 0 };
    size_t an, pn, sn, bn;
    const uint8_t *alg = g_str(&kr, &an), *pk = g_str(&kr, &pn);
    const uint8_t *salg = g_str(&sr, &sn), *sb = g_str(&sr, &bn);
    if (kr.bad || sr.bad || an != 11 || memcmp(alg, "ssh-ed25519", 11) || pn != 32 || sn != 11 || memcmp(salg, "ssh-ed25519", 11) || bn != 64) {
        fail("the host key is not ssh-ed25519"); return;
    }
    uint8_t k[32], zero[32] = { 0 }, h[32];
    crypto_x25519(k, S->x_sk, qs);
    crypto_wipe(S->x_sk, 32);
    if (!crypto_verify32(k, zero)) { fail("a bad public key of the server"); return; }
    sha256_ctx c;
    sha256_init(&c);
    h_str(&c, VERSION, strlen(VERSION));
    h_str(&c, S->vs, strlen(S->vs));
    h_str(&c, S->ic, S->ic_n);
    h_str(&c, S->is_, S->is_n);
    h_str(&c, ks, ks_n);
    h_str(&c, S->q_c, 32);
    h_str(&c, qs, 32);
    h_mpint(&c, k);
    sha256_final(&c, h);
    if (crypto_ed25519_check(sb, pk, h, 32)) { crypto_wipe(k, 32); fail("the signature of the host is wrong"); return; }
    if (!S->have_session) { memcpy(S->session_id, h, 32); S->have_session = 1; }
    memset(&S->tx_next, 0, sizeof S->tx_next); memset(&S->rx_next, 0, sizeof S->rx_next);
    S->tx_next.seq = S->tx.seq; S->rx_next.seq = S->rx.seq;
    derive(S->tx_next.key, k, h, 'C');
    derive(S->rx_next.key, k, h, 'D');
    crypto_wipe(k, 32);
    if (S->kex_n) {                                     /* a rekey: the same host */
        if (crypto_verify32(pk, S->host_pk)) { fail("the host key changed in a rekey"); return; }
        send_newkeys();
        return;
    }
    memcpy(S->host_pk, pk, 32);
    char fp[52];
    ssh_fingerprint(pk, fp);
    enum ssh_hk known = S->io->hostkey(S->host, S->port, pk, 0);
    if (known == SSH_HK_OK) { send_newkeys(); return; }
    if (known == SSH_HK_NEW) {
        snprintf(S->ask_text, sizeof S->ask_text,
                 "The authenticity of host '%s' (port %d) can't be established.\r\n"
                 "ED25519 key fingerprint is %s.\r\n"
                 "Are you sure you want to continue connecting (yes/no)? ", S->host, S->port, fp);
        S->ask = A_HOST_NEW;
    } else {
        snprintf(S->ask_text, sizeof S->ask_text,
                 "\x1b[1;31m@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\r\n"
                 "@    WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!     @\r\n"
                 "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\x1b[0m\r\n"
                 "IT IS POSSIBLE THAT SOMEONE IS DOING SOMETHING NASTY!\r\n"
                 "Someone could be eavesdropping on you right now (man-in-the-middle attack)!\r\n"
                 "It is also possible that the host key of '%s' (port %d) has just been changed.\r\n"
                 "The new ED25519 key fingerprint is %s.\r\n"
                 "Type \"replace\" to trust the new key, anything else disconnects: ", S->host, S->port, fp);
        S->ask = A_HOST_CHANGED;
    }
    S->ask_echo = 1;
}

static void on_newkeys(void) {
    if (S->kx < 2 || S->got_newkeys) { fail("an unexpected NEWKEYS"); return; }
    S->rx = S->rx_next;
    S->rx.on = 1;
    if (S->strict) S->rx.seq = 0;
    S->got_newkeys = 1;
    if (S->sent_newkeys) kex_done();
}

/* ------------------------------------------------------------------ auth */

static void send_service_request(void) { p_begin(MSG_SERVICE_REQUEST); p_cstr("ssh-userauth"); p_send(); }

static void auth_head(const char *method) {
    p_begin(MSG_USERAUTH_REQUEST);
    p_cstr(S->user);
    p_cstr("ssh-connection");
    p_cstr(method);
    S->last_method = method;
}

static void auth_none(void) { auth_head("none"); p_send(); }

static void auth_pk(const uint8_t *sk) {
    uint8_t blob[51];
    memcpy(blob, "\0\0\0\x0bssh-ed25519\0\0\0\x20", 19);
    memcpy(blob + 19, sk + 32, 32);
    /* the signed data: session id, then the request up to the signature */
    uint8_t d[400];
    size_t n = 0;
#define DSTR(p, l) do { size_t _l = (l); d[n] = (uint8_t)(_l >> 24); d[n + 1] = (uint8_t)(_l >> 16); d[n + 2] = (uint8_t)(_l >> 8); d[n + 3] = (uint8_t)_l; memcpy(d + n + 4, (p), _l); n += 4 + _l; } while (0)
    DSTR(S->session_id, 32);
    d[n++] = MSG_USERAUTH_REQUEST;
    DSTR(S->user, strlen(S->user));
    DSTR("ssh-connection", 14);
    DSTR("publickey", 9);
    d[n++] = 1;
    DSTR("ssh-ed25519", 11);
    DSTR(blob, 51);
#undef DSTR
    uint8_t sig[64], sigblob[83];
    crypto_ed25519_sign(sig, sk, d, n);
    memcpy(sigblob, "\0\0\0\x0bssh-ed25519\0\0\0\x40", 19);
    memcpy(sigblob + 19, sig, 64);
    auth_head("publickey");
    p_byte(1);
    p_cstr("ssh-ed25519");
    p_str(blob, 51);
    p_str(sigblob, 83);
    p_send();
}

static void ask_password(void) {
    snprintf(S->ask_text, sizeof S->ask_text, "%s@%s's password: ", S->user, S->host);
    S->ask = A_PASS;
    S->ask_echo = 0;
}

/* the next method after a failure (or the first after "none") */
static void auth_next(const uint8_t *m, size_t mn) {
    const uint8_t *sk = S->io->user_key ? S->io->user_key() : 0;
    if (!S->tried_pk && sk && in_list(m, mn, "publickey")) { S->tried_pk = 1; auth_pk(sk); return; }
    if (S->kbi_tries < 3 && in_list(m, mn, "keyboard-interactive")) {
        S->kbi_tries++;
        auth_head("keyboard-interactive");
        p_cstr(""); p_cstr("");
        p_send();
        return;
    }
    if (S->pw_tries < 3 && in_list(m, mn, "password")) { S->pw_tries++; ask_password(); return; }
    char b[128];
    snprintf(b, sizeof b, "Permission denied (%.*s)", (int)(mn > 90 ? 90 : mn), (const char *)m);
    fail(b);
}

static void kbi_ask_next(void) {
    snprintf(S->ask_text, sizeof S->ask_text, "%s", S->kbi_p[S->kbi_i].text);
    S->ask = A_KBI;
    S->ask_echo = S->kbi_p[S->kbi_i].echo;
}

static void kbi_send(void) {
    p_begin(MSG_USERAUTH_INFO_RESPONSE);
    p_u32((uint32_t)S->kbi_n);
    p_raw(S->kbi_buf, S->kbi_len);
    p_send();
    crypto_wipe(S->kbi_buf, sizeof S->kbi_buf);
    S->kbi_len = 0;
}

static void on_info_request(struct rd *r) {
    size_t nn, in_, ln;
    const uint8_t *name = g_str(r, &nn), *ins = g_str(r, &in_);
    g_str(r, &ln);
    uint32_t num = g_u32(r);
    if (r->bad || num > 8) { fail("a bad keyboard-interactive request"); return; }
    if (nn) { say_text(name, nn); say("\r\n"); }
    if (in_) { say_text(ins, in_); say("\r\n"); }
    S->kbi_n = (int)num;
    for (int i = 0; i < S->kbi_n; i++) {
        size_t pn;
        const uint8_t *p = g_str(r, &pn);
        S->kbi_p[i].echo = g_byte(r);
        size_t k = 0;
        for (size_t j = 0; j < pn && k < sizeof S->kbi_p[i].text - 1; j++) if (p[j] >= 0x20) S->kbi_p[i].text[k++] = (char)p[j];
        S->kbi_p[i].text[k] = 0;
    }
    if (r->bad) { fail("a bad keyboard-interactive request"); return; }
    S->kbi_len = 0;
    S->kbi_i = 0;
    if (!S->kbi_n) kbi_send();
    else kbi_ask_next();
}

/* ------------------------------------------------------------------ the channel */

static void open_channel(void) {
    p_begin(MSG_CHANNEL_OPEN);
    p_cstr("session");
    p_u32(0);
    p_u32(SSH_LWIN);
    p_u32(LMAXPKT);
    p_send();
}

static void on_open_confirmation(struct rd *r) {
    g_u32(r);
    S->rchan = g_u32(r);
    S->rwin = g_u32(r);
    S->rmaxpkt = g_u32(r);
    if (r->bad) { fail("a bad CHANNEL_OPEN_CONFIRMATION"); return; }
    S->chan_open = 1;
    p_begin(MSG_CHANNEL_REQUEST);
    p_u32(S->rchan);
    p_cstr("pty-req");
    p_byte(1);
    p_cstr("xterm-256color");
    p_u32((uint32_t)S->cols); p_u32((uint32_t)S->rows);
    p_u32((uint32_t)S->cols * 8); p_u32((uint32_t)S->rows * 16);
    /* the modes: VERASE = DEL (what libvterm sends for Backspace), IUTF8 */
    static const uint8_t modes[] = { 3, 0, 0, 0, 0x7f, 42, 0, 0, 0, 1, 0 };
    p_str(modes, sizeof modes);
    p_send();
    p_begin(MSG_CHANNEL_REQUEST);
    p_u32(S->rchan);
    p_cstr("shell");
    p_byte(1);
    p_send();
    S->replies_wanted = 2;
    S->shell_asked = 1;
}

static void on_channel_reply(int ok) {
    if (!S->replies_wanted) return;
    S->replies_wanted--;
    if (S->replies_wanted == 1 && !ok) say("\r\n[the server gave no terminal (pty-req failed)]\r\n");
    if (S->replies_wanted == 0) {
        if (!ok) { fail("the server refused the shell"); return; }
        S->st = SSH_OPEN;
    }
}

static void on_data(struct rd *r, int ext) {
    g_u32(r);
    if (ext) g_u32(r);
    size_t n;
    const uint8_t *d = g_str(r, &n);
    if (r->bad) { fail("a bad CHANNEL_DATA"); return; }
    S->io->data(S->ctx, d, n);
    S->lconsumed += (uint32_t)n;
    if (S->lconsumed >= SSH_LWIN / 2 && S->kx == 0) {
        p_begin(MSG_CHANNEL_WINDOW_ADJUST);
        p_u32(S->rchan);
        p_u32(S->lconsumed);
        p_send();
        S->lconsumed = 0;
    }
}


static void on_channel_request(struct rd *r) {
    g_u32(r);
    size_t tn;
    const uint8_t *t = g_str(r, &tn);
    int want = g_byte(r);
    if (r->bad) { fail("a bad CHANNEL_REQUEST"); return; }
    if (tn == 11 && !memcmp(t, "exit-status", 11)) { S->exit_status = g_u32(r); S->have_exit = !r->bad; }
    if (want) { p_begin(MSG_CHANNEL_FAILURE); p_u32(S->rchan); p_send(); }
}

static void on_close(void) {
    if (!S->chan_closed) { p_begin(MSG_CHANNEL_CLOSE); p_u32(S->rchan); p_send(); S->chan_closed = 1; }
    p_begin(MSG_DISCONNECT);
    p_u32(11);
    p_cstr("bye");
    p_cstr("");
    p_send();
    char b[64];
    if (S->have_exit) snprintf(b, sizeof b, "the shell exited (status %u)", (unsigned)S->exit_status);
    else snprintf(b, sizeof b, "the channel closed");
    fail(b);
}

size_t ssh_write(struct ssh *s, const uint8_t *p, size_t n) {
    S = s;
    if (S->st != SSH_OPEN || S->kx || S->chan_closed) return 0;
    size_t k = n;
    if (k > S->rwin) k = S->rwin;
    if (k > S->rmaxpkt) k = S->rmaxpkt;
    if (k > 4096) k = 4096;
    if (!k || oq_free() < k + 64) return 0;
    p_begin(MSG_CHANNEL_DATA);
    p_u32(S->rchan);
    p_str(p, k);
    p_send();
    S->rwin -= (uint32_t)k;
    return k;
}

/* ------------------------------------------------------------------ the packet loop */
static void kex_done(void) {
    S->kx = 0;
    S->kex_n++;
    S->we_sent_kexinit = S->got_newkeys = S->sent_newkeys = 0;
    if (S->kex_n == 1) { S->st = SSH_AUTH; send_service_request(); }
}

static void handle(const uint8_t *payload, size_t n, uint32_t seq) {
    struct rd r = { payload + 1, n - 1, 0 };
    uint8_t t = payload[0];
    /* strict kex: only the kex itself until the first NEWKEYS */
    if (S->kex_n == 0 && S->strict && t != MSG_KEXINIT && t != MSG_KEX_ECDH_REPLY && t != MSG_NEWKEYS) { fail("an unexpected message in the key exchange"); return; }
    if (S->kex_n == 0 && S->kx < 2 && t != MSG_KEXINIT && t != MSG_IGNORE && t != MSG_DEBUG && t != MSG_DISCONNECT) { fail("the server did not start with KEXINIT"); return; }
    /* during a kex: transport messages only */
    if (S->kx >= 2 && t >= 50) { fail("a message of another layer in the key exchange"); return; }
    switch (t) {
    case MSG_DISCONNECT: {
        g_u32(&r);
        size_t dn;
        const uint8_t *d = g_str(&r, &dn);
        char b[128];
        snprintf(b, sizeof b, "the server disconnected: %.*s", (int)(dn > 90 ? 90 : dn), (const char *)d);
        fail(b);
        return;
    }
    case MSG_IGNORE: case MSG_DEBUG: case MSG_UNIMPLEMENTED: case MSG_EXT_INFO: return;
    case MSG_KEXINIT:
        if (S->kx >= 2) { fail("a second KEXINIT"); return; }
        on_kexinit(&r, payload, n);
        return;
    case MSG_KEX_ECDH_REPLY: on_ecdh_reply(&r); return;
    case MSG_NEWKEYS: on_newkeys(); return;
    case MSG_SERVICE_ACCEPT:
        if (!S->user[0]) { snprintf(S->ask_text, sizeof S->ask_text, "login as: "); S->ask = A_USER; S->ask_echo = 1; }
        else auth_none();
        return;
    case MSG_USERAUTH_BANNER: {
        size_t bn;
        const uint8_t *b = g_str(&r, &bn);
        say_text(b, bn);
        return;
    }
    case MSG_USERAUTH_FAILURE: {
        size_t mn;
        const uint8_t *m = g_str(&r, &mn);
        if (r.bad) { fail("a bad USERAUTH_FAILURE"); return; }
        if (strcmp(S->last_method, "none") && strcmp(S->last_method, "keyboard-interactive") && strcmp(S->last_method, "publickey")) say("Permission denied, please try again.\r\n");
        auth_next(m, mn);
        return;
    }
    case MSG_USERAUTH_SUCCESS:
        open_channel();
        return;
    case MSG_USERAUTH_60:
        if (!strcmp(S->last_method, "keyboard-interactive")) on_info_request(&r);
        else if (!strcmp(S->last_method, "password")) fail("the password has expired: change it on another computer");
        return;
    case MSG_GLOBAL_REQUEST: {
        size_t gn;
        g_str(&r, &gn);
        if (g_byte(&r) && !r.bad) { p_begin(MSG_REQUEST_FAILURE); p_send(); }
        return;
    }
    case MSG_REQUEST_SUCCESS: case MSG_REQUEST_FAILURE: return;
    case MSG_CHANNEL_OPEN: {
        size_t cn;
        g_str(&r, &cn);
        uint32_t sender = g_u32(&r);
        p_begin(MSG_CHANNEL_OPEN_FAILURE); p_u32(sender); p_u32(1); p_cstr("no"); p_cstr(""); p_send();
        return;
    }
    case MSG_CHANNEL_OPEN_CONFIRMATION: on_open_confirmation(&r); return;
    case MSG_CHANNEL_OPEN_FAILURE: fail("the server refused the session channel"); return;
    case MSG_CHANNEL_WINDOW_ADJUST: g_u32(&r); { uint32_t a = g_u32(&r); if (!r.bad) S->rwin += a; } return;
    case MSG_CHANNEL_DATA: on_data(&r, 0); return;
    case MSG_CHANNEL_EXTENDED_DATA: on_data(&r, 1); return;
    case MSG_CHANNEL_EOF: return;
    case MSG_CHANNEL_CLOSE: on_close(); return;
    case MSG_CHANNEL_REQUEST: on_channel_request(&r); return;
    case MSG_CHANNEL_SUCCESS: on_channel_reply(1); return;
    case MSG_CHANNEL_FAILURE: on_channel_reply(0); return;
    default:
        p_begin(MSG_UNIMPLEMENTED);
        p_u32(seq);
        p_send();
        return;
    }
}


/* the packets in rxb, until a question stops it */
static void process(void) {
    while (S->st != SSH_CLOSED && S->ask == A_NONE) {
        if (S->rx_n < 4) return;
        if (!S->rx_plen) {
            uint8_t l[4];
            if (S->rx.on) {
                uint8_t nonce[8];
                nonce_of(S->rx.seq, nonce);
                crypto_chacha20_djb(l, S->rxb, 4, S->rx.key + 32, nonce, 0);
            } else memcpy(l, S->rxb, 4);
            uint32_t len = (uint32_t)l[0] << 24 | (uint32_t)l[1] << 16 | (uint32_t)l[2] << 8 | l[3];
            if (len < 8 || len > RXCAP - 4 - 16 || (S->rx.on ? len : len + 4) % 8) { fail("a bad packet length (wrong keys?)"); return; }
            S->rx_plen = len;
        }
        size_t total = 4 + S->rx_plen + (S->rx.on ? 16 : 0);
        if (S->rx_n < total) return;
        if (S->rx.on) {
            uint8_t nonce[8], polykey[32], mac[16];
            nonce_of(S->rx.seq, nonce);
            crypto_chacha20_djb(polykey, NULL, 32, S->rx.key, nonce, 0);
            crypto_poly1305(mac, S->rxb, 4 + S->rx_plen, polykey);
            crypto_wipe(polykey, sizeof polykey);
            if (crypto_verify16(mac, S->rxb + 4 + S->rx_plen)) { fail("a packet with a bad MAC"); return; }
            crypto_chacha20_djb(S->rxb + 4, S->rxb + 4, S->rx_plen, S->rx.key, nonce, 1);
        }
        uint32_t pad = S->rxb[4];
        if (pad < 4 || pad + 1 >= S->rx_plen) { fail("a bad packet padding"); return; }
        uint32_t seq = S->rx.seq++;
        /* a copy: handle() may send packets, the buffer moves after it */
        static uint8_t pl[RXCAP];            /* shared by the sessions: handle() never runs another session */
        size_t n = S->rx_plen - pad - 1;
        memcpy(pl, S->rxb + 5, n);
        memmove(S->rxb, S->rxb + total, S->rx_n - total);
        S->rx_n -= total;
        S->rx_plen = 0;
        handle(pl, n, seq);
        crypto_wipe(pl, n);
    }
}

void ssh_in(struct ssh *s, const uint8_t *p, size_t n) {
    S = s;
    if (S->st == SSH_OFF || S->st == SSH_CLOSED) return;
    while (!S->vdone && n) {
        uint8_t c = *p++; n--;
        if (c == '\n') {
            if (S->vn && S->vs[S->vn - 1] == '\r') S->vn--;
            S->vs[S->vn] = 0;
            if (!strncmp(S->vs, "SSH-", 4)) {
                if (strncmp(S->vs, "SSH-2.0-", 8) && strncmp(S->vs, "SSH-1.99-", 9)) { fail("the server speaks SSH 1 only"); return; }
                S->vdone = 1;
            } else if (++S->vlines > 50) { fail("not an SSH server"); return; }
            S->vn = 0;
            continue;
        }
        if (S->vn < sizeof S->vs - 1) S->vs[S->vn++] = (char)c;
        else { fail("not an SSH server"); return; }
    }
    if (!n) return;
    if (S->rx_n + n > RXCAP) { fail("the input buffer is full"); return; }
    memcpy(S->rxb + S->rx_n, p, n);
    S->rx_n += n;
    process();
}

void ssh_start(struct ssh *s, const struct ssh_io *io_, void *ctx, const char *h, int p, const char *u, int c, int r) {
    S = s;
    S->io = io_;
    S->ctx = ctx;
    memset(&S->tx, 0, sizeof S->tx); memset(&S->rx, 0, sizeof S->rx);
    S->oq_r = S->oq_w = 0;
    S->rx_n = 0; S->rx_plen = 0; S->vdone = 0; S->vlines = 0; S->vn = 0;
    S->have_session = 0; S->strict = 0; S->kex_n = 0; S->kx = 0; S->we_sent_kexinit = S->got_newkeys = S->sent_newkeys = 0;
    S->tried_pk = S->pw_tries = S->kbi_tries = 0; S->last_method = "";
    S->chan_open = S->chan_closed = S->shell_asked = S->replies_wanted = 0; S->lconsumed = 0; S->have_exit = 0;
    S->ask = A_NONE;
    S->err[0] = 0;
    snprintf(S->host, sizeof S->host, "%s", h);
    snprintf(S->user, sizeof S->user, "%s", u ? u : "");
    S->port = p; S->cols = c; S->rows = r;
    S->st = SSH_HANDSHAKE;
    oq_put((const uint8_t *)VERSION "\r\n", strlen(VERSION) + 2);
    send_kexinit();
    ssh_poll(S);
}

void ssh_close(struct ssh *s) {
    S = s;
    if (S->st == SSH_OFF || S->st == SSH_CLOSED) return;
    p_begin(MSG_DISCONNECT);
    p_u32(11);
    p_cstr("the user closed the session");
    p_cstr("");
    p_send();
    ssh_poll(S);
    fail("closed");
}

int ssh_ask(struct ssh *s, const char **prompt, int *echo) {
    S = s;
    if (S->ask == A_NONE || S->st == SSH_CLOSED) return 0;
    *prompt = S->ask_text;
    *echo = S->ask_echo;
    return 1;
}

void ssh_answer(struct ssh *s, const char *line) {
    S = s;
    enum askq a = S->ask;
    S->ask = A_NONE;
    switch (a) {
    case A_HOST_NEW:
        if (strcmp(line, "yes")) { fail("Host key verification failed"); break; }
        S->io->hostkey(S->host, S->port, S->host_pk, 1);
        { char b[160]; snprintf(b, sizeof b, "Warning: Permanently added '%s' (ED25519) to the list of known hosts.\r\n", S->host); say(b); }
        send_newkeys();
        break;
    case A_HOST_CHANGED:
        if (strcmp(line, "replace")) { fail("Host key verification failed"); break; }
        S->io->hostkey(S->host, S->port, S->host_pk, 1);
        say("The new host key is stored.\r\n");
        send_newkeys();
        break;
    case A_USER:
        snprintf(S->user, sizeof S->user, "%s", line);
        if (!S->user[0]) { fail("no user name"); break; }
        auth_none();
        break;
    case A_PASS:
        auth_head("password");
        p_byte(0);
        p_cstr(line);
        p_send();
        break;
    case A_KBI: {
        size_t l = strlen(line);
        if (S->kbi_len + 4 + l > sizeof S->kbi_buf) { fail("the answer is too long"); break; }
        S->kbi_buf[S->kbi_len] = (uint8_t)(l >> 24); S->kbi_buf[S->kbi_len + 1] = (uint8_t)(l >> 16); S->kbi_buf[S->kbi_len + 2] = (uint8_t)(l >> 8); S->kbi_buf[S->kbi_len + 3] = (uint8_t)l;
        memcpy(S->kbi_buf + S->kbi_len + 4, line, l);
        S->kbi_len += 4 + l;
        if (++S->kbi_i < S->kbi_n) kbi_ask_next();
        else kbi_send();
        break;
    }
    default: break;
    }
    if (S->st == SSH_HANDSHAKE && S->got_newkeys && S->sent_newkeys) kex_done();
    process();
    ssh_poll(S);
}

/* ------------------------------------------------------------------ keys as text */
static size_t b64(const uint8_t *p, size_t n, char *out, int pad) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t k = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16 | (i + 1 < n ? (uint32_t)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        out[k++] = t[v >> 18 & 63];
        out[k++] = t[v >> 12 & 63];
        if (i + 1 < n) out[k++] = t[v >> 6 & 63]; else if (pad) out[k++] = '=';
        if (i + 2 < n) out[k++] = t[v & 63]; else if (pad) out[k++] = '=';
    }
    out[k] = 0;
    return k;
}

static void key_blob(const uint8_t key[32], uint8_t blob[51]) {
    memcpy(blob, "\0\0\0\x0bssh-ed25519\0\0\0\x20", 19);
    memcpy(blob + 19, key, 32);
}

void ssh_fingerprint(const uint8_t key[32], char out[52]) {
    uint8_t blob[51], h[32];
    key_blob(key, blob);
    sha256(h, blob, sizeof blob);
    memcpy(out, "SHA256:", 7);
    b64(h, 32, out + 7, 0);
}

void ssh_pubkey_line(const uint8_t key[32], const char *comment, char *out, size_t size) {
    uint8_t blob[51];
    char b[72];
    key_blob(key, blob);
    b64(blob, sizeof blob, b, 1);
    snprintf(out, size, "ssh-ed25519 %s%s%s", b, comment && comment[0] ? " " : "", comment ? comment : "");
}

/* Ф3 host test of ssh.c (make build-host/test_ssh): the client over a real TCP socket of this PC against a
   test server (src/tools/ssh_test_server.py) or any sshd.
   usage: test_ssh host port user [options]
     -a ANSWER   the next answer to a question of ssh.c (host key "yes", password, ...); in order
     -s TEXT     sent to the shell once it is open (\n and \r escapes); several: one after another
     -k FILE     the user key: 32 bytes seed (made from /dev/urandom if the file is missing)
     -K FILE     known_hosts of this test: host port hex lines
     -p          print the public key line of -k and quit
     -t SECONDS  give up after this (default 20)
   The session output goes to stdout; the end: "END <state> <error>" on stderr; exit 0 if the shell exited
   with status 0. */
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "ssh.h"
#include "monocypher.h"
#include "monocypher-ed25519.h"

static int sock = -1;
static const char *answers[16]; static int na, ia;
static const char *sends[16]; static int ns, is;
static const char *known_file;
static uint8_t ukey[64]; static int have_ukey;

static size_t tx(const uint8_t *p, size_t n) {
    ssize_t k = send(sock, p, n, MSG_NOSIGNAL | MSG_DONTWAIT);
    return k > 0 ? (size_t)k : 0;
}
static void rnd(uint8_t *p, size_t n) {
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f || fread(p, 1, n, f) != n) { fprintf(stderr, "no /dev/urandom\n"); exit(2); }
    fclose(f);
}
static void out(const uint8_t *p, size_t n) { fwrite(p, 1, n, stdout); fflush(stdout); }

static enum ssh_hk hk(const char *host, int port, const uint8_t key[32], int store) {
    char hex[65];
    for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", key[i]);
    if (!known_file) return store ? SSH_HK_OK : SSH_HK_NEW;
    if (store) {
        FILE *f = fopen(known_file, "a");
        if (f) { fprintf(f, "%s %d %s\n", host, port, hex); fclose(f); }
        return SSH_HK_OK;
    }
    FILE *f = fopen(known_file, "r");
    if (!f) return SSH_HK_NEW;
    char h[80], k[80]; int p;
    enum ssh_hk r = SSH_HK_NEW;
    while (fscanf(f, "%79s %d %79s", h, &p, k) == 3)
        if (!strcmp(h, host) && p == port) r = strcmp(k, hex) ? SSH_HK_CHANGED : SSH_HK_OK;   /* the last line wins */
    fclose(f);
    return r;
}
static const uint8_t *uk(void) { return have_ukey ? ukey : 0; }

static void load_key(const char *file) {
    uint8_t seed[32];
    FILE *f = fopen(file, "rb");
    if (f && fread(seed, 1, 32, f) == 32) fclose(f);
    else {
        if (f) fclose(f);
        rnd(seed, 32);
        f = fopen(file, "wb");
        if (!f) { perror(file); exit(2); }
        fwrite(seed, 1, 32, f);
        fclose(f);
    }
    uint8_t pk[32];
    crypto_ed25519_key_pair(ukey, pk, seed);
    have_ukey = 1;
}

static char *unescape(const char *s) {
    char *o = malloc(strlen(s) + 1), *q = o;
    for (; *s; s++) {
        if (*s == '\\' && s[1] == 'n') { *q++ = '\n'; s++; }
        else if (*s == '\\' && s[1] == 'r') { *q++ = '\r'; s++; }
        else *q++ = *s;
    }
    *q = 0;
    return o;
}

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: test_ssh host port user [-a answer] [-s text] [-k key] [-K known] [-p] [-t s]\n"); return 2; }
    const char *host = argv[1], *user = argv[3];
    int port = atoi(argv[2]), print_pk = 0;
    double timeout = 20;
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "-a") && i + 1 < argc) answers[na++] = argv[++i];
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) sends[ns++] = unescape(argv[++i]);
        else if (!strcmp(argv[i], "-k") && i + 1 < argc) load_key(argv[++i]);
        else if (!strcmp(argv[i], "-K") && i + 1 < argc) known_file = argv[++i];
        else if (!strcmp(argv[i], "-p")) print_pk = 1;
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) timeout = atof(argv[++i]);
        else { fprintf(stderr, "bad option %s\n", argv[i]); return 2; }
    }
    if (print_pk) {
        if (!have_ukey) return 2;
        char line[160];
        ssh_pubkey_line(ukey + 32, "test@karabas", line, sizeof line);
        puts(line);
        return 0;
    }
    struct addrinfo hints = { 0 }, *res;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    char ps[16]; snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &res)) { fprintf(stderr, "no host\n"); return 2; }
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(sock, res->ai_addr, res->ai_addrlen)) { perror("connect"); return 2; }
    freeaddrinfo(res);
    static const struct ssh_io io = { tx, rnd, out, hk, uk };
    ssh_start(&io, host, port, user, 128, 48);
    double t0 = now(), t_open = 0;
    const char *cur = 0; size_t cur_left = 0;
    while (ssh_state() != SSH_CLOSED) {
        if (now() - t0 > timeout) { fprintf(stderr, "END timeout\n"); return 1; }
        const char *prompt; int echo;
        if (ssh_ask(&prompt, &echo)) {
            fprintf(stdout, "%s", prompt);
            if (ia >= na) { fprintf(stderr, "\nEND no answer for: %s\n", prompt); return 1; }
            fprintf(stdout, "%s\n", echo ? answers[ia] : "********");
            ssh_answer(answers[ia++]);
            continue;
        }
        if (ssh_state() == SSH_OPEN) {
            if (!t_open) t_open = now();
            if (!cur_left && is < ns && now() - t_open > 0.3 * (is + 1)) { cur = sends[is++]; cur_left = strlen(cur); }
            if (cur_left) { size_t k = ssh_write((const uint8_t *)cur, cur_left); cur += k; cur_left -= k; }
        }
        ssh_poll();
        struct pollfd pf = { sock, POLLIN, 0 };
        if (poll(&pf, 1, 10) > 0) {
            uint8_t b[256];       /* the ESP gives 256-byte pieces too */
            ssize_t k = recv(sock, b, sizeof b, 0);
            if (k <= 0) { ssh_in(b, 0); fprintf(stderr, "\nEND tcp closed: %s\n", ssh_error()); return ssh_state() == SSH_CLOSED ? 0 : 1; }
            ssh_in(b, (size_t)k);
        }
    }
    ssh_poll();
    fprintf(stderr, "\nEND closed: %s (kexes %d strict %d)\n", ssh_error(), ssh_kexes(), ssh_strict());
    return strstr(ssh_error(), "status 0)") ? 0 : 1;
}

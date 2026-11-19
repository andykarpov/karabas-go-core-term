/* A minimal SSH-2 client (docs/PLAN.md Ф3) on Monocypher: no MMIO here, the user gives the I/O (struct ssh_io),
   so the same code runs on the board (ESP TCP) and on the host (src/fw/test_ssh.c against a server on the PC).
     kex      curve25519-sha256 (+ @libssh.org), strict kex (kex-strict-c-v00@openssh.com: the Terrapin fix)
     host key ssh-ed25519, ecdsa-sha2-nistp256 / 384 / 521, rsa-sha2-512 / 256 (pk.c; docs/PLAN.md Ф6)
     cipher   chacha20-poly1305@openssh.com only (the MAC names in KEXINIT are a formality)
     auth     publickey (ed25519), keyboard-interactive, password; "none" first for the method list
     channel  one session: pty-req xterm-256color, shell; window adjust both ways; rekey by the server
   Several sessions at once (docs/PLAN.md Ф5, a tab each): struct ssh from ssh_new(), SSH_MAX of them.
   Questions to the user (host key, user name, password, keyboard-interactive prompts) come as ssh_ask():
   the caller shows the prompt, reads a line and gives it with ssh_answer(). */
#pragma once
#include <stdint.h>
#include <stddef.h>

enum ssh_hk { SSH_HK_NEW, SSH_HK_OK, SSH_HK_CHANGED };
/* the types of host keys (known_hosts keeps them: the numbers are on the SD card) */
enum ssh_kt { SSH_KT_ED25519, SSH_KT_P256, SSH_KT_P384, SSH_KT_P521, SSH_KT_RSA, SSH_KT_N };
const char *ssh_kt_name(int kt);                   /* "ED25519", "ECDSA", "RSA" (as OpenSSH shows them) */

#ifndef SSH_MAX
#define SSH_MAX 4
#endif

struct ssh;                                         /* a session */

/* ctx: given to ssh_start, back to send / data */
struct ssh_io {
    size_t (*send)(void *ctx, const uint8_t *p, size_t n);     /* to the TCP connection: the bytes taken */
    void (*random)(uint8_t *p, size_t n);           /* a CSPRNG */
    void (*data)(void *ctx, const uint8_t *p, size_t n);       /* text for the screen: the session and our messages */
    /* known_hosts: look up (store 0) or remember (store 1) the host key of host:port: its type and the SHA-256
       of its blob (as an OpenSSH fingerprint) */
    enum ssh_hk (*hostkey)(const char *host, int port, int kt, const uint8_t hash[32], int store);
    /* the ed25519 key of the user (Monocypher layout: seed 32 + public 32), or 0 */
    const uint8_t *(*user_key)(void);
    /* the key type known for host:port, -1 none (offered first: a known host keeps its type); may be 0 */
    int (*hostkey_type)(const char *host, int port);
    /* the host key algorithms we may offer (a comma list, our order kept), 0 = all (the tests narrow it) */
    const char *hostkey_algs;
};

enum ssh_state { SSH_OFF, SSH_HANDSHAKE, SSH_AUTH, SSH_OPEN, SSH_CLOSED };

struct ssh *ssh_new(void);                     /* 0 if all SSH_MAX are in use */
void ssh_free(struct ssh *s);                  /* wiped */
void ssh_start(struct ssh *s, const struct ssh_io *io, void *ctx, const char *host, int port, const char *user,
               int cols, int rows);
void ssh_in(struct ssh *s, const uint8_t *p, size_t n);      /* bytes of the TCP connection */
void ssh_poll(struct ssh *s);                  /* sends the queued bytes: call often */
size_t ssh_write(struct ssh *s, const uint8_t *p, size_t n); /* keys of the user to the shell: the bytes taken */
enum ssh_state ssh_state(struct ssh *s);
const char *ssh_error(struct ssh *s);          /* why it closed, "" if it did not */
void ssh_close(struct ssh *s);                 /* a polite DISCONNECT, then SSH_CLOSED */
int ssh_kexes(struct ssh *s);                  /* key exchanges done (1 + rekeys) */
int ssh_strict(struct ssh *s);                 /* strict kex agreed with the server */

/* a question: 0 none; else *prompt is the text, *echo 0 for secrets */
int ssh_ask(struct ssh *s, const char **prompt, int *echo);
void ssh_answer(struct ssh *s, const char *line);    /* wiped after use; the caller wipes its copy */

/* "SHA256:..." of an ed25519 public key (OpenSSH style) */
void ssh_fingerprint(const uint8_t key[32], char out[52]);
/* "SHA256:..." of the SHA-256 of a key blob */
void ssh_fingerprint_hash(const uint8_t hash[32], char out[52]);
/* "ssh-ed25519 AAAA... comment" of a public key */
void ssh_pubkey_line(const uint8_t key[32], const char *comment, char *out, size_t size);

/* A minimal SSH-2 client (docs/PLAN.md Ф3) on Monocypher: no MMIO here, the user gives the I/O (struct ssh_io),
   so the same code runs on the board (ESP TCP) and on the host (src/fw/test_ssh.c against a server on the PC).
     kex      curve25519-sha256 (+ @libssh.org), strict kex (kex-strict-c-v00@openssh.com: the Terrapin fix)
     host key ssh-ed25519 only
     cipher   chacha20-poly1305@openssh.com only (the MAC names in KEXINIT are a formality)
     auth     publickey (ed25519), keyboard-interactive, password; "none" first for the method list
     channel  one session: pty-req xterm-256color, shell; window adjust both ways; rekey by the server
   Questions to the user (host key, user name, password, keyboard-interactive prompts) come as ssh_ask():
   the caller shows the prompt, reads a line and gives it with ssh_answer(). */
#pragma once
#include <stdint.h>
#include <stddef.h>

enum ssh_hk { SSH_HK_NEW, SSH_HK_OK, SSH_HK_CHANGED };

struct ssh_io {
    size_t (*send)(const uint8_t *p, size_t n);     /* to the TCP connection: the bytes taken */
    void (*random)(uint8_t *p, size_t n);           /* a CSPRNG */
    void (*data)(const uint8_t *p, size_t n);       /* text for the screen: the session and our messages */
    /* known_hosts: look up (store 0) or remember (store 1) the ed25519 key of host:port */
    enum ssh_hk (*hostkey)(const char *host, int port, const uint8_t key[32], int store);
    /* the ed25519 key of the user (Monocypher layout: seed 32 + public 32), or 0 */
    const uint8_t *(*user_key)(void);
};

enum ssh_state { SSH_OFF, SSH_HANDSHAKE, SSH_AUTH, SSH_OPEN, SSH_CLOSED };

void ssh_start(const struct ssh_io *io, const char *host, int port, const char *user, int cols, int rows);
void ssh_in(const uint8_t *p, size_t n);      /* bytes of the TCP connection */
void ssh_poll(void);                           /* sends the queued bytes: call often */
size_t ssh_write(const uint8_t *p, size_t n); /* keys of the user to the shell: the bytes taken */
enum ssh_state ssh_state(void);
const char *ssh_error(void);                   /* why it closed, "" if it did not */
void ssh_close(void);                          /* a polite DISCONNECT, then SSH_CLOSED */
int ssh_kexes(void);                           /* key exchanges done (1 + rekeys) */
int ssh_strict(void);                          /* strict kex agreed with the server */

/* a question: 0 none; else *prompt is the text, *echo 0 for secrets */
int ssh_ask(const char **prompt, int *echo);
void ssh_answer(const char *line);             /* wiped after use; the caller wipes its copy */

/* "SHA256:..." of an ed25519 public key (OpenSSH style) */
void ssh_fingerprint(const uint8_t key[32], char out[52]);
/* "ssh-ed25519 AAAA... comment" of a public key */
void ssh_pubkey_line(const uint8_t key[32], const char *comment, char *out, size_t size);

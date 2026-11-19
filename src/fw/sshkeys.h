/* The SSH keys of the term core in the save file (store.h): sector S_KEYS = "TERMKEYS", version, the seed of
   the user key (ed25519, kept as is: no passphrase, user decision 2026-11-19), the seed of rng.c;
   sectors S_HOSTS.. = known_hosts (4 entries of 128 bytes a sector: host 64, port 2, format 1, key 32, type 1;
   format 1 = the ed25519 key (Ф3), 2 = the SHA-256 of the host key blob and its type ssh_kt (Ф6)). */
#pragma once
#include <stdint.h>
#include "ssh.h"

void sshkeys_poll(void);                    /* loads once the save file is there: call often */
const uint8_t *sshkeys_user(void);          /* seed 32 + public 32 (made at the first call), 0 = no randomness yet */
const uint8_t *sshkeys_user_pub(void);      /* 0 if none made yet */
enum ssh_hk sshkeys_host(const char *host, int port, int kt, const uint8_t hash[32], int store);
int sshkeys_host_type(const char *host, int port);   /* the key type known for the host, -1 none */
void sshkeys_reseed(void);                  /* a fresh seed for the next start (at every connection) */

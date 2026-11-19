/* see sshkeys.h */
#include <string.h>
#include "sshkeys.h"
#include "store.h"
#include "rng.h"
#include "monocypher.h"
#include "monocypher-ed25519.h"

#define HOSTS_MAX (S_HOSTS_N * 4)

static struct host { char name[64]; uint16_t port; uint8_t used; uint8_t key[32]; } hosts[HOSTS_MAX];
static uint8_t user_seed[32], user_sk[64];
static int have_user, loaded;

static void save_keys(void) {
    uint8_t b[512];
    memset(b, 0, sizeof b);
    memcpy(b, "TERMKEYS", 8);
    b[8] = 1;
    b[9] = (uint8_t)have_user;
    memcpy(b + 16, user_seed, 32);
    rng_bytes(b + 48, 32);                     /* the seed of the next start */
    store_wr(S_KEYS, b);
    crypto_wipe(b, sizeof b);
}

static void save_hosts(void) {
    uint8_t b[512];
    for (int s = 0; s < S_HOSTS_N; s++) {
        memset(b, 0, sizeof b);
        for (int k = 0; k < 4; k++) {
            const struct host *h = &hosts[s * 4 + k];
            uint8_t *e = b + 128 * k;
            if (!h->used) continue;
            memcpy(e, h->name, 64);
            e[64] = (uint8_t)h->port; e[65] = (uint8_t)(h->port >> 8);
            e[66] = 1;
            memcpy(e + 67, h->key, 32);
        }
        if (store_wr((uint32_t)(S_HOSTS + s), b)) return;
    }
}

static void make_user(void) {
    uint8_t seed[32], pk[32];
    memcpy(seed, user_seed, 32);
    crypto_ed25519_key_pair(user_sk, pk, seed);     /* wipes seed */
}

void sshkeys_poll(void) {
    if (loaded || store_state() == 0) return;
    loaded = 1;
    if (store_state() != 1) return;                  /* RAM only */
    uint8_t b[512];
    if (!store_rd(S_KEYS, b) && !memcmp(b, "TERMKEYS", 8) && b[8] == 1) {
        rng_mix(b + 48, 32, 0);
        rng_seeded();
        if (b[9]) {                                   /* the stored key wins over one made before the mount */
            memcpy(user_seed, b + 16, 32); have_user = 1; make_user(); }
    }
    crypto_wipe(b, sizeof b);
    save_keys();                                      /* the old seed must not come back */
    for (int s = 0; s < S_HOSTS_N; s++) {
        if (store_rd((uint32_t)(S_HOSTS + s), b)) break;
        for (int k = 0; k < 4; k++) {
            const uint8_t *e = b + 128 * k;
            struct host *h = &hosts[s * 4 + k];
            if (h->used || e[66] != 1) continue;     /* a host met before the mount stays */
            memcpy(h->name, e, 64);
            h->name[63] = 0;
            h->port = (uint16_t)(e[64] | e[65] << 8);
            memcpy(h->key, e + 67, 32);
            h->used = 1;
        }
    }
}

const uint8_t *sshkeys_user(void) {
    if (!have_user) {
        if (!rng_ready()) return 0;
        rng_bytes(user_seed, 32);
        have_user = 1;
        make_user();
        save_keys();
    }
    return user_sk;
}

const uint8_t *sshkeys_user_pub(void) { return have_user ? user_sk + 32 : 0; }

void sshkeys_reseed(void) { if (store_state() == 1) save_keys(); }

enum ssh_hk sshkeys_host(const char *name, int port, const uint8_t key[32], int store) {
    int i;
    for (i = 0; i < HOSTS_MAX; i++)
        if (hosts[i].used && hosts[i].port == port && !strcmp(hosts[i].name, name)) break;
    if (!store) {
        if (i == HOSTS_MAX) return SSH_HK_NEW;
        return crypto_verify32(hosts[i].key, key) ? SSH_HK_CHANGED : SSH_HK_OK;
    }
    if (i == HOSTS_MAX) {                             /* a free entry, or the first one goes */
        for (i = 0; i < HOSTS_MAX && hosts[i].used; i++) {}
        if (i == HOSTS_MAX) { memmove(&hosts[0], &hosts[1], sizeof hosts[0] * (HOSTS_MAX - 1)); i = HOSTS_MAX - 1; }
    }
    memset(&hosts[i], 0, sizeof hosts[i]);
    strncpy(hosts[i].name, name, sizeof hosts[i].name - 1);
    hosts[i].port = (uint16_t)port;
    memcpy(hosts[i].key, key, 32);
    hosts[i].used = 1;
    save_hosts();
    return SSH_HK_OK;
}

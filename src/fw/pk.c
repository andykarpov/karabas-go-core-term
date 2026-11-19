/* see pk.h. Numbers: little-endian arrays of 32-bit limbs; products in Montgomery form (CIOS). The curve
   parameters are those of `openssl ecparam -param_enc explicit` (SEC 2 / FIPS 186-4), a = -3 on all three. */
#include <string.h>
#include "pk.h"
#include "sha256.h"
#include "monocypher.h"
#include "monocypher-ed25519.h"

typedef uint32_t limb;
#define BN_MAX (PK_RSA_MAX_BITS / 32 + 2)

/* ------------------------------------------------------------------ numbers */

/* big-endian bytes -> n limbs (the bytes fit: checked by the callers) */
static void bn_from(limb *a, int n, const uint8_t *b, size_t len) {
    memset(a, 0, sizeof(limb) * (size_t)n);
    for (size_t i = 0; i < len; i++) {
        size_t k = len - 1 - i;              /* byte k from the end */
        a[k / 4] |= (limb)b[i] << (8 * (k % 4));
    }
}
static void bn_to(uint8_t *b, size_t len, const limb *a) {
    for (size_t i = 0; i < len; i++) { size_t k = len - 1 - i; b[i] = (uint8_t)(a[k / 4] >> (8 * (k % 4))); }
}
static int bn_cmp(const limb *a, const limb *b, int n) {
    for (int i = n - 1; i >= 0; i--) if (a[i] != b[i]) return a[i] > b[i] ? 1 : -1;
    return 0;
}
static int bn_is0(const limb *a, int n) { for (int i = 0; i < n; i++) if (a[i]) return 0; return 1; }
static limb bn_sub(limb *r, const limb *a, const limb *b, int n) {
    uint64_t br = 0;
    for (int i = 0; i < n; i++) { uint64_t d = (uint64_t)a[i] - b[i] - br; r[i] = (limb)d; br = (d >> 32) & 1; }
    return (limb)br;
}
static limb bn_add(limb *r, const limb *a, const limb *b, int n) {
    uint64_t c = 0;
    for (int i = 0; i < n; i++) { c += (uint64_t)a[i] + b[i]; r[i] = (limb)c; c >>= 32; }
    return (limb)c;
}
static int bn_bits(const limb *a, int n) {
    for (int i = n - 1; i >= 0; i--) if (a[i]) { int b = 32; while (!(a[i] >> (b - 1))) b--; return 32 * i + b; }
    return 0;
}
static int bn_bit(const limb *a, int i) { return (int)(a[i / 32] >> (i % 32)) & 1; }

/* a bytes string without its leading zero bytes */
static void strip(const uint8_t **p, size_t *n) { while (*n && !**p) { (*p)++; (*n)--; } }

/* ------------------------------------------------------------------ Montgomery arithmetic mod m (odd) */
struct mont {
    int n;                                   /* limbs */
    limb m[BN_MAX], rr[BN_MAX], one[BN_MAX]; /* m, R^2 mod m, R mod m (R = 2^(32 n)) */
    limb m0;                                 /* -m^-1 mod 2^32 */
    int p521;                                /* m = 2^521 - 1: plain products, a shift-add reduction (R = 1) */
};

static void mont_consts(struct mont *M);
static void mont_init(struct mont *M, const limb *m, int n) {
    M->n = n;
    memcpy(M->m, m, sizeof(limb) * (size_t)n);
    limb x = m[0];                           /* Newton: x = m[0]^-1 mod 2^32 */
    for (int i = 0; i < 5; i++) x *= 2 - m[0] * x;
    M->m0 = (limb)(0u - x);
    M->p521 = n == 17 && m[16] == 0x1ff;
    for (int i = 0; i < 16 && M->p521; i++) M->p521 = m[i] == 0xffffffffu;
    if (M->p521) {                            /* no Montgomery form: 1 and R^2 are 1 */
        memset(M->one, 0, sizeof(limb) * 17); M->one[0] = 1;
        memcpy(M->rr, M->one, sizeof(limb) * 17);
        return;
    }
    mont_consts(M);
}

/* r = a b mod 2^521 - 1 (a, b < m): t = lo + 2^521 hi = lo + hi (mod m) */
static void p521_mul(limb *r, const limb *a, const limb *b, const limb *m) {
    limb t[34], lo[17], hi[17];
    memset(t, 0, sizeof t);
    for (int i = 0; i < 17; i++) {
        uint64_t c = 0;
        for (int j = 0; j < 17; j++) { c += (uint64_t)a[j] * b[i] + t[i + j]; t[i + j] = (limb)c; c >>= 32; }
        t[i + 17] = (limb)c;
    }
    memcpy(lo, t, sizeof(limb) * 16);
    lo[16] = t[16] & 0x1ff;
    for (int i = 0; i < 17; i++) hi[i] = t[16 + i] >> 9 | t[17 + i] << 23;
    bn_add(r, lo, hi, 17);                   /* < 2^522: no carry out */
    while (bn_cmp(r, m, 17) >= 0) bn_sub(r, r, m, 17);
}

static void mont_mul(limb *r, const limb *a, const limb *b, const struct mont *M);

/* t = 2 t mod m (t < m) */
static void mod_dbl(limb *t, const limb *m, int n) {
    limb top = t[n - 1] >> 31;
    for (int i = n - 1; i > 0; i--) t[i] = t[i] << 1 | t[i - 1] >> 31;
    t[0] <<= 1;
    if (top || bn_cmp(t, m, n) >= 0) bn_sub(t, t, m, n);
}

/* R mod m and R^2 mod m: R mod m by doubling from the top bit of m; then the Montgomery form of 2^a (a doublings
   more) squared k times is that of 2^(a 2^k) = R, 32 n = a 2^k, a odd: R^2 mod m */
static void mont_consts(struct mont *M) {
    int n = M->n, bits = bn_bits(M->m, n);
    limb t[BN_MAX];
    memset(t, 0, sizeof(limb) * (size_t)n);
    t[(bits - 1) / 32] = (limb)1 << ((bits - 1) % 32);     /* 2^(bits - 1) < m */
    for (int k = bits - 1; k < 32 * n; k++) mod_dbl(t, M->m, n);
    memcpy(M->one, t, sizeof(limb) * (size_t)n);
    int a = 32 * n, k = 0;
    while (!(a & 1)) { a >>= 1; k++; }
    for (int i = 0; i < a; i++) mod_dbl(t, M->m, n);        /* the form of 2^a */
    for (int i = 0; i < k; i++) mont_mul(t, t, t, M);
    memcpy(M->rr, t, sizeof(limb) * (size_t)n);
}

/* r = a b R^-1 mod m (a, b < m; r may be a or b) */
static void mont_mul(limb *r, const limb *a, const limb *b, const struct mont *M) {
    if (M->p521) { p521_mul(r, a, b, M->m); return; }
    int n = M->n;
    limb t[BN_MAX + 1];
    memset(t, 0, sizeof(limb) * (size_t)(n + 1));
    /* FIOS: the product row and the reduction in one pass (t < 2m all along: t[n] is 0 or 1) */
    for (int i = 0; i < n; i++) {
        limb bi = b[i];
        uint64_t p = (uint64_t)a[0] * bi + t[0];
        limb c = (limb)(p >> 32), s = (limb)p;
        limb u = s * M->m0;
        uint64_t q = (uint64_t)u * M->m[0] + s;
        limb c2 = (limb)(q >> 32);
        for (int j = 1; j < n; j++) {
            p = (uint64_t)a[j] * bi + t[j] + c;
            c = (limb)(p >> 32);
            q = (uint64_t)u * M->m[j] + (limb)p + c2;
            c2 = (limb)(q >> 32);
            t[j - 1] = (limb)q;
        }
        p = (uint64_t)t[n] + c + c2;
        t[n - 1] = (limb)p;
        t[n] = (limb)(p >> 32);
    }
    if (t[n] || bn_cmp(t, M->m, n) >= 0) bn_sub(t, t, M->m, n);
    memcpy(r, t, sizeof(limb) * (size_t)n);
}
static void to_mont(limb *r, const limb *a, const struct mont *M) { mont_mul(r, a, M->rr, M); }
static void from_mont(limb *r, const limb *a, const struct mont *M) {
    limb one[BN_MAX];
    memset(one, 0, sizeof(limb) * (size_t)M->n);
    one[0] = 1;
    mont_mul(r, a, one, M);
}
/* r = a^e (Montgomery form in and out), e: el limbs */
static void mont_pow(limb *r, const limb *a, const limb *e, int el, const struct mont *M) {
    limb x[BN_MAX];
    memcpy(x, M->one, sizeof(limb) * (size_t)M->n);
    for (int i = bn_bits(e, el) - 1; i >= 0; i--) {
        mont_mul(x, x, x, M);
        if (bn_bit(e, i)) mont_mul(x, x, a, M);
    }
    memcpy(r, x, sizeof(limb) * (size_t)M->n);
}
/* r = a^-1 (Montgomery form; m prime, a != 0): a^(m-2) */
static void mont_inv(limb *r, const limb *a, const struct mont *M) {
    limb e[BN_MAX], two[BN_MAX];
    memset(two, 0, sizeof(limb) * (size_t)M->n);
    two[0] = 2;
    bn_sub(e, M->m, two, M->n);
    mont_pow(r, a, e, M->n, M);
}
static void mod_add(limb *r, const limb *a, const limb *b, const struct mont *M) {
    limb c = bn_add(r, a, b, M->n);
    if (c || bn_cmp(r, M->m, M->n) >= 0) bn_sub(r, r, M->m, M->n);
}
static void mod_sub(limb *r, const limb *a, const limb *b, const struct mont *M) {
    if (bn_sub(r, a, b, M->n)) bn_add(r, r, M->m, M->n);
}

/* ------------------------------------------------------------------ hashes */
void pk_sha384(uint8_t out[48], const uint8_t *m, size_t n) {
    static const uint64_t iv[8] = {
        0xcbbb9d5dc1059ed8ull, 0x629a292a367cd507ull, 0x9159015a3070dd17ull, 0x152fecd8f70e5939ull,
        0x67332667ffc00b31ull, 0x8eb44a8768581511ull, 0xdb0c2e0d64f98fa7ull, 0x47b5481dbefa4fa4ull };
    crypto_sha512_ctx c;
    uint8_t h[64];
    crypto_sha512_init(&c);
    memcpy(c.hash, iv, sizeof iv);           /* SHA-384 = SHA-512 with its own IV, cut to 48 bytes */
    crypto_sha512_update(&c, m, n);
    crypto_sha512_final(&c, h);
    memcpy(out, h, 48);
}

/* ------------------------------------------------------------------ RSA */
static struct mont rsa_m;                    /* static: 3 KB */

int pk_rsa_verify(const uint8_t *n_, size_t nlen, const uint8_t *e_, size_t elen, int sha512,
                  const uint8_t *msg, size_t mlen, const uint8_t *sig, size_t slen) {
    strip(&n_, &nlen);
    strip(&e_, &elen);
    if (nlen * 8 > PK_RSA_MAX_BITS || nlen < 1 || elen < 1 || elen > 8) return 0;
    int nl = (int)(nlen + 3) / 4;
    limb n[BN_MAX], e[3], s[BN_MAX];
    bn_from(n, nl, n_, nlen);
    if (bn_bits(n, nl) < PK_RSA_MIN_BITS || !(n[0] & 1)) return 0;
    bn_from(e, 3, e_, elen);
    if (!(e[0] & 1) || (e[0] == 1 && !e[1] && !e[2])) return 0;
    if (slen > nlen) return 0;               /* a shorter one is left-padded (some servers strip zeros) */
    bn_from(s, nl, sig, slen);
    if (bn_cmp(s, n, nl) >= 0) return 0;
    mont_init(&rsa_m, n, nl);
    limb x[BN_MAX];
    to_mont(x, s, &rsa_m);
    mont_pow(x, x, e, 3, &rsa_m);
    from_mont(x, x, &rsa_m);
    uint8_t em[PK_RSA_MAX_BITS / 8], want[PK_RSA_MAX_BITS / 8];
    bn_to(em, nlen, x);
    /* EMSA-PKCS1-v1_5 (RFC 8017 9.2): 00 01 FF..FF 00 DigestInfo H, the whole string compared */
    static const uint8_t di256[19] = { 0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20 };
    static const uint8_t di512[19] = { 0x30, 0x51, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x03, 0x05, 0x00, 0x04, 0x40 };
    size_t hl = sha512 ? 64 : 32, tl = 19 + hl;
    if (nlen < tl + 11) return 0;
    want[0] = 0; want[1] = 1;
    memset(want + 2, 0xff, nlen - tl - 3);
    want[nlen - tl - 1] = 0;
    memcpy(want + nlen - tl, sha512 ? di512 : di256, 19);
    if (sha512) crypto_sha512(want + nlen - hl, msg, mlen);
    else sha256(want + nlen - hl, msg, mlen);
    int ok = 1;
    for (size_t i = 0; i < nlen; i++) ok &= em[i] == want[i];
    return ok;
}

/* ------------------------------------------------------------------ ECDSA */
#define EL 17                                /* limbs of P-521 */
static const struct { size_t sz; const char *p, *n, *b, *gx, *gy; } curves[PK_CURVES] = {
    { 32, "ffffffff00000001000000000000000000000000ffffffffffffffffffffffff",
      "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551",
      "5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b",
      "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296",
      "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5" },  /* P256 */
    { 48, "fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000ffffffff",
      "ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973",
      "b3312fa7e23ee7e4988e056be3f82d19181d9c6efe8141120314088f5013875ac656398d8a2ed19d2a85c8edd3ec2aef",
      "aa87ca22be8b05378eb1c71ef320ad746e1d3b628ba79b9859f741e082542a385502f25dbf55296c3a545e3872760ab7",
      "3617de4a96262c6f5d9e98bf9292dc29f8f41dbd289a147ce9da3113b5f0b8c00a60b1ce1d7e819d7a431d7c90ea0e5f" },  /* P384 */
    { 66, "01ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
      "01fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffa51868783bf2f966b7fcc0148f709a5d03bb5c9b8899c47aebb6fb71e91386409",
      "0051953eb9618e1c9a1f929a21a0b68540eea2da725b99b315f3b8b489918ef109e156193951ec7e937b1652c0bd3bb1bf073573df883d2c34f1ef451fd46b503f00",
      "00c6858e06b70404e9cd9e3ecb662395b4429c648139053fb521f828af606b4d3dbaa14b5e77efe75928fe1dc127a2ffa8de3348b3c1856a429bf97e7e31c2e5bd66",
      "011839296a789a3bc0045c8a5fb42c7d1bd998f54449579b446817afbd17273e662c97ee72995ef42640c550b9013fad0761353c7086a272c24088be94769fd16650" },  /* P521 */
};

size_t pk_curve_bytes(int curve) { return curve >= 0 && curve < PK_CURVES ? curves[curve].sz : 0; }

static void hex_limbs(limb *a, int n, const char *h) {
    uint8_t b[66];
    size_t len = strlen(h) / 2;
    for (size_t i = 0; i < len; i++) {
        int v = 0;
        for (int k = 0; k < 2; k++) { char c = h[2 * i + k]; v = v * 16 + (c <= '9' ? c - '0' : c - 'a' + 10); }
        b[i] = (uint8_t)v;
    }
    bn_from(a, n, b, len);
}

/* a point in Jacobian coordinates, Montgomery form mod p; Z = 0: the point at infinity */
struct pt { limb x[EL], y[EL], z[EL]; };
static struct mont fp, fn;                  /* mod p, mod n of the curve */

#define MUL(r, a, b) mont_mul(r, a, b, &fp)
#define ADD(r, a, b) mod_add(r, a, b, &fp)
#define SUB(r, a, b) mod_sub(r, a, b, &fp)

/* dbl-2001-b (a = -3) */
static void pt_dbl(struct pt *r, const struct pt *p) {
    int n = fp.n;
    if (bn_is0(p->z, n) || bn_is0(p->y, n)) { memset(r, 0, sizeof *r); return; }
    limb d[EL], g[EL], be[EL], al[EL], t[EL], u[EL];
    MUL(d, p->z, p->z);                      /* delta */
    MUL(g, p->y, p->y);                      /* gamma */
    MUL(be, p->x, g);                        /* beta */
    SUB(t, p->x, d); ADD(u, p->x, d); MUL(al, t, u); ADD(t, al, al); ADD(al, al, t);   /* alpha = 3 (x - d)(x + d) */
    limb x3[EL], y3[EL], z3[EL];
    MUL(x3, al, al);
    ADD(t, be, be); ADD(t, t, t); ADD(u, t, t);          /* t = 4 beta, u = 8 beta */
    SUB(x3, x3, u);
    ADD(z3, p->y, p->z); MUL(z3, z3, z3); SUB(z3, z3, g); SUB(z3, z3, d);
    SUB(t, t, x3); MUL(y3, al, t);
    MUL(g, g, g); ADD(g, g, g); ADD(g, g, g); ADD(g, g, g);   /* 8 gamma^2 */
    SUB(y3, y3, g);
    memcpy(r->x, x3, sizeof x3); memcpy(r->y, y3, sizeof y3); memcpy(r->z, z3, sizeof z3);
}

static void pt_add(struct pt *r, const struct pt *p, const struct pt *q) {
    int n = fp.n;
    if (bn_is0(p->z, n)) { *r = *q; return; }
    if (bn_is0(q->z, n)) { *r = *p; return; }
    limb z1z1[EL], z2z2[EL], u1[EL], u2[EL], s1[EL], s2[EL], t[EL];
    MUL(z1z1, p->z, p->z); MUL(z2z2, q->z, q->z);
    MUL(u1, p->x, z2z2); MUL(u2, q->x, z1z1);
    MUL(t, q->z, z2z2); MUL(s1, p->y, t);
    MUL(t, p->z, z1z1); MUL(s2, q->y, t);
    if (!bn_cmp(u1, u2, n)) {
        if (!bn_cmp(s1, s2, n)) { pt_dbl(r, p); return; }  /* P = Q */
        memset(r, 0, sizeof *r); return;                      /* P = -Q */
    }
    limb h[EL], rr[EL], h2[EL], h3[EL], x3[EL], y3[EL], z3[EL];
    SUB(h, u2, u1); SUB(rr, s2, s1);
    MUL(h2, h, h); MUL(h3, h2, h);
    MUL(u1, u1, h2);                         /* u1 h^2 */
    MUL(x3, rr, rr); SUB(x3, x3, h3); SUB(x3, x3, u1); SUB(x3, x3, u1);
    SUB(t, u1, x3); MUL(y3, rr, t); MUL(t, s1, h3); SUB(y3, y3, t);
    MUL(z3, p->z, q->z); MUL(z3, z3, h);
    memcpy(r->x, x3, sizeof x3); memcpy(r->y, y3, sizeof y3); memcpy(r->z, z3, sizeof z3);
}

/* r = p + q, q affine (Z = 1): madd, 11 products instead of 16 */
static void pt_madd(struct pt *r, const struct pt *p, const struct pt *q) {
    int n = fp.n;
    if (bn_is0(p->z, n)) { *r = *q; return; }
    limb z1z1[EL], u2[EL], s2[EL], t[EL];
    MUL(z1z1, p->z, p->z);
    MUL(u2, q->x, z1z1);
    MUL(t, p->z, z1z1); MUL(s2, q->y, t);
    if (!bn_cmp(p->x, u2, n)) {
        if (!bn_cmp(p->y, s2, n)) { pt_dbl(r, p); return; }
        memset(r, 0, sizeof *r); return;
    }
    limb h[EL], rr[EL], h2[EL], h3[EL], u1h2[EL], x3[EL], y3[EL], z3[EL];
    SUB(h, u2, p->x); SUB(rr, s2, p->y);
    MUL(h2, h, h); MUL(h3, h2, h);
    MUL(u1h2, p->x, h2);
    MUL(x3, rr, rr); SUB(x3, x3, h3); SUB(x3, x3, u1h2); SUB(x3, x3, u1h2);
    SUB(t, u1h2, x3); MUL(y3, rr, t); MUL(t, p->y, h3); SUB(y3, y3, t);
    MUL(z3, p->z, h);
    memcpy(r->x, x3, sizeof x3); memcpy(r->y, y3, sizeof y3); memcpy(r->z, z3, sizeof z3);
}

int pk_ecdsa_verify(int curve, const uint8_t *q, size_t qlen, const uint8_t *r_, size_t rlen,
                    const uint8_t *s_, size_t slen, const uint8_t *msg, size_t mlen) {
    if (curve < 0 || curve >= PK_CURVES) return 0;
    size_t sz = curves[curve].sz;
    int n = (int)(sz + 3) / 4;
    if (qlen != 1 + 2 * sz || q[0] != 4) return 0;
    strip(&r_, &rlen);
    strip(&s_, &slen);
    if (rlen > sz || slen > sz) return 0;
    limb p[EL], ord[EL], b[EL], qx[EL], qy[EL], r[EL], s[EL], e[EL];
    hex_limbs(p, n, curves[curve].p);
    hex_limbs(ord, n, curves[curve].n);
    hex_limbs(b, n, curves[curve].b);
    bn_from(qx, n, q + 1, sz);
    bn_from(qy, n, q + 1 + sz, sz);
    bn_from(r, n, r_, rlen);
    bn_from(s, n, s_, slen);
    if (bn_is0(r, n) || bn_is0(s, n) || bn_cmp(r, ord, n) >= 0 || bn_cmp(s, ord, n) >= 0) return 0;
    if (bn_cmp(qx, p, n) >= 0 || bn_cmp(qy, p, n) >= 0) return 0;
    mont_init(&fp, p, n);
    mont_init(&fn, ord, n);
    /* Q on the curve: y^2 = x^3 - 3 x + b */
    struct pt Q, G, GQ, R;
    limb l[EL], rt[EL], t[EL];
    to_mont(Q.x, qx, &fp); to_mont(Q.y, qy, &fp); memcpy(Q.z, fp.one, sizeof(limb) * (size_t)n);
    to_mont(t, b, &fp);
    MUL(l, Q.y, Q.y);
    MUL(rt, Q.x, Q.x); MUL(rt, rt, Q.x);
    SUB(rt, rt, Q.x); SUB(rt, rt, Q.x); SUB(rt, rt, Q.x); ADD(rt, rt, t);
    if (bn_cmp(l, rt, n)) return 0;
    /* e: the hash (no cut needed: 256 / 384 / 512 bits for 256 / 384 / 521-bit orders), mod n */
    uint8_t h[64];
    size_t hl = curve == PK_P256 ? 32 : curve == PK_P384 ? 48 : 64;
    if (curve == PK_P256) sha256(h, msg, mlen);
    else if (curve == PK_P384) pk_sha384(h, msg, mlen);
    else crypto_sha512(h, msg, mlen);
    bn_from(e, n, h, hl);
    if (bn_cmp(e, ord, n) >= 0) bn_sub(e, e, ord, n);
    /* u1 = e / s, u2 = r / s mod n */
    limb w[EL], u1[EL], u2[EL];
    to_mont(w, s, &fn); mont_inv(w, w, &fn);
    to_mont(t, e, &fn); mont_mul(t, t, w, &fn); from_mont(u1, t, &fn);
    to_mont(t, r, &fn); mont_mul(t, t, w, &fn); from_mont(u2, t, &fn);
    /* u1 G + u2 Q (Shamir) */
    hex_limbs(t, n, curves[curve].gx); to_mont(G.x, t, &fp);
    hex_limbs(t, n, curves[curve].gy); to_mont(G.y, t, &fp);
    memcpy(G.z, fp.one, sizeof(limb) * (size_t)n);
    pt_add(&GQ, &G, &Q);
    int gq_inf = bn_is0(GQ.z, n);             /* Q = -G */
    if (!gq_inf) {                            /* affine, for pt_madd */
        limb zi[EL], z2[EL];
        mont_inv(zi, GQ.z, &fp); MUL(z2, zi, zi);
        MUL(GQ.x, GQ.x, z2); MUL(z2, z2, zi); MUL(GQ.y, GQ.y, z2);
        memcpy(GQ.z, fp.one, sizeof(limb) * (size_t)n);
    }
    memset(&R, 0, sizeof R);
    int bits = bn_bits(u1, n) > bn_bits(u2, n) ? bn_bits(u1, n) : bn_bits(u2, n);
    for (int i = bits - 1; i >= 0; i--) {
        pt_dbl(&R, &R);
        int k = bn_bit(u1, i) | bn_bit(u2, i) << 1;
        if (k == 1) pt_madd(&R, &R, &G);
        else if (k == 2) pt_madd(&R, &R, &Q);
        else if (k == 3 && !gq_inf) pt_madd(&R, &R, &GQ);
    }
    if (bn_is0(R.z, n)) return 0;
    /* x = X / Z^2, mod n == r */
    mont_inv(t, R.z, &fp); MUL(t, t, t); MUL(t, t, R.x); from_mont(t, t, &fp);
    if (bn_cmp(t, ord, n) >= 0) bn_sub(t, t, ord, n);
    return !bn_cmp(t, r, n);
}

/* Ф6 bench: the host-key signature checks of pk.c on rv32im (ISS). Marks: 1..3 RSA 2048 / 3072 / 4096 (SHA-512),
   4..6 ECDSA P-256 / P-384 / P-521. Each must be valid: the results are printed. */
#include <stdint.h>
#include <stdio.h>
#include "pk.h"
#include "bench_pk_vec.h"
#ifdef HOST
#define MARK(v) ((void)0)
#else
#include "mmio.h"
#define MARK(v) (MMIO_MARK = (v))
#endif

int main(void) {
    static const uint8_t e[3] = { 1, 0, 1 };
    int ok[6];
    MARK(0);
    ok[0] = pk_rsa_verify(rsa2048_n, sizeof rsa2048_n, e, 3, 1, msg, sizeof msg, rsa2048_s, sizeof rsa2048_s); MARK(1);
    ok[1] = pk_rsa_verify(rsa3072_n, sizeof rsa3072_n, e, 3, 1, msg, sizeof msg, rsa3072_s, sizeof rsa3072_s); MARK(2);
    ok[2] = pk_rsa_verify(rsa4096_n, sizeof rsa4096_n, e, 3, 1, msg, sizeof msg, rsa4096_s, sizeof rsa4096_s); MARK(3);
    ok[3] = pk_ecdsa_verify(PK_P256, ec0_q, sizeof ec0_q, ec0_r, sizeof ec0_r, ec0_s, sizeof ec0_s, msg, sizeof msg); MARK(4);
    ok[4] = pk_ecdsa_verify(PK_P384, ec1_q, sizeof ec1_q, ec1_r, sizeof ec1_r, ec1_s, sizeof ec1_s, msg, sizeof msg); MARK(5);
    ok[5] = pk_ecdsa_verify(PK_P521, ec2_q, sizeof ec2_q, ec2_r, sizeof ec2_r, ec2_s, sizeof ec2_s, msg, sizeof msg); MARK(6);
    printf("ok %d %d %d %d %d %d\n", ok[0], ok[1], ok[2], ok[3], ok[4], ok[5]);
    return 0;
}

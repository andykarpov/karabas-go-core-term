#!/usr/bin/env python3
"""Ф6: one valid signature of each kind as a C header for fw/bench_pk.c (RSA 2048 / 3072 / 4096 with SHA-512,
ECDSA P-256 / P-384 / P-521). usage: pk_bench_vec.py > header"""
from cryptography.hazmat.primitives.asymmetric import rsa, ec, padding, utils
from cryptography.hazmat.primitives import hashes
msg = bytes(range(32))           # an exchange hash
def arr(name, b): return f'static const uint8_t {name}[{len(b)}] = {{ ' + ', '.join(str(x) for x in b) + ' };'
def ib(v, n=None): return v.to_bytes(n or (v.bit_length() + 7) // 8, 'big')
print('/* made by src/tools/pk_bench_vec.py */')
print(arr('msg', msg))
for bits in (2048, 3072, 4096):
    k = rsa.generate_private_key(public_exponent=65537, key_size=bits)
    print(arr(f'rsa{bits}_n', ib(k.public_key().public_numbers().n)))
    print(arr(f'rsa{bits}_s', k.sign(msg, padding.PKCS1v15(), hashes.SHA512())))
for i, (c, h, sz) in enumerate([(ec.SECP256R1(), hashes.SHA256(), 32), (ec.SECP384R1(), hashes.SHA384(), 48), (ec.SECP521R1(), hashes.SHA512(), 66)]):
    k = ec.generate_private_key(c); pn = k.public_key().public_numbers()
    r, s = utils.decode_dss_signature(k.sign(msg, ec.ECDSA(h)))
    print(arr(f'ec{i}_q', b'\x04' + ib(pn.x, sz) + ib(pn.y, sz)))
    print(arr(f'ec{i}_r', ib(r, sz)))
    print(arr(f'ec{i}_s', ib(s, sz)))

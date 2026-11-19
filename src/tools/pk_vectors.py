#!/usr/bin/env python3
"""Ф6: test vectors of src/fw/pk.c (test_pk.c reads them on stdin), made with Python cryptography: RSA PKCS #1 v1.5
SHA-256 / SHA-512 (1024..4096 bits, e 65537 and 3) and ECDSA P-256 / P-384 / P-521, each valid and broken (the
signature, the message, another key), the edges (r, s = 0 / n; s >= n; a short / long signature; a point off the
curve, x >= p), and SHA-384. usage: pk_vectors.py [seed 1] [keys per kind 3]"""
import os, random, sys, hashlib
from cryptography.hazmat.primitives.asymmetric import rsa, ec, padding, utils
from cryptography.hazmat.primitives import hashes

rnd = random.Random(int(sys.argv[1]) if len(sys.argv) > 1 else 1)
K = int(sys.argv[2]) if len(sys.argv) > 2 else 3
out = []
def hx(b): return b.hex() if b else '-'
def ib(v, n=None): return v.to_bytes(n or max(1, (v.bit_length() + 7) // 8), 'big')
def flip(b): i = rnd.randrange(len(b)); return b[:i] + bytes([b[i] ^ (1 << rnd.randrange(8))]) + b[i + 1:]

for bits in (1024, 2048, 3072, 4096):
    for e in (65537, 3):
        for _ in range(1 if bits >= 3072 else K):
            k = rsa.generate_private_key(public_exponent=e, key_size=bits)
            pn = k.public_key().public_numbers()
            other = rsa.generate_private_key(public_exponent=65537, key_size=bits).public_key().public_numbers()
            for sha512 in (0, 1):
                msg = os.urandom(rnd.choice([0, 1, 32, 100]))
                sig = k.sign(msg, padding.PKCS1v15(), hashes.SHA512() if sha512 else hashes.SHA256())
                n, ee = ib(pn.n), ib(pn.e)
                out.append(f'rsa {hx(n)} {hx(ee)} {sha512} {hx(msg)} {hx(sig)} 1')
                out.append(f'rsa {hx(n)} {hx(ee)} {1 - sha512} {hx(msg)} {hx(sig)} 0')           # the other hash
                out.append(f'rsa {hx(n)} {hx(ee)} {sha512} {hx(msg)} {hx(flip(sig))} 0')
                out.append(f'rsa {hx(n)} {hx(ee)} {sha512} {hx(flip(msg) if msg else b"x")} {hx(sig)} 0')
                out.append(f'rsa {hx(ib(other.n))} {hx(ee)} {sha512} {hx(msg)} {hx(sig)} 0')
                out.append(f'rsa {hx(b"" + n)} {hx(ee)} {sha512} {hx(msg)} {hx(b"\0" + sig)} 0')   # longer than n
                s = int.from_bytes(sig, 'big')
                out.append(f'rsa {hx(n)} {hx(ee)} {sha512} {hx(msg)} {hx(ib(s + pn.n, len(n)) if s + pn.n < 1 << (8 * len(n)) else ib(pn.n))} 0')  # s >= n
                if sig[0] == 0:
                    out.append(f'rsa {hx(n)} {hx(ee)} {sha512} {hx(msg)} {hx(sig.lstrip(b"\0"))} 1')  # a stripped one
# RSA: an even modulus, e = 1, a small modulus
k = rsa.generate_private_key(public_exponent=65537, key_size=1024); pn = k.public_key().public_numbers()
msg = b'abc'; sig = k.sign(msg, padding.PKCS1v15(), hashes.SHA256())
out.append(f'rsa {hx(ib(pn.n ^ 1))} {hx(ib(pn.e))} 0 {hx(msg)} {hx(sig)} 0')
out.append(f'rsa {hx(ib(pn.n))} 01 0 {hx(msg)} {hx(sig)} 0')
k512 = rsa.generate_private_key(public_exponent=65537, key_size=512)
out.append(f'rsa {hx(ib(k512.public_key().public_numbers().n))} 010001 0 {hx(msg)} {hx(k512.sign(msg, padding.PKCS1v15(), hashes.SHA256()))} 0')

curves = [(ec.SECP256R1(), hashes.SHA256(), 32), (ec.SECP384R1(), hashes.SHA384(), 48), (ec.SECP521R1(), hashes.SHA512(), 66)]
P = [2**256 - 2**224 + 2**192 + 2**96 - 1, 2**384 - 2**128 - 2**96 + 2**32 - 1, 2**521 - 1]
for ci, (crv, h, sz) in enumerate(curves):
    for _ in range(K * 3):
        k = crv and ec.generate_private_key(crv)
        pn = k.public_key().public_numbers()
        q = b'\x04' + ib(pn.x, sz) + ib(pn.y, sz)
        oq = ec.generate_private_key(crv).public_key().public_numbers()
        oq = b'\x04' + ib(oq.x, sz) + ib(oq.y, sz)
        msg = os.urandom(rnd.choice([0, 1, 32, 100]))
        r, s = utils.decode_dss_signature(k.sign(msg, ec.ECDSA(h)))
        nn = {0: 0xffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551,
              1: 0xffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973,
              2: 0x01fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffa51868783bf2f966b7fcc0148f709a5d03bb5c9b8899c47aebb6fb71e91386409}[ci]
        R, S = ib(r), ib(s)
        out.append(f'ec {ci} {hx(q)} {hx(R)} {hx(S)} {hx(msg)} 1')
        out.append(f'ec {ci} {hx(q)} {hx(b"\0" + R)} {hx(S)} {hx(msg)} 1')          # an mpint with a sign byte
        out.append(f'ec {ci} {hx(q)} {hx(ib(nn - s if s != nn - s else s))} {hx(S)} {hx(msg)} 0')
        out.append(f'ec {ci} {hx(q)} {hx(R)} {hx(ib(nn - s))} {hx(msg)} 1')         # (r, n - s) is valid too (ECDSA malleability)
        out.append(f'ec {ci} {hx(q)} {hx(flip(R))} {hx(S)} {hx(msg)} 0')
        out.append(f'ec {ci} {hx(q)} {hx(R)} {hx(flip(S))} {hx(msg)} 0')
        out.append(f'ec {ci} {hx(q)} {hx(R)} {hx(S)} {hx(flip(msg) if msg else b"x")} 0')
        out.append(f'ec {ci} {hx(oq)} {hx(R)} {hx(S)} {hx(msg)} 0')
        out.append(f'ec {ci} {hx(flip(q[1:]) and b"\x04" + flip(q[1:]))} {hx(R)} {hx(S)} {hx(msg)} 0')   # off the curve
        out.append(f'ec {ci} {hx(q)} - {hx(S)} {hx(msg)} 0')                       # r = 0
        out.append(f'ec {ci} {hx(q)} {hx(R)} {hx(b"\0")} {hx(msg)} 0')              # s = 0
        out.append(f'ec {ci} {hx(q)} {hx(ib(nn))} {hx(S)} {hx(msg)} 0')            # r = n
        out.append(f'ec {ci} {hx(q)} {hx(R)} {hx(ib(nn))} {hx(msg)} 0')            # s = n
        out.append(f'ec {ci} {hx(q)} {hx(ib(r + nn) if (r + nn).bit_length() <= 8 * sz else ib(nn))} {hx(S)} {hx(msg)} 0')  # r + n
        out.append(f'ec {ci} {hx(q[:-1])} {hx(R)} {hx(S)} {hx(msg)} 0')            # short point
        out.append(f'ec {ci} {hx(b"\x02" + q[1:])} {hx(R)} {hx(S)} {hx(msg)} 0')   # not 04
        out.append(f'ec {(ci + 1) % 3} {hx(q)} {hx(R)} {hx(S)} {hx(msg)} 0')       # another curve
    # x >= p (x + p with the same y mod p is not a valid encoding)
    pn = ec.generate_private_key(crv).public_key().public_numbers()
    if pn.x + P[ci] < 1 << (8 * sz):
        out.append(f'ec {ci} {hx(b"\x04" + ib(pn.x + P[ci], sz) + ib(pn.y, sz))} 01 01 - 0')
for m in (b'', b'abc', os.urandom(200), os.urandom(1000)):
    out.append(f'sha384 {hx(m)} {hashlib.sha384(m).hexdigest()}')
out.append('sha384 616263 cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7')
print('\n'.join(out))

#!/usr/bin/env python3
"""ppm2png.py in.ppm out.png [x y w h [scale]]: a crop of a P6 PPM (tb_soc shots) as PNG (stdlib only)."""
import struct, sys, zlib
d = open(sys.argv[1], 'rb').read()
parts = d.split(b'\n', 2) if d[2:3] == b' ' else None
hdr_end = d.index(b'255\n') + 4
w, h = map(int, d[:hdr_end].split()[1:3])
px = d[hdr_end:]
x0, y0, cw, ch = (map(int, sys.argv[3:7]) if len(sys.argv) > 6 else (0, 0, w, h))
sc = int(sys.argv[7]) if len(sys.argv) > 7 else 1
rows = []
for y in range(y0, y0 + ch):
    r = px[(y * w + x0) * 3:(y * w + x0 + cw) * 3]
    if sc > 1:
        r = b''.join(r[i:i + 3] * sc for i in range(0, len(r), 3))
    for _ in range(sc):
        rows.append(b'\0' + r)
def chunk(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', cw * sc, ch * sc, 8, 2, 0, 0, 0)) + \
      chunk(b'IDAT', zlib.compress(b''.join(rows), 6)) + chunk(b'IEND', b'')
open(sys.argv[2], 'wb').write(png)

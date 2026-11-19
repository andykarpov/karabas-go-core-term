#!/usr/bin/env python3
"""Ф1 link test with the echo firmware (fw make hw HWDEFS="-DTS_RING=768 -DECHO_TEST"): sends N lines of
printable text over the board's USB serial port, the core sends every byte back; prints the first
differences. usage: echo_test.py [lines 60] [port glob: default the core serial port
/dev/serial/by-id/usb-Karabas_Go_*-if02; the control port -if00 has no core UART since fw 2026-11-19]"""
import os, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'karabas-go-tools', 'board'))
import kg_board
n = int(sys.argv[1]) if len(sys.argv) > 1 else 60
kg_board.PORT_GLOB = sys.argv[2] if len(sys.argv) > 2 else '/dev/serial/by-id/usb-Karabas_Go_*-if02'
data = ''.join(f'line {i:03d} 0123456789 {chr(98 + i % 20) * 30} xyz\r\n' for i in range(n)).encode()
p = kg_board.Port()
p.drain()
got = bytearray(); sent = 0; t0 = time.time()
while (sent < len(data) or len(got) < len(data)) and time.time() - t0 < 60:
    r, w, _ = kg_board.select.select([p.fd], [p.fd] if sent < len(data) else [], [], 0.2)
    if r:
        got += os.read(p.fd, 4096)
    if w:
        try: sent += os.write(p.fd, data[sent:sent + 32])
        except BlockingIOError: pass
    if sent == len(data) and time.time() - t0 > 2 and not r and len(got) >= len(data): break
p.close()
print(f'sent {sent}, back {len(got)} in {time.time() - t0:.1f} s, equal {bytes(got) == data}')
gl = bytes(got).split(b'\r\n'); dl = data.split(b'\r\n')
bad = [(i, a, b) for i, (a, b) in enumerate(zip(dl, gl)) if a != b]
for i, a, b in bad[:8]:
    print(f'line {i}: sent {a!r}\n         back {b!r}')

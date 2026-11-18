#!/usr/bin/env python3
"""Ф0.3: ESP -> host rate of +IPD data through the boot core bridge (AT+KARABAS=esp).
usage: esp_rate.py <pc ip> [bytes 65536]. Current-only settings, CIPMODE 0, nothing stored."""
import os, socket, sys, threading, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'karabas-go-tools', 'board'))
import kg_board
PC, N, PORT = sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 65536, 23231
t = {}
def server():
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(('0.0.0.0', PORT)); s.listen(1)
    c, _ = s.accept(); t['start'] = time.time(); c.sendall(bytes(i & 255 for i in range(N))); t['sent'] = time.time()
    time.sleep(30); c.close()
threading.Thread(target=server, daemon=True).start()
p = kg_board.Port(); p.at('AT+KARABAS=esp')
os.write(p.fd, b'AT+CIPMODE=0\r\n'); time.sleep(0.5)
os.write(p.fd, f'AT+CIPSTART="TCP","{PC}",{PORT}\r\n'.encode())
raw = bytearray(); got = 0; first = last = None; end = time.time() + 40
while time.time() < end and got < N:
    r, _, _ = kg_board.select.select([p.fd], [], [], 0.05)
    if r:
        d = os.read(p.fd, 4096); raw += d
        now = time.time()
        # count payload: +IPD headers are small, approximate by parsing at the end
        i = 0; got = 0
        while True:
            j = raw.find(b'+IPD,', i)
            if j < 0: break
            k = raw.find(b':', j)
            if k < 0: break
            n = int(raw[j + 5:k]); got += min(n, len(raw) - k - 1); i = k + 1 + n
        if got and first is None: first = now
        last = now
os.write(p.fd, b'AT+CIPCLOSE\r\n'); time.sleep(0.5)
p.at('AT+KARABAS=uart'); p.close()
print(f'{got} / {N} bytes, first {first - t["start"]:.2f} s after accept, {got / (last - t["start"]) / 1024:.1f} KB/s '
      f'(server sendall done {t["sent"] - t["start"]:.2f} s)')

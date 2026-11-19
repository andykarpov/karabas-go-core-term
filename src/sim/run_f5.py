#!/usr/bin/env python3
"""Ф5: two tabs at once through the ESP8266 model (CIPMUX 1) in the RTL SoC sim. Two telnet servers on
127.0.0.1:2323 (tab 1) and :2325 (tab 2) send banners and echo; the firmware connects both at once (SW bit 28:
tab 1, then a new tab 2, shown). Keys: "hi" Enter go to tab 2; Ctrl+PgUp shows tab 1; "yo" Enter go to it.
Checks: each server got its own keys only, NAWS 128 x 47 on both, the module saw 2 links at once and
CIPMUX=1 before the first CIPSTART; a shot of tab 1 with the tab bar.
usage: run_f5.py [frames 60]"""
import os, socket, subprocess, sys, threading, time
HERE = os.path.dirname(os.path.abspath(__file__))
W = os.path.join(HERE, 'build', 'f5'); os.makedirs(W, exist_ok=True)
FR = int(sys.argv[1]) if len(sys.argv) > 1 else 60
IAC, DO, WILL, SB, SE = 255, 253, 251, 250, 240
got = {2323: bytearray(), 2325: bytearray()}
events = []


def server(port, banner):
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(('127.0.0.1', port)); s.listen(1)
    s.settimeout(300)
    c, _ = s.accept(); events.append(f'connect {port}')
    c.sendall(bytes([IAC, DO, 24, IAC, DO, 31, IAC, WILL, 1, IAC, WILL, 3]))
    time.sleep(0.2)
    c.sendall(banner.encode('utf-8'))
    c.settimeout(0.2)
    t0 = time.time()
    while time.time() - t0 < 280:
        try:
            d = c.recv(4096)
            if not d: break
            got[port].extend(d)
            data = bytes(b for b in d if 32 <= b < 127 or b == 13)
            if data: c.sendall(data.replace(b'\r', b'\r\n'))
        except socket.timeout:
            pass
    c.close()


threading.Thread(target=server, args=(2323, '\x1b[1;33mTab ONE server\x1b[0m — первый\r\n> '), daemon=True).start()
threading.Thread(target=server, args=(2325, '\x1b[1;36mTab TWO server\x1b[0m — второй\r\n> '), daemon=True).start()
time.sleep(0.3)
# HID usages: h 0b, i 0c, Enter 28, y 1c, o 12; Ctrl+PgUp = 0x4b with the Ctrl mod (0x01) << 8
k0 = FR - 24
keys = f'{k0}:0b,{k0 + 2}:0c,{k0 + 4}:28,{k0 + 8}:14b,{k0 + 11}:1c,{k0 + 13}:12,{k0 + 15}:28'
at_log = os.path.join(W, 'at.log')
r = subprocess.run([os.path.join(HERE, 'obj_soc', 'tb_soc'), os.path.join(HERE, '..', 'fw', 'build-hw-rv32imO2', 'term.bin'),
                    os.path.join(HERE, 'build', 'font', 'font_full.bin'), '--esp', '--sw', '0x10000000', '--frames', str(FR),
                    '--at-log', at_log, '--keys', keys, '--shot', str(FR - 1), os.path.join(W, 'f5.ppm'),
                    '--shot', str(k0 + 6), os.path.join(W, 'f5_tab2.ppm')],
                   capture_output=True, text=True)
print(r.stdout[-300:])
time.sleep(0.5)
print('events', events)
at = open(at_log).read().splitlines()
naws = bytes([IAC, SB, 31, 0, 128, 0, 47, IAC, SE])
first_start = next((i for i, l in enumerate(at) if l.startswith('AT+CIPSTART')), -1)
mux_before = any(l == 'AT+CIPMUX=1' for l in at[:first_start]) if first_start >= 0 else False
exp = {
    'both connected': len(events) == 2,
    'CIPMUX=1 before CIPSTART': mux_before,
    'CIPSTART=0 and =1': any(l.startswith('AT+CIPSTART=0,') for l in at) and any(l.startswith('AT+CIPSTART=1,') for l in at),
    'NAWS 128x47 tab 1': naws in got[2323],
    'NAWS 128x47 tab 2': naws in got[2325],
    'tab 2 got hi': b'hi\r\x00' in got[2325],
    'tab 1 got yo': b'yo\r\x00' in got[2323],
    'tab 1 no hi': b'hi' not in got[2323],
    'tab 2 no yo': b'yo' not in got[2325],
}
ok = True
for k, v in exp.items():
    ok &= v
    print(f'{k:26} {"ok" if v else "FAIL"}')
print('server 2323 got', bytes(got[2323])[-40:], '\nserver 2325 got', bytes(got[2325])[-40:])
for p in ('f5', 'f5_tab2'):
    subprocess.run([sys.executable, os.path.join(HERE, 'ppm2png.py'), os.path.join(W, p + '.ppm'), os.path.join(W, p + '.png')])
sys.exit(0 if ok else 1)

#!/usr/bin/env python3
"""Ф2: telnet through the ESP8266 model in the RTL SoC sim. A telnet server on 127.0.0.1:2323 asks
DO TTYPE, DO NAWS, WILL ECHO, WILL SGA, TTYPE SEND, sends a banner (colours, UTF-8), echoes what comes;
the firmware connects at once (SW bit 30), then keys are typed. Checks the answers of the client.
usage: run_f2.py [frames 40]"""
import os, socket, subprocess, sys, threading, time
HERE = os.path.dirname(os.path.abspath(__file__))
W = os.path.join(HERE, 'build', 'f2'); os.makedirs(W, exist_ok=True)
FR = int(sys.argv[1]) if len(sys.argv) > 1 else 40
IAC, DO, DONT, WILL, WONT, SB, SE = 255, 253, 254, 251, 252, 250, 240
got = bytearray(); events = []

def server():
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(('127.0.0.1', 2323)); s.listen(1)
    s.settimeout(300)
    c, _ = s.accept(); events.append('connect')
    c.sendall(bytes([IAC, DO, 24, IAC, DO, 31, IAC, WILL, 1, IAC, WILL, 3]))
    time.sleep(0.2)
    c.sendall(bytes([IAC, SB, 24, 1, IAC, SE]))
    c.sendall('\x1b[1;32mWelcome to the sim BBS\x1b[0m — Привет, 你好 \xff\xff\r\n> '.encode('utf-8').replace(b'\xc3\xbf\xc3\xbf', bytes([IAC, IAC])))
    c.settimeout(0.2)
    t0 = time.time()
    while time.time() - t0 < 280:
        try:
            d = c.recv(4096)
            if not d: break
            got.extend(d)
            # echo the data part (after the negotiation answers): printable bytes and CR
            data = bytes(b for b in d if 32 <= b < 127 or b == 13)
            if data and b'XTERM' not in d: c.sendall(data.replace(b'\r', b'\r\n'))
        except socket.timeout:
            pass
    c.close()

threading.Thread(target=server, daemon=True).start()
time.sleep(0.3)
# keys: h i Enter (HID usages 0x0b 0x0c 0x28) after the session is up
keys = f'{FR - 12}:0b,{FR - 10}:0c,{FR - 8}:28'
r = subprocess.run([os.path.join(HERE, 'obj_soc', 'tb_soc'), os.path.join(HERE, '..', 'fw', 'build-hw-rv32imO2', 'term.bin'),
                    os.path.join(HERE, 'build', 'font', 'font_full.bin'), '--esp', '--sw', '0x40000000', '--frames', str(FR),
                    '--at-log', os.path.join(W, 'at.log'), '--keys', keys, '--shot', str(FR - 1), os.path.join(W, 'f2.ppm')],
                   capture_output=True, text=True)
print(r.stdout[-400:])
time.sleep(0.5)
print('events', events)
print('server got', bytes(got))
exp = {'WILL TTYPE': bytes([IAC, WILL, 24]), 'WILL NAWS': bytes([IAC, WILL, 31]),
       'NAWS 128x48': bytes([IAC, SB, 31, 0, 128, 0, 48, IAC, SE]), 'DO ECHO': bytes([IAC, DO, 1]), 'DO SGA': bytes([IAC, DO, 3]),
       'TTYPE IS': bytes([IAC, SB, 24, 0]) + b'XTERM-256COLOR' + bytes([IAC, SE]), 'typed hi CR NUL': b'hi\r\x00'}
ok = True
for k, v in exp.items():
    f = v in got; ok &= f
    print(f'{k:16} {"ok" if f else "MISSING"}')
sys.exit(0 if ok else 1)

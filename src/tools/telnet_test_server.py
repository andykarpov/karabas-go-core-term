#!/usr/bin/env python3
"""A telnet test server for the board (Ф2): listens on 0.0.0.0:PORT, asks DO TTYPE / DO NAWS / WILL ECHO /
WILL SGA and TTYPE SEND, sends a banner (colours, UTF-8, box drawing), echoes the data; logs every byte of
the client (hex) to LOG. usage: telnet_test_server.py [port 2323] [log telnet_test.log] [codec utf-8 | cp866 | koi8_r | cp1251 | cp437]"""
import socket, sys, time
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 2323
LOG = sys.argv[2] if len(sys.argv) > 2 else 'telnet_test.log'
CODEC = sys.argv[3] if len(sys.argv) > 3 else 'utf-8'
IAC, DO, WILL, SB, SE = 255, 253, 251, 250, 240
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(('0.0.0.0', PORT)); s.listen(1)
log = open(LOG, 'a', buffering=1)
while True:
    c, a = s.accept()
    log.write(f'connect {a}\n')
    c.sendall(bytes([IAC, DO, 24, IAC, DO, 31, IAC, WILL, 1, IAC, WILL, 3]))
    time.sleep(0.3)
    c.sendall(bytes([IAC, SB, 24, 1, IAC, SE]))
    banner = ('\x1b[1;32mWelcome to the term test server\x1b[0m — Привет, мир! Γειά σου 你好 こんにちは\r\n'
              '\x1b[38;5;208m┌──────────────┐\r\n│ \x1b[1mbox drawing\x1b[0;38;5;208m │\r\n└──────────────┘\x1b[0m\r\n'
              + ''.join(f'\x1b[48;5;{i}m ' for i in range(16, 232)) + '\x1b[0m\r\n> ')
    if CODEC != 'utf-8':
        banner = (f'\x1b[1;32mCharset {CODEC}\x1b[0m: Привет, мир! Съешь же ещё этих мягких булок\r\n'
                  '\x1b[38;5;208m┌──────────────┐\r\n│ box drawing  │\r\n└──────────────┘\x1b[0m ░▒▓█\r\n> ')
    c.sendall(banner.encode(CODEC, errors='replace'))
    c.settimeout(1)
    while True:
        try:
            d = c.recv(4096)
        except socket.timeout:
            continue
        except OSError:
            break
        if not d:
            break
        log.write('got ' + d.hex() + ' ' + repr(d) + '\n')
        data = bytes(b for b in d if 32 <= b < 127 or b == 13)
        if data and b'XTERM' not in d:
            c.sendall(data.replace(b'\r', b'\r\n> '))
    log.write('closed\n')
    c.close()

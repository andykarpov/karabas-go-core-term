#!/usr/bin/env python3
"""Ф5: AT+CIPMUX=1 (several TCP links) of the stock ESP8266 AT firmware, from the PC through the boot core
(AT+KARABAS=esp bridge, as esp_probe.py). Two TCP servers on this PC (ports P and P+1) are the remote hosts.
usage: esp_mux_probe.py <pc ip> [port 23240] [logdir]
Checks: +IPD,<id>,<n>: of two links streaming at once (payloads per link), CIPSEND=<id>,n while the other
link streams (where its +IPD frames fall around "> " / SEND OK), "<id>,CLOSED" of a remote close, CIPCLOSE=<id>,
CIPSTATUS; then what a core that expects CIPMUX=0 meets while a link is open (CIPMUX=0, CIPCLOSE, CIPSTART
without id), CIPCLOSE=5, and the way back to CIPMUX=0.
Never stores anything in the module (no AT+UART_DEF / SAVETRANSLINK / RESTORE / CWJAP): CIPMUX is current only,
left at 0 at the end. Payloads to the ESP avoid CR / LF (the bridge turns them into CR LF)."""
import os, re, socket, sys, threading, time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'karabas-go-tools', 'board'))
import kg_board  # noqa: E402

PC = sys.argv[1]
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 23240
LOG = sys.argv[3] if len(sys.argv) > 3 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'sim', 'build', 'esp')
os.makedirs(LOG, exist_ok=True)

BULK = 24576
DOWN = [bytes((i * 7 + 1) & 255 for i in range(BULK)), bytes((i * 13 + 5) & 255 for i in range(BULK))]
UP = [bytes(b for b in range(256) if b not in (10, 13)), b'link1-' + bytes(b for b in range(255, -1, -1) if b not in (10, 13))]
LIFE = [40, 9]          # seconds a server keeps its link (link 1 is closed by the server)

ev = {}
srv_log = []


def server(k):
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(('0.0.0.0', PORT + k))
    s.listen(1)
    s.settimeout(60)
    try:
        c, a = s.accept()
    except socket.timeout:
        srv_log.append(f'server {k}: no connection')
        return
    srv_log.append(f'server {k}: connect from {a[0]}')
    c.settimeout(0.2)
    rx = bytearray()
    c.sendall(DOWN[k])
    end = time.time() + LIFE[k]
    while time.time() < end and not ev.get(f'close{k}'):
        try:
            d = c.recv(4096)
            if not d:
                srv_log.append(f'server {k}: closed by the ESP')
                break
            rx += d
        except socket.timeout:
            pass
    ev[f'rx{k}'] = bytes(rx)
    c.close()
    srv_log.append(f'server {k}: closed, got {len(rx)} bytes')


class Esp:
    def __init__(self):
        self.p = kg_board.Port()
        self.raw = bytearray()

    def rd(self, seconds, until=None):
        end = time.time() + seconds
        got = bytearray()
        while time.time() < end:
            r, _, _ = kg_board.select.select([self.p.fd], [], [], 0.05)
            if r:
                got += os.read(self.p.fd, 4096)
                if until and until in got:
                    break
        self.raw += got
        return bytes(got)

    def cmd(self, line, seconds=5, until=b'OK\r\n'):
        pass
        os.write(self.p.fd, line.encode() + b'\r\n')
        r = self.rd(seconds, until)
        print(f'> {line}\n{text(r)}')
        return r


def parse(raw):
    """(payload per link, the text outside the frames)"""
    data, rest, i = {}, bytearray(), 0
    while True:
        m = re.compile(rb'\+IPD,(\d),(\d+):').search(raw, i)
        if not m:
            rest += raw[i:]
            break
        rest += raw[i:m.start()]
        lid, n = int(m.group(1)), int(m.group(2))
        data.setdefault(lid, bytearray()).extend(raw[m.end():m.end() + n])
        i = m.end() + n
    return data, bytes(rest)


def text(b):
    data, rest = parse(b)
    s = rest.decode(errors='replace').strip()
    if data:
        s += '  [' + ', '.join(f'+IPD {k}: {len(v)} B' for k, v in sorted(data.items())) + ']'
    return s


def lines(b):
    return [l for l in b.decode(errors='replace').replace('\r', '').split('\n') if l]


def main():
    for k in (0, 1):
        threading.Thread(target=server, args=(k,), daemon=True).start()
    e = Esp()
    rep = []
    try:
        print(e.p.at('AT+KARABAS=esp'))
        e.cmd('AT')
        e.cmd('AT+CIFSR')
        e.cmd('AT+CIPCLOSE', 3, b'\r\n')
        e.cmd('AT+CIPMUX?')
        r = e.cmd('AT+CIPMUX=1')
        rep.append(f'CIPMUX=1 with no link: {lines(r)[-1:]}')
        e.raw = bytearray()
        r0 = e.cmd(f'AT+CIPSTART=0,"TCP","{PC}",{PORT}', 10)
        r1 = e.cmd(f'AT+CIPSTART=1,"TCP","{PC}",{PORT + 1}', 10)
        rep.append(f'CIPSTART 0: {lines(parse(r0)[1])}; CIPSTART 1 (link 0 streaming): {lines(parse(r1)[1])}')
        # sends while both stream: where do the frames of the other link fall?
        for k in (0, 1):
            mark = len(e.raw)
            os.write(e.p.fd, f'AT+CIPSEND={k},{len(UP[k])}\r\n'.encode())
            e.rd(3, b'> ')
            os.write(e.p.fd, UP[k])
            e.rd(4, b'SEND OK')
            seg = bytes(e.raw[mark:])
            p, s = seg.find(b'> '), seg.find(b'SEND OK')
            frames = [m.start() for m in re.finditer(rb'\+IPD,', seg)]
            rep.append(f'CIPSEND={k}: "> " at {p}, SEND OK at {s}, +IPD frames {len(frames)} '
                       f'(before "> " {sum(f < p for f in frames)}, between {sum(p < f < s for f in frames)}, '
                       f'after {sum(f > s for f in frames)}); text {lines(parse(seg)[1])[:6]}')
        e.rd(10, b'1,CLOSED')              # server 1 closes 9 s after its connect
        e.rd(2)
        r = e.cmd('AT+CIPSTATUS', 3)
        rep.append(f'CIPSTATUS (link 0 open): {lines(parse(r)[1])}')
        # what a core that expects CIPMUX=0 meets now
        for c in ('AT+CIPMUX=0', 'AT+CIPCLOSE', f'AT+CIPSTART="TCP","{PC}",{PORT + 1}'):
            r = e.cmd(c, 5, b'\r\n\r\n')
            r += e.rd(1)
            rep.append(f'mux 1, link 0 open: {c} -> {lines(parse(r)[1])}')
        r = e.cmd('AT+CIPCLOSE=5', 5)
        rep.append(f'CIPCLOSE=5: {lines(parse(r)[1])}')
        r = e.cmd('AT+CIPMUX=0')
        rep.append(f'CIPMUX=0 after closing all: {lines(r)}')
        e.cmd('AT+CIPMUX?')
        e.rd(1)
        open(os.path.join(LOG, 'mux_raw.bin'), 'wb').write(e.raw)
        data, rest = parse(bytes(e.raw))
        for k in (0, 1):
            d = bytes(data.get(k, b''))
            rep.append(f'link {k} down: {len(d)} / {len(DOWN[k])} B, equal {d == DOWN[k]}')
            rep.append(f'link {k} up: server got {len(ev.get(f"rx{k}", b""))} / {len(UP[k])} B, '
                       f'equal {ev.get(f"rx{k}") == UP[k]}')
        rep.append(f'text lines outside frames: {sorted(set(lines(rest)))[:30]}')
    finally:
        ev['close0'] = ev['close1'] = 1
        e.cmd('AT+CIPCLOSE=5', 3, b'\r\n')
        e.cmd('AT+CIPMUX=0', 3)
        e.cmd('AT+CIPMUX?', 3)
        print(e.p.at('AT+KARABAS=uart'))
        e.p.close()
    time.sleep(0.5)
    print('\n'.join(srv_log))
    print('\n'.join(rep))


main()

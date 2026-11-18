#!/usr/bin/env python3
"""Ф0.3: the ESP8266 AT behaviour that the term core depends on, from the PC, through the boot core
(AT+KARABAS=esp bridge of the firmware, kg_board.py Port). A TCP server on this PC is the remote host.
usage: esp_probe.py <pc ip> [port 23230] [logdir]
Never stores anything in the module: no AT+UART_DEF / SAVETRANSLINK / RESTORE / CWJAP; current-only
AT+CIPMUX=0, AT+CIPMODE (back to 0 at the end), AT+CIPCLOSE.
Bridge limits (firmware serial_bridge.c): host -> ESP bytes go through kg_crlf (CR / LF -> CR LF), so
payloads sent to the ESP avoid CR / LF; ESP -> host is raw."""
import os, socket, sys, threading, time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'karabas-go-tools', 'board'))
import kg_board  # noqa: E402

PC = sys.argv[1]
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 23230
LOG = sys.argv[3] if len(sys.argv) > 3 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'sim', 'build', 'esp')
os.makedirs(LOG, exist_ok=True)

DOWN = bytes(range(256)) + b'+++' + b'\xff\xfb\x01\xff\xfd\x03' + b'AT\r\n' + bytes(range(255, -1, -1))  # all bytes, "+++", telnet IAC
UP = bytes(b for b in range(256) if b not in (10, 13)) + b'+++xyz\xff\xff'                          # no CR / LF (bridge)
BULK = 32768

ev = {}
srv_log = []


def server():
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(('0.0.0.0', PORT))
    s.listen(2)
    s.settimeout(60)
    for test in ('ipd', 'trans'):
        try:
            c, a = s.accept()
        except socket.timeout:
            srv_log.append(f'{test}: no connection')
            return
        srv_log.append(f'{test}: connect from {a[0]}')
        c.settimeout(0.2)
        rx = bytearray()
        c.sendall(DOWN)
        t0 = time.time()
        if test == 'ipd':
            c.sendall(bytes((i * 7) & 255 for i in range(BULK)))
        ev[test + '_sent'] = t0
        end = time.time() + (12 if test == 'ipd' else 6)
        while time.time() < end and not ev.get(test + '_close_now'):
            try:
                d = c.recv(4096)
                if not d:
                    break
                rx += d
            except socket.timeout:
                pass
        ev[test + '_rx'] = bytes(rx)
        c.close()
        ev[test + '_closed'] = time.time()
        srv_log.append(f'{test}: closed by server, got {len(rx)} bytes')


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
                d = os.read(self.p.fd, 4096)
                got += d
                if until and until in got:
                    break
        self.raw += got
        return bytes(got)

    def cmd(self, line, seconds=5, until=b'OK\r\n'):
        os.write(self.p.fd, line.encode() + b'\r\n')
        r = self.rd(seconds, until)
        print(f'> {line}\n{r.decode(errors="replace").strip()}')
        return r


def ipd_payload(raw):
    """the data of the +IPD,n: frames in raw, and the rest"""
    out, i, rest = bytearray(), 0, bytearray()
    while True:
        j = raw.find(b'+IPD,', i)
        if j < 0:
            rest += raw[i:]
            break
        rest += raw[i:j]
        k = raw.find(b':', j)
        n = int(raw[j + 5:k])
        out += raw[k + 1:k + 1 + n]
        i = k + 1 + n
    return bytes(out), bytes(rest)


def main():
    threading.Thread(target=server, daemon=True).start()
    e = Esp()
    rep = []
    try:
        print(e.p.at('AT+KARABAS=esp'))
        e.cmd('AT')
        e.cmd('AT+GMR')
        e.cmd('AT+CIFSR')
        e.cmd('AT+UART_CUR?')
        e.cmd('AT+CIPSTATUS')
        e.cmd('AT+CIPCLOSE', 3, b'\r\n')
        e.cmd('AT+CIPMUX=0')
        e.cmd('AT+CIPMODE=0')
        # --- +IPD mode
        e.raw = bytearray()
        r = e.cmd(f'AT+CIPSTART="TCP","{PC}",{PORT}', 10, b'OK\r\n')
        t0 = time.time()
        got = e.rd(10, None)  # DOWN + BULK arrive as +IPD frames, then the server keeps the link 12 s
        t_all = time.time() - t0
        os.write(e.p.fd, f'AT+CIPSEND={len(UP)}\r\n'.encode())
        r = e.rd(3, b'> ')
        os.write(e.p.fd, UP)
        r += e.rd(3, b'SEND OK')
        print('CIPSEND:', r.decode(errors='replace').strip()[-60:])
        r = e.rd(8, b'CLOSED')  # the server closes 12 s after the connect
        open(os.path.join(LOG, 'ipd_raw.bin'), 'wb').write(e.raw)
        data, rest = ipd_payload(bytes(e.raw))
        exp = DOWN + bytes((i * 7) & 255 for i in range(BULK))
        rep.append(f'+IPD down: {len(data)} / {len(exp)} bytes, equal {data == exp}, ~{len(data) / t_all / 1024:.1f} KB/s '
                   f'over {t_all:.1f} s (incl. waiting)')
        rep.append(f'+IPD up: server got {len(ev.get("ipd_rx", b""))} / {len(UP)} bytes, equal {ev.get("ipd_rx") == UP}')
        rep.append(f'+IPD remote close: {"CLOSED" if b"CLOSED" in rest else "NOT reported"}; '
                   f'other text {sorted(set(l for l in rest.decode(errors="replace").split(chr(13) + chr(10)) if l))[:8]}')
        # --- transparent mode
        e.raw = bytearray()
        e.cmd('AT+CIPMODE=1')
        e.cmd(f'AT+CIPSTART="TCP","{PC}",{PORT}', 10, b'OK\r\n')
        e.cmd('AT+CIPSEND', 3, b'>')
        got = e.rd(3)
        rep.append(f'trans down: {len(got)} bytes, starts with DOWN {got.startswith(DOWN)}')
        os.write(e.p.fd, UP)
        got = e.rd(6)  # the server closes ~6 s after the connect
        after = e.rd(4)
        rep.append(f'trans remote close: module printed {after[:80]!r} after close (rx before {got[:40]!r})')
        rep.append(f'trans up: server got {len(ev.get("trans_rx", b""))} / {len(UP)}, equal {ev.get("trans_rx") == UP}')
        time.sleep(1.1)
        os.write(e.p.fd, b'+++')
        time.sleep(1.1)
        r = e.rd(1)
        e.cmd('AT', 3)
        open(os.path.join(LOG, 'trans_raw.bin'), 'wb').write(e.raw)
    finally:
        e.cmd('AT+CIPMODE=0', 3)
        e.cmd('AT+CIPCLOSE', 3, b'\r\n')
        e.cmd('AT+CIPSTATUS', 3)
        print(e.p.at('AT+KARABAS=uart'))
        e.p.close()
    print('\n'.join(srv_log))
    print('\n'.join(rep))


main()

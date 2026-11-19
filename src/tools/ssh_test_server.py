#!/usr/bin/env python3
"""Ф3 test SSH server (asyncssh) for the SSH client of the term core (src/fw/ssh.c): host test test_ssh and the
tb_soc sims. Only on 127.0.0.1, test accounts only:
  user "test", password "secret" (password + keyboard-interactive), or a public key of --authorized
The shell: prints TERM and the size, then commands (one per line):
  big N     N bytes of text (window adjust: N > 512 KB)
  utf8      some Cyrillic / CJK / box drawing
  exit N    the shell exits with status N
  anything else is echoed back as "got: ..."
usage: ssh_test_server.py [--port 2222] [--keydir DIR] [--authorized FILE] [--rekey-bytes N] [--kbi-only] [--log]
"""
import argparse, asyncio, os, sys, warnings
warnings.simplefilter('ignore')
import asyncssh

ap = argparse.ArgumentParser()
ap.add_argument('--port', type=int, default=2222)
ap.add_argument('--keydir', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'sim', 'ssh_keys'))
ap.add_argument('--authorized', default=None)
ap.add_argument('--rekey-bytes', type=int, default=None)
ap.add_argument('--kbi-only', action='store_true', help='no "password" method: keyboard-interactive only')
ap.add_argument('--pw-only', action='store_true', help='no keyboard-interactive: "password" only')
ap.add_argument('--log', action='store_true')
a = ap.parse_args()

os.makedirs(a.keydir, exist_ok=True)
hk = os.path.join(a.keydir, 'host_ed25519')
if not os.path.exists(hk):
    asyncssh.generate_private_key('ssh-ed25519').write_private_key(hk)


class Server(asyncssh.SSHServer):
    def connection_made(self, conn):
        if a.log: print('connection', conn.get_extra_info('peername'), file=sys.stderr, flush=True)

    def auth_completed(self):
        if a.log: print('auth ok', file=sys.stderr, flush=True)

    def begin_auth(self, username):
        return True

    def password_auth_supported(self):
        return not a.kbi_only

    def validate_password(self, username, password):
        return username == 'test' and password == 'secret'

    def kbdint_auth_supported(self):
        return not a.pw_only

    def get_kbdint_challenge(self, username, lang, submethods):
        return ('Test login', 'Answer two questions', '', [('Password: ', False), ('Your colour: ', True)])

    def validate_kbdint_response(self, username, responses):
        return username == 'test' and len(responses) == 2 and responses[0] == 'secret' and responses[1] == 'green'

    def public_key_auth_supported(self):
        return a.authorized is not None


async def shell(process):
    w, h, _, _ = process.get_terminal_size()
    process.stdout.write(f'TERM={process.get_terminal_type()} size={w}x{h}\r\n')
    try:
        while True:
            line = (await process.stdin.readline()).strip()
            if a.log: print('line:', line, file=sys.stderr, flush=True)
            if line.startswith('big'):
                n = int(line.split()[1])
                chunk = ''.join(f'{i:07d} the quick brown fox jumps over the lazy dog 0123456789\r\n' for i in range(64))
                sent = 0
                while sent < n:
                    part = chunk[:n - sent]
                    process.stdout.write(part)
                    sent += len(part)
                    await process.stdout.drain()
                process.stdout.write('\r\nBIG DONE\r\n')
            elif line == 'utf8':
                process.stdout.write('Привет, мир! 你好 ┌─┐ ★\r\n')
            elif line.startswith('exit'):
                p = line.split()
                process.exit(int(p[1]) if len(p) > 1 else 0)
                return
            else:
                process.stdout.write(f'got: {line}\r\n')
    except (asyncssh.BreakReceived, asyncssh.TerminalSizeChanged, ConnectionError):
        process.exit(1)


async def main():
    opts = dict(server_host_keys=[hk], process_factory=shell,
                encryption_algs=['chacha20-poly1305@openssh.com'],
                kex_algs=['curve25519-sha256', 'curve25519-sha256@libssh.org'])
    if a.authorized: opts['authorized_client_keys'] = a.authorized
    if a.rekey_bytes: opts['rekey_bytes'] = a.rekey_bytes
    await asyncssh.create_server(Server, '127.0.0.1', a.port, **opts)
    print(f'ssh test server on 127.0.0.1:{a.port}', file=sys.stderr, flush=True)
    await asyncio.Event().wait()

asyncio.run(main())

#!/usr/bin/env python3
"""Ф3: SSH through the ESP8266 model in the RTL SoC sim (term_soc + the real firmware) against the asyncssh test
server (src/tools/ssh_test_server.py) on 127.0.0.1:2224. The save file of the sim (--sd) holds a user key and the
known_hosts entry of the server (made here: a key of the sim only, never on a board), so the firmware logs in by
publickey without questions (SW bit 29: SSH to 127.0.0.1:2224 as "test"); then "utf8" Enter is typed.
Checks: the server saw the login and the line, the firmware kept the store (the seed was replaced).
usage: run_f3.py [frames 150] [key frame 120] [exit: also type "exit 0", the end of the session is on the shot]"""
import os, socket, subprocess, sys, time, base64
HERE = os.path.dirname(os.path.abspath(__file__))
W = os.path.join(HERE, 'build', 'f3'); os.makedirs(W, exist_ok=True)
FR = int(sys.argv[1]) if len(sys.argv) > 1 else 150
KF = int(sys.argv[2]) if len(sys.argv) > 2 else 120
EXIT = len(sys.argv) > 3 and sys.argv[3] == 'exit'
FW = os.path.join(HERE, '..', 'fw')
T = os.path.join(FW, 'build-host', 'test_ssh')
import warnings; warnings.simplefilter('ignore')
import asyncssh

# the server and its host key
keys = os.path.join(W, 'keys'); os.makedirs(keys, exist_ok=True)
hk = os.path.join(keys, 'host_ed25519')
if not os.path.exists(hk):
    asyncssh.generate_private_key('ssh-ed25519').write_private_key(hk)
host_pub = base64.b64decode(asyncssh.read_private_key(hk).export_public_key('openssh').split()[1])[-32:]
# the user key of the sim: a seed file; its public key line via test_ssh -p
seed_f = os.path.join(W, 'user.seed')
if not os.path.exists(seed_f):
    with open(seed_f, 'wb') as f: f.write(os.urandom(32))
seed = open(seed_f, 'rb').read()
if not os.path.exists(T):
    sys.exit('build the host test first: src/sim/run_f3_host.sh')
pub = subprocess.run([T, '127.0.0.1', '1', 'x', '-k', seed_f, '-p'], capture_output=True, text=True, env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0')).stdout
auth = os.path.join(W, 'authorized'); open(auth, 'w').write(pub)

# the save file: 34 sectors (store.h): the phonebook empty, S_KEYS 17, S_HOSTS 18
img = bytearray(34 * 512)
img[0:8] = b'TERMBOOK'; img[8] = 1; img[9] = 0
k = 17 * 512
img[k:k + 8] = b'TERMKEYS'; img[k + 8] = 1; img[k + 9] = 1
img[k + 16:k + 48] = seed
old_rng_seed = os.urandom(32); img[k + 48:k + 80] = old_rng_seed
h = 18 * 512
img[h:h + 9] = b'127.0.0.1'; img[h + 64] = 2224 & 255; img[h + 65] = 2224 >> 8; img[h + 66] = 1; img[h + 67:h + 99] = host_pub
sd = os.path.join(W, 'term.sav'); open(sd, 'wb').write(img)

log = open(os.path.join(W, 'server.log'), 'w')
srv = subprocess.Popen([sys.executable, '-u', os.path.join(HERE, '..', 'tools', 'ssh_test_server.py'), '--port', '2224', '--keydir', keys,
                        '--authorized', auth, '--pw-only', '--log'], stdout=log, stderr=log)
try:
    for _ in range(40):
        if 'server on' in open(log.name).read(): break
        time.sleep(0.25)
    # keys: u t f 8 Enter (HID usages 0x18 0x17 0x09 0x25 0x28)
    # then "exit 0" Enter: the shell exits, the server closes the channel and the TCP connection
    seq = ['18', '17', '09', '25', '28'] + (['08', '1b', '0c', '17', '2c', '27', '28'] if EXIT else [])
    keys_arg = ','.join(f'{KF + 2 * i}:{u}' for i, u in enumerate(seq))
    t0 = time.time()
    r = subprocess.run([os.path.join(HERE, 'obj_soc', 'tb_soc'), os.path.join(FW, 'build-hw-rv32imO2', 'term.bin'),
                        os.path.join(HERE, 'build', 'font', 'font_full.bin'), '--esp', '--sw', '0x20000000', '--frames', str(FR),
                        '--sd', sd, '--at-log', os.path.join(W, 'at.log'), '--keys', keys_arg, '--shot', str(FR - 1), os.path.join(W, 'f3.ppm')],
                       capture_output=True, text=True)
    print(r.stdout[-300:])
    print(f'sim {time.time() - t0:.0f} s')
    time.sleep(0.5)
finally:
    srv.terminate()
slog = open(log.name).read()
print(slog[-600:])
out = open(sd, 'rb').read()
checks = {'login by publickey': 'auth ok' in slog, 'typed line': 'line: utf8' in slog,
          'seed replaced': out[k + 48:k + 80] != old_rng_seed, 'user key kept': out[k + 16:k + 48] == seed}
ok = True
for n, v in checks.items():
    ok &= v
    print(f'{n:20} {"ok" if v else "MISSING"}')
subprocess.run([sys.executable, os.path.join(HERE, 'ppm2png.py'), os.path.join(W, 'f3.ppm'), os.path.join(W, 'f3.png')])
sys.exit(0 if ok else 1)

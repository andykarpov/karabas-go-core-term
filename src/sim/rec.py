#!/usr/bin/env python3
"""Ф0.1: record terminal output streams (pty 48 x 128, TERM=xterm-256color) for bench_vterm.
usage: rec.py <outdir>   -> outdir/{htop,mc,utf8,colors,ls}.bin
The streams are made on the host on demand and not committed (htop / ls show the host)."""
import os, pty, select, signal, sys, time, struct, fcntl, termios, random, tempfile

ROWS, COLS = 48, 128


def run(argv, keys, out, env_extra=None, cwd=None):
    """keys: list of (delay_s, bytes) sent in order; the output until the process exits (or 3 s after the last key)."""
    pid, fd = pty.fork()
    if pid == 0:
        env = dict(os.environ, TERM='xterm-256color', LANG='C.UTF-8', LC_ALL='C.UTF-8', COLUMNS=str(COLS), LINES=str(ROWS))
        if env_extra:
            env.update(env_extra)
        if cwd:
            os.chdir(cwd)
        os.execvpe(argv[0], argv, env)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack('HHHH', ROWS, COLS, 0, 0))
    os.kill(pid, signal.SIGWINCH)
    data = bytearray()
    t0 = time.time()
    plan = []
    t = 0.0
    for d, k in keys:
        t += d
        plan.append((t, k))
    end = t + 3.0
    while True:
        now = time.time() - t0
        while plan and plan[0][0] <= now:
            os.write(fd, plan.pop(0)[1])
        if now > end:
            break
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            try:
                b = os.read(fd, 65536)
            except OSError:
                break
            if not b:
                break
            data += b
    try:
        os.kill(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    os.waitpid(pid, 0)
    open(out, 'wb').write(data)
    print(f'{os.path.basename(out)}: {len(data)} bytes')


def utf8_text():
    random.seed(1)
    words = ['Привет, мир', 'Съешь же ещё этих мягких французских булок', 'Γειά σου κόσμε', 'Hello, world',
             'こんにちは世界', '你好，世界', '안녕하세요', 'Ünïcödé façade naïve', 'שלום עולם', 'مرحبا بالعالم',
             '─│┌┐└┘├┤┬┴┼ ═║╔╗╚╝ ░▒▓█ ▲►▼◄', '⠁⠃⠉⠙⠑⠋⠛⠓ ←↑→↓ ✓✗ ★☆', 'é ä combining']
    lines = []
    while sum(len(l) for l in lines) < 120000:
        lines.append(' '.join(random.choice(words) for _ in range(random.randint(1, 5))))
    return '\n'.join(lines).encode() + b'\n'


def colors_text():
    random.seed(2)
    out = []
    while sum(len(x) for x in out) < 300000:
        if random.random() < 0.5:
            out.append(f'\x1b[38;5;{random.randrange(256)}m\x1b[48;5;{random.randrange(256)}m')
        else:
            out.append(f'\x1b[38;2;{random.randrange(256)};{random.randrange(256)};{random.randrange(256)}m')
        if random.random() < 0.1:
            out.append(random.choice(['\x1b[1m', '\x1b[3m', '\x1b[4m', '\x1b[7m', '\x1b[0m']))
        out.append(random.choice(['█▓▒░', 'abc', 'Цвет', ' ', '●']) * random.randint(1, 4))
        if random.random() < 0.03:
            out.append('\x1b[0m\r\n')
    return ''.join(out).encode()


def main():
    od = sys.argv[1]
    os.makedirs(od, exist_ok=True)
    work = tempfile.mkdtemp(dir=od, prefix='work_')
    for n in ['readme.txt', 'Документ.odt', 'фото_2026.jpg', 'notes.md', 'архив.tar.xz', 'データ.bin', 'src', 'build']:
        p = os.path.join(work, n)
        if n in ('src', 'build'):
            os.mkdir(p)
        else:
            open(p, 'wb').write(os.urandom(random.randint(10, 5000)))
    for i in range(60):
        open(os.path.join(work, 'src', f'file_{i:03d}.c'), 'w').write('int x;\n' * i)
    home = os.path.join(work, 'home')
    os.mkdir(home)
    open(os.path.join(od, 'utf8_src.txt'), 'wb').write(utf8_text())
    open(os.path.join(od, 'colors_src.txt'), 'wb').write(colors_text())
    run(['htop', '-d', '3', '-u', 'nobody'], [(5.0, b'q')], os.path.join(od, 'htop.bin'), {'HOME': home})
    mc_keys = [(1.5, b'')] + [(0.05, b'\x1b[B')] * 8 + [(0.2, b'\r')] + [(0.05, b'\x1b[B')] * 40 + \
              [(0.2, b'\t')] + [(0.05, b'\x1b[B')] * 5 + [(0.3, b'\x1b[21~'), (0.5, b'\r')]
    run(['mc', '--nosubshell', '-c', work, work], mc_keys, os.path.join(od, 'mc.bin'), {'HOME': home})
    run(['cat', os.path.join(od, 'utf8_src.txt')], [], os.path.join(od, 'utf8.bin'))
    run(['cat', os.path.join(od, 'colors_src.txt')], [], os.path.join(od, 'colors.bin'))
    run(['sh', '-c', 'ls -la --color=always /usr/lib | head -3000'], [], os.path.join(od, 'ls.bin'))


main()

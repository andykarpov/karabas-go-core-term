#!/usr/bin/env python3
"""Ф1: pixel check of term_video. Makes a random screen (cells in an SRAM image as tscreen.h lays them
out, a shuffled row table, a palette, a cursor), runs obj_video/tb_video on it and compares the frame
with a Python rendering of the same rules (docs in term_video.v).
usage: ref_video.py <font_full.bin> [seed 1] [workdir build/video]"""
import os, random, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
font = open(sys.argv[1], 'rb').read()
seed = int(sys.argv[2]) if len(sys.argv) > 2 else 1
wd = sys.argv[3] if len(sys.argv) > 3 else os.path.join(HERE, 'build', 'video')
os.makedirs(wd, exist_ok=True)
rnd = random.Random(seed)
ROWS, COLS = 48, 128
sram = bytearray(4 << 20)
sram[0x200000:0x200000 + len(font)] = font

# rows: a permutation of ring rows (0x100000 + p * 1024) and alt rows (0x1c0000 + y * 1024)
rowtab = []
for y in range(ROWS):
    if rnd.random() < 0.3:
        rowtab.append((0x1c0000 >> 10) + y)
    else:
        rowtab.append((0x100000 >> 10) + rnd.randrange(768))
# distinct rows (a row shown twice would be fine, but keep them distinct)
seen = set()
for y in range(ROWS):
    while rowtab[y] in seen:
        rowtab[y] = (0x100000 >> 10) + rnd.randrange(768)
    seen.add(rowtab[y])

pal = [rnd.randrange(1 << 24) for _ in range(256)]
deffg, defbg = 0xd0d0d0, 0x101820
cps_narrow = list(range(0x20, 0x7f)) + list(range(0x410, 0x450)) + [0x2500, 0x2502, 0x250c, 0x2588, 0x2591, 0x2800 + 0x55, 0x3b1, 0]
cps_wide = [0x4e2d, 0x6587, 0xac00, 0x3042, 0xff21]


def color():
    r = rnd.random()
    if r < 0.2:
        return 0x100
    if r < 0.6:
        return rnd.randrange(256)
    return 0x8000 | rnd.randrange(1 << 15)


cells = [[None] * COLS for _ in range(ROWS)]
for y in range(ROWS):
    x = 0
    while x < COLS:
        attr = 0
        if rnd.random() < 0.4:
            attr = rnd.randrange(256) & ~0x40 if rnd.random() < 0.9 else rnd.randrange(256)
        fg, bg = color(), color()
        if x > 0 and x < COLS - 1 and rnd.random() < 0.08:
            cp = rnd.choice(cps_wide)
            cells[y][x] = (cp | 1 << 21 | attr << 22, fg | bg << 16)
            cells[y][x + 1] = (0x1fffff, 0)
            x += 2
            continue
        if rnd.random() < 0.02:
            cp = 0x1f600 + rnd.randrange(16)  # above U+FFFF -> U+FFFD
        else:
            cp = rnd.choice(cps_narrow)
        cells[y][x] = (cp | attr << 22, fg | bg << 16)
        x += 1
for y in range(ROWS):
    base = rowtab[y] << 10
    for x in range(COLS):
        struct.pack_into('<II', sram, base + x * 8, *cells[y][x])
cur_x, cur_y = rnd.randrange(COLS), rnd.randrange(ROWS)
cursor = cur_x | cur_y << 8 | 1 << 15  # block, no blink
# the mouse pointer (Ф5): anywhere, the edges too (clipped)
ptr_x, ptr_y = rnd.choice([rnd.randrange(1024), 1015, 1023, 0]), rnd.choice([rnd.randrange(768), 760, 767, 0])
ARROW = ["B", "BB", "BWB", "BWWB", "BWWWB", "BWWWWB", "BWWWWWB", "BWWWWWWB", "BWWWWWWWB", "BWWWWWWWWB",
         "BWWWWWBBBBB", "BWWBWWB", "BWB BWWB", "BB  BWWB", "     BWWB", "      BB"]
open(os.path.join(wd, 'sram.bin'), 'wb').write(sram)
with open(os.path.join(wd, 'regs.txt'), 'w') as f:
    for y in range(ROWS):
        f.write(f'row {y:x} {rowtab[y]:x}\n')
    for i in range(256):
        f.write(f'pal {i:x} {pal[i]:x}\n')
    f.write(f'deffg {deffg:x}\ndefbg {defbg:x}\ncursor {cursor:x}\n')
    f.write(f'pointer {1 << 31 | ptr_y << 16 | ptr_x:x}\n')

FRAMES = 2
r = subprocess.run([os.path.join(HERE, 'obj_video', 'tb_video'), os.path.join(wd, 'sram.bin'), os.path.join(wd, 'regs.txt'),
                    os.path.join(wd, 'rtl.ppm'), str(FRAMES)], capture_output=True, text=True)
if r.returncode:
    print(r.stdout, r.stderr)
    sys.exit(1)

# ---- the reference
blink_on = (FRAMES - 1) >> 5 & 1


def rgb555(c):
    r5, g5, b5 = c >> 10 & 31, c >> 5 & 31, c & 31
    return (r5 << 3 | r5 >> 2) << 16 | (g5 << 3 | g5 >> 2) << 8 | (b5 << 3 | b5 >> 2)


def res(c, default):
    if c & 0x8000:
        return rgb555(c & 0x7fff)
    if c & 0x100:
        return default
    return pal[c & 0xff]


img = bytearray(1024 * 768 * 3)
prev_attr, prev_fg, prev_bg, prev_addr = 0, 0, 0, 0
for line in range(768):
    y, g = line >> 4, line & 15
    for x in range(COLS):
        w0, w1 = cells[y][x]
        cp = w0 & 0x1fffff
        cont = cp == 0x1fffff
        if cont:
            addr = prev_addr
        elif cp > 0xffff:
            addr = 0xfffd
        else:
            addr = cp
        prev_addr = addr
        gl = struct.unpack_from('<H', font, addr * 32 + g * 2)[0]
        if cont:
            attr, fgc, bgc = prev_attr, prev_fg, prev_bg
        else:
            attr, fgc, bgc = w0 >> 22, w1 & 0xffff, w1 >> 16
            prev_attr, prev_fg, prev_bg = attr, fgc, bgc
        f, b = res(fgc, deffg), res(bgc, defbg)
        pat = gl & 0xff if cont else gl >> 8
        if attr & 8:
            pat >>= (15 - g) >> 2
        if attr & 1:
            pat |= pat >> 1
        ul = attr >> 1 & 3
        if ul in (1, 3) and g == 14:
            pat = 0xff
        if ul == 2 and g in (13, 15):
            pat = 0xff
        if attr & 0x80 and g == 8:
            pat = 0xff
        if attr & 0x40 or (attr & 0x10 and not blink_on):
            pat = 0
        if attr & 0x20:
            f, b = b, f
        if y == cur_y and x == cur_x:
            f, b = b, f
        for i in range(8):
            c = f if pat >> (7 - i) & 1 else b
            o = (line * 1024 + x * 8 + i) * 3
            img[o:o + 3] = bytes((c >> 16, c >> 8 & 255, c & 255))
for dy, l in enumerate(ARROW):
    for dx, ch in enumerate(l):
        X, Y = ptr_x + dx, ptr_y + dy
        if ch in 'BW' and X < 1024 and Y < 768:
            o = (Y * 1024 + X) * 3
            img[o:o + 3] = b'\xff\xff\xff' if ch == 'W' else b'\x00\x00\x00'
open(os.path.join(wd, 'ref.ppm'), 'wb').write(b'P6 1024 768 255\n' + bytes(img))

d = open(os.path.join(wd, 'rtl.ppm'), 'rb').read()
rtl = d[d.index(b'255\n') + 4:]
bad = [i // 3 for i in range(0, len(img), 3) if rtl[i:i + 3] != img[i:i + 3]]
if not bad:
    print(f'seed {seed}: frame == reference (1024 x 768)')
    sys.exit(0)
rows_bad = sorted(set(p // 1024 >> 4 for p in bad))
print(f'seed {seed}: {len(bad)} pixels differ; text rows {rows_bad[:12]}; first at x {bad[0] % 1024} line {bad[0] // 1024}')
sys.exit(1)

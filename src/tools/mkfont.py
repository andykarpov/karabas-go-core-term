#!/usr/bin/env python3
"""The font of the term core from GNU Unifont (.hex, GPL-2+ with the font embedding exception).
usage: mkfont.py <unifont_all-X.hex[.gz]> <outdir>
  outdir/font_full.bin   2 MB: codepoint U+0000..U+FFFF x 32 bytes = 16 halfwords (little-endian), one per
                         glyph row, bit 15 = the left pixel; narrow glyphs in bits 15..8 (the file on SD,
                         loaded to SRAM 0x200000)
  outdir/base_XXXX.bin   the base blocks (BASE below), the same layout, for the ROMS of the .kg at
                         0x200000 + first codepoint * 32
  outdir/base.json       the "roms" entries of core.json for the base blocks
Codepoints without a glyph and the control codes (U+0000..001F, U+007F..009F: Unifont draws them as
boxes; an erased cell of the screen is codepoint 0): zeros. U+FFFD is drawn by the RTL for codepoints
above U+FFFF."""
import gzip, json, os, sys

# the base blocks: Latin, Greek, Cyrillic, punctuation, symbols, arrows, box drawing, blocks, braille
BASE = [(0x0000, 0x024F), (0x0370, 0x03FF), (0x0400, 0x052F), (0x2000, 0x206F), (0x20A0, 0x20CF),
        (0x2100, 0x218F), (0x2190, 0x21FF), (0x2200, 0x22FF), (0x2300, 0x23FF), (0x2500, 0x259F),
        (0x25A0, 0x25FF), (0x2600, 0x26FF), (0x2700, 0x27BF), (0x2800, 0x28FF), (0xFFFD, 0xFFFD)]
FONT_BASE = 0x200000


def load(path):
    op = gzip.open if path.endswith('.gz') else open
    g = {}
    with op(path, 'rt') as f:
        for line in f:
            line = line.strip()
            if not line or ':' not in line:
                continue
            cp, hx = line.split(':', 1)
            cp = int(cp, 16)
            if cp > 0xFFFF:
                continue
            if len(hx) == 32:      # 8 x 16
                rows = [int(hx[i:i + 2], 16) << 8 for i in range(0, 32, 2)]
            elif len(hx) == 64:    # 16 x 16
                rows = [int(hx[i:i + 4], 16) for i in range(0, 64, 4)]
            else:
                continue
            g[cp] = b''.join(r.to_bytes(2, 'little') for r in rows)
    return g


def main():
    src, od = sys.argv[1], sys.argv[2]
    os.makedirs(od, exist_ok=True)
    g = load(src)
    empty = bytes(32)
    for cp in list(range(0x00, 0x20)) + list(range(0x7f, 0xa0)):
        g.pop(cp, None)
    full = b''.join(g.get(cp, empty) for cp in range(0x10000))
    open(os.path.join(od, 'font_full.bin'), 'wb').write(full)
    roms = []
    n = 0
    for a, b in BASE:
        name = f'base_{a:04x}.bin'
        open(os.path.join(od, name), 'wb').write(full[a * 32:(b + 1) * 32])
        roms.append({'filename': f'roms/{name}', 'address': FONT_BASE + a * 32})
        n += b - a + 1
    json.dump(roms, open(os.path.join(od, 'base.json'), 'w'), indent=1)
    print(f'{len(g)} BMP glyphs; full {len(full)} bytes; base {n} glyphs = {n * 32} bytes in {len(BASE)} blocks')


main()

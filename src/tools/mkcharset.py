#!/usr/bin/env python3
"""The 8-bit charsets of the term core (Python codecs): bytes 0x80..0xff -> Unicode, as a C table for
src/fw/charset.c (0x00..0x7f are ASCII in all of them; the control codes stay controls).
usage: mkcharset.py > fw/charset_tab.h"""
import codecs
SETS = [('cp437', 'CP437'), ('cp866', 'CP866'), ('koi8_r', 'KOI8-R'), ('cp1251', 'CP1251'), ('latin_1', 'ISO-8859-1')]
print('/* made by src/tools/mkcharset.py: bytes 0x80..0xff of each charset as Unicode (0xfffd: none) */')
print(f'#define CHARSET_N {len(SETS) + 1}')
print('static const char *const charset_names[CHARSET_N] = { "UTF-8", ' + ', '.join(f'"{n}"' for _, n in SETS) + ' };')
print(f'static const uint16_t charset_tab[{len(SETS)}][128] = {{')
for cod, name in SETS:
    vals = []
    for b in range(0x80, 0x100):
        try:
            vals.append(ord(bytes([b]).decode(cod)))
        except UnicodeDecodeError:
            vals.append(0xfffd)
    print(f'    /* {name} */ {{')
    for i in range(0, 128, 8):
        print('        ' + ' '.join(f'0x{v:04x},' for v in vals[i:i + 8]))
    print('    },')
print('};')

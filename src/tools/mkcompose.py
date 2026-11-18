#!/usr/bin/env python3
"""The canonical compositions of the BMP (Unicode NFC, Python unicodedata) as a C table for
src/fw/compose.c: { mark, base, composed } sorted by (mark, base). usage: mkcompose.py > fw/compose_tab.h"""
import sys, unicodedata
pairs = []
for c in range(0x10000):
    ch = chr(c)
    d = unicodedata.decomposition(ch)
    if not d or d.startswith('<'):
        continue
    parts = [int(x, 16) for x in d.split()]
    if len(parts) != 2 or max(parts) > 0xffff:
        continue
    if unicodedata.normalize('NFC', chr(parts[0]) + chr(parts[1])) != ch:
        continue   # a composition exclusion
    pairs.append((parts[1], parts[0], c))
pairs.sort()
print(f'/* made by src/tools/mkcompose.py (Unicode {unicodedata.unidata_version}): {len(pairs)} pairs */')
print('static const uint16_t compose_tab[][3] = {')
for i in range(0, len(pairs), 4):
    print('    ' + ' '.join('{0x%04x,0x%04x,0x%04x},' % p for p in pairs[i:i + 4]))
print('};')

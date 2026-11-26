#!/usr/bin/env python3
"""mkdat.py TILESBW.XPM OUT.DAT -- the tile art file the host sends (tilesbw.dat).

TILESBW.XPM is images/tilesbw.xpm of the Micropolis GPL release
(micropolis-activity): 960 black and white 16x16 tiles, one under the other.
OUT.DAT has 15360 rows of 2 bytes, high byte first; the leftmost pixel is
0x8000 and a black pixel ('#') is a 1 bit.
"""
import re
import sys

xpm, out = sys.argv[1:3]
lines = re.findall(r'"([^"]*)"', open(xpm).read())
w, h, nc = map(int, lines[0].split()[:3])
assert w == 16 and h == 15360
data = bytearray()
for s in lines[1 + nc:1 + nc + h]:
    v = 0
    for x in range(16):
        if s[x] == '#':
            v |= 0x8000 >> x
    data += bytes([v >> 8, v & 0xff])
open(out, 'wb').write(data)
print(out, len(data), 'bytes')

#!/usr/bin/env python3
"""mktiles.py TILESBW.XPM ANIMTAB.H OUTDIR
Writes OUTDIR/tiles.h (the 960 16x16 mono tiles, compressed) and
OUTDIR/anitab.h (the next frame of each animated tile), K&R C for dmdcc.
In the tiles, '#' (black in the XPM) is a 1 bit; the leftmost pixel is 0x8000.

Compressed form (unsigned char mc_art[]), tile after tile:
  0xFF lo hi      = the same as tile number (hi<<8|lo)
  else N (0..16)  = N different rows follow (2 bytes each, hi first), then a
                    16-bit mask (hi first): bit r set = row r is a new row;
                    clear rows repeat the row above (row 0 above = 0)
"""
import sys, re, subprocess
xpm, anim, out = sys.argv[1:4]
lines = re.findall(r'"([^"]*)"', open(xpm).read())
w, h, nc, cpp = map(int, lines[0].split()[:4])
rows = lines[1 + nc:]
assert w == 16 and h == 15360 and len(rows) == h
tiles = []
for t in range(960):
    tile = []
    for r in range(16):
        s = rows[t * 16 + r]; v = 0
        for x in range(16):
            if s[x] == '#': v |= 0x8000 >> x
        tile.append(v)
    tiles.append(tuple(tile))
seen = {}; data = bytearray()
for t, tile in enumerate(tiles):
    if tile in seen:
        o = seen[tile]; data += bytes([0xFF, o & 0xFF, o >> 8]); continue
    seen[tile] = t
    mask = 0; new = []; prev = 0
    for r, v in enumerate(tile):
        if v != prev: mask |= 1 << r; new.append(v)
        prev = v
    data.append(len(new))
    for v in new: data += bytes([v >> 8, v & 0xFF])
    data += bytes([mask >> 8, mask & 0xFF])
with open(out + '/tiles.h', 'w') as f:
    f.write('/* tiles.h - made by mktiles.py from tilesbw.xpm of Micropolis (GPL v3,\n'
            ' * Copyright (C) 1989 - 2007 Electronic Arts Inc.): %d tiles, %d unique,\n'
            ' * %d bytes (raw: %d). See mktiles.py for the format. */\n' % (len(tiles), len(seen), len(data), 960 * 32))
    f.write('#define MC_ARTLEN %d\nunsigned char mc_art[MC_ARTLEN] = {\n' % len(data))
    for i in range(0, len(data), 16):
        f.write('\t' + ', '.join('%d' % b for b in data[i:i + 16]) + ',\n')
    f.write('};\n')
src = subprocess.run(['clang', '-E', '-P', '-x', 'c', anim], capture_output=True, text=True, check=True).stdout
m = re.search(r'aniTile\s*\[\s*1024\s*\]\s*=\s*\{([^}]*)\}', src)
ani = [int(v) for v in re.findall(r'\d+', m.group(1))]
ani += [0] * (1024 - len(ani))   # C fills the rest with 0, as the original does
with open(out + '/anitab.h', 'w') as f:
    f.write('/* anitab.h - made by mktiles.py from animtab.h of Micropolis (GPL v3,\n'
            ' * Copyright (C) 1989 - 2007 Electronic Arts Inc.): the next frame of each tile. */\n')
    f.write('short mc_ani[960] = {\n')
    for i in range(0, 960, 16):
        f.write('\t' + ', '.join('%d' % v for v in ani[i:i + 16]) + ',\n')
    f.write('};\n')
print('tiles', len(tiles), 'unique', len(seen), 'bytes', len(data), 'animated', sum(1 for i in range(960) if ani[i] != i))

#!/usr/bin/env python3
"""mkicons.py -- the tool palette of Micropolis for the DMD 5620.

16x16 1 bit icons, drawn here for this port, in the style of classic
Macintosh tool palettes (own drawings).  2 columns of framed
buttons with a shadow, on a grey ground.  The host sends the icons ('I').

Writes (run it in mcity/):
  term/palette.h  the layout for mcity.m: PALW, ICONSH, icon[]
  host/icons.h    icondata[]: each icon tool w h n(2), n PackBits bytes
  a preview PNG   (-p FILE)

Usage: python3 -I tools/mkicons.py [-p preview.png]
"""
import math
import struct
import sys
import zlib

# toolname[] in mcity.c: 0 Query 1 Bulldozer 2 Road 3 Rail 4 Wire 5 Park
# 6 Residential 7 Commercial 8 Industrial 9 Police 10 Fire 11 Stadium
# 12 Seaport 13 Coal power 14 Nuclear 15 Airport
TOOL = {"qry": 0, "dozer": 1, "road": 2, "rail": 3, "wire": 4, "park": 5,
        "res": 6, "com": 7, "ind": 8, "pol": 9, "fire": 10, "stad": 11,
        "port": 12, "coal": 13, "nuc": 14, "air": 15}

# the rows, as the Mac palette, + nuclear and query
ROWS = [["dozer", "road"], ["wire", "rail"], ["park", "res"], ["com", "ind"],
        ["pol", "fire"], ["stad", "coal"], ["port", "air"], ["nuc", "qry"]]
N = 16          # icon size
MARGIN = 6      # around the buttons
PITCH = 24      # button to button: 1 frame, 1 white, 16, 1 white, 1 frame, 1 shadow, 3
PALW = 2 * MARGIN + PITCH + N + 6       # the last column is the line on the right
ICONSH = 2 * MARGIN + len(ROWS) * PITCH - 3

# 1 bit drawings, '#' = ink
DRAW = {
"dozer": """
................
................
........####....
........#..#.#..
........#..#.#..
.#......#######.
.##....#########
.#.#..##########
.#..############
.#.#..##########
.##.............
......#########.
.....#.#.#.#.#.#
.....#.#.#.#.#.#
......#########.
................
""",
"road": """
................
................
................
................
################
.#.#.#.#.#.#.#.#
#.#.#.#.#.#.#.#.
.#.#.#.#.#.#.#.#
#.#.#.#.#.#.#.#.
.#.#.#.#.#.#.#.#
#.#.#.#.#.#.#.#.
################
................
................
................
................
""",
"wire": """
...#............
#######.........
#..#..#.........
...#............
.#####..........
.#.#.#...#......
...#...#####....
...#...#.#.#....
...#.....#......
...#.....#...#..
...#.....#..###.
...#.....#..#.#.
...#.....#...#..
...#.....#...#..
...#.....#...#..
..###...###.###.
""",
"rail": """
................
................
................
.##..##..##..##.
.##..##..##..##.
################
################
.##..##..##..##.
.##..##..##..##.
################
################
.##..##..##..##.
.##..##..##..##.
................
................
................
""",
"park": """
..####..........
.#.#.##.........
#.#.#.##........
.#.#.#.#........
#.#.#.##........
.#.#.#.#........
..##.##..#######
...##....#.#.#.#
...##....#.#.#.#
...##....#.#.#.#
...##....#.#.#.#
...##....#.###.#
...##....#.....#
...##....#.....#
...##....#.....#
################
""",
"res": """
................
.......#...##...
......###..##...
.....#####.##...
....#########...
...#########....
..###########...
.#############..
###############.
.#...........#..
.#.##.###.##.#..
.#.##.#.#.##.#..
.#....#.#....#..
.#....#.#....#..
.#############..
................
""",
"com": """
.....######.....
.....#....#.....
.....#.##.#.....
.....#....#.....
.....#.##.#.....
######....#.....
#....#.##.#####.
#.##.#....#...#.
#....#.##.#.#.#.
#.##.#....#...#.
#....#.##.#.#.#.
#.##.#....#...#.
#....#.##.#.#.#.
#.##.#....#...#.
#....#.##.#.#.#.
################
""",
"ind": """
.........##.##..
........#..#..#.
.........##.##..
...........##...
...........##...
...........##...
...........##...
#...#...#..##...
##..##..##.##...
###.###.###.##..
################
#..............#
#.##.##.##.##..#
#..............#
#..............#
################
""",
"pol": """
.##############.
.#............#.
.#.###..###...#.
.#.#.#..#..#..#.
.#.###..#..#..#.
.#.#....#..#..#.
.#.#....###...#.
.#............#.
.#............#.
..#..........#..
..#..........#..
...#........#...
....#......#....
.....##..##.....
.......##.......
................
""",
"fire": """
................
...#####........
......##........
......####......
.....####.#.....
....######.#....
....######..#...
....######..#...
....#....#..#...
....#....#..#...
....######.###..
....######.###..
....######......
....######......
....######......
.....####.......
""",
"stad": """
................
.....#######....
...##.......##..
..#############.
.##############.
.#######..#####.
.#######..#####.
.##############.
.##############.
######.########.
#.....#########.
######.#######..
#.....#######...
######.###......
................
................
""",
"coal": """
..##.........##.
.#..#.......##..
..##.......##...
.##.......#####.
.##.........##..
.##........##...
.##.......##....
.##......##.....
.##......#......
#####...........
#...#...........
#...############
#.#.#..........#
#...#.##.##.##.#
#.#.#..........#
################
""",
"port": """
......####......
.....#....#.....
......####......
.......##.......
...##########...
.......##.......
.......##.......
.......##.......
.#.....##.....#.
.##....##....##.
.##....##....##.
..##...##...##..
...###.##.###...
.....######.....
.......##.......
................
""",
"air": """
.......##.......
......####......
......####......
.......##.......
......####......
....########....
..############..
################
###...####...###
.......##.......
.......##.......
.......##.......
.....######.....
....########....
....##....##....
................
""",
"qry": """
................
.....######.....
....##....##....
....##....##....
..........##....
.........##.....
........##......
.......##.......
.......##.......
.......##.......
................
.......##.......
.......##.......
................
................
................
""",
}


def trefoil(n=16):
    g = []
    c = (n - 1) / 2
    for y in range(n):
        row = ""
        for x in range(n):
            dx, dy = x - c, c - y
            r = math.hypot(dx, dy)
            a = math.degrees(math.atan2(dy, dx)) % 360
            blade = any(min(abs(a - k), 360 - abs(a - k)) < 30 for k in (30, 150, 270))
            row += "#" if r < 1.6 or (2.6 < r < 8.1 and blade) else "."
        g.append(row)
    return "\n".join(g)


DRAW["nuc"] = trefoil()


def bits(name):
    rows = DRAW[name].strip().split("\n")
    assert len(rows) == N and all(len(r) == N for r in rows), name
    return [[1 if ch == "#" else 0 for ch in r] for r in rows]


def layout():
    """name -> x, y of the icon (inside its button) in the palette"""
    return {name: (MARGIN + c * PITCH + 2, MARGIN + r * PITCH + 2)
            for r, row in enumerate(ROWS) for c, name in enumerate(row)}


def tobytes(bm):
    rb = (N + 7) // 8
    data = bytearray()
    for y in range(N):
        for j in range(rb):
            v = 0
            for k in range(8):
                x = j * 8 + k
                if x < N and bm[y][x]:
                    v |= 0x80 >> k
            data.append(v)
    return bytes(data)


def packbits(data):
    out = bytearray()
    i, n = 0, len(data)
    while i < n:
        j = i
        while j + 1 < n and data[j + 1] == data[i] and j - i < 127:
            j += 1
        if j - i >= 1:
            out.append(257 - (j - i + 1))
            out.append(data[i])
            i = j + 1
            continue
        k = i
        while k < n and k - i < 128:
            if k + 1 < n and data[k + 1] == data[k]:
                break
            k += 1
        if k == i:
            k = i + 1
        out.append(k - i - 1)
        out += data[i:k]
        i = k
    return bytes(out)


def unpackbits(p, size):
    out = bytearray()
    i = 0
    while len(out) < size:
        c = p[i]
        i += 1
        if c < 128:
            out += p[i:i + c + 1]
            i += c + 1
        elif c > 128:
            out += bytes([p[i]]) * (257 - c)
            i += 1
    return bytes(out)


def png(path, w, h, rows):
    raw = b"".join(b"\0" + bytes(r) for r in rows)

    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


def preview(path, pos, sel="road"):
    """the palette as mcity.m draws it: grey ground, framed buttons"""
    S = 4
    W, H = PALW, ICONSH
    ink = [[(x + y) & 1 for x in range(W)] for y in range(H)]

    def rect(x0, y0, x1, y1, v):
        for y in range(y0, y1):
            for x in range(x0, x1):
                ink[y][x] = v
    for name, (x, y) in pos.items():
        s = name == sel
        rect(x - 2, y - 2, x + N + 2, y + N + 2, 1)
        rect(x - 1, y - 1, x + N + 1, y + N + 1, s)
        rect(x + N + 2, y - 1, x + N + 3, y + N + 3, 1)
        rect(x - 1, y + N + 2, x + N + 3, y + N + 3, 1)
        b = bits(name)
        for yy in range(N):
            for xx in range(N):
                ink[y + yy][x + xx] = b[yy][xx] ^ s
    rect(W - 1, 0, W, H, 1)
    img = []
    for y in range(H):
        row = []
        for x in range(W):
            row += [0 if ink[y][x] else 255] * S
        img += [row] * S
    png(path, W * S, H * S, img)


def main():
    args = sys.argv[1:]
    opts = dict(zip(args[0::2], args[1::2]))
    pos = layout()
    lines = ["\t{ %2d, %3d, %3d }," % (TOOL[n], pos[n][0], pos[n][1])
             for n in sorted(pos, key=lambda n: TOOL[n])]
    with open("term/palette.h", "w") as f:
        f.write("/* palette.h -- made by tools/mkicons.py: the tool palette.\n"
                " * icon: tool, x y of the %dx%d icon in the palette (its button\n"
                " * is 2 pixels more each side, its shadow 1 more right and below). */\n" % (N, N))
        f.write("#define PALW\t%d\n#define ICONSH\t%d\n#define ICONN\t%d\n\n"
                % (PALW, ICONSH, N))
        f.write("struct icon icon[NTOOL] = {\n%s\n};\n" % "\n".join(lines))
    blobs = []
    for n in sorted(pos, key=lambda n: TOOL[n]):
        raw = tobytes(bits(n))
        p = packbits(raw)
        assert unpackbits(p, len(raw)) == raw
        blobs.append(bytes([TOOL[n], N, N]) + struct.pack(">H", len(p)) + p)
    allb = b"".join(blobs)
    with open("host/icons.h", "w") as f:
        f.write("/* icons.h -- made by tools/mkicons.py.  icondata: the palette icons,\n"
                " * each tool w h n(2) and n PackBits bytes (rows of (w+7)/8 bytes,\n"
                " * 1 = ink). */\n")
        f.write("#define NICONDATA %d\n" % len(allb))
        f.write("static unsigned char icondata[NICONDATA] = {\n")
        for i in range(0, len(allb), 16):
            f.write("  " + ",".join("%d" % b for b in allb[i:i + 16]) + ",\n")
        f.write("};\n")
    print("PALW %d ICONSH %d, %d bytes of icons" % (PALW, ICONSH, len(allb)))
    if "-p" in opts:
        preview(opts["-p"], pos)


if __name__ == "__main__":
    main()

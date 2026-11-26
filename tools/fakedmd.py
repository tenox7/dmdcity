#!/usr/bin/env python3
"""fakedmd.py MCITY DATADIR [city.cty] -- a stand-in for mcity.m on the Mac.

Runs the host program on pipes, decodes what it sends like the terminal
would, and reports when the view's cells, the view's art and the whole map
were complete. Then it uses a few tools and checks that the changes come
back. Writes view.png (the view, 16 px tiles, real art or the 4x4 stand-in)
and map.png (the overall map from the 4x4 table) next to this script's
output dir (argv OUT=...).
"""
import os, sys, subprocess, time, struct, select, zlib

mcity, data = sys.argv[1], sys.argv[2]
city = sys.argv[3] if len(sys.argv) > 3 else None
outdir = os.environ.get('OUT', '.')
W, H, NT = 120, 100, 960
VX, VY, VW, VH = 36, 20, 48, 60

env = dict(os.environ, MCHOME=data, MCLOG=os.path.join(outdir, 'host.log'), MCWIN=os.environ.get('MCWIN', '128'))
args = [mcity] + ([city] if city else [])
p = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=env)
os.set_blocking(p.stdout.fileno(), False)

cells = [None] * (W * H)
art = {}
small = None
status = ''
msgs = []
buf = bytearray()
t0 = time.time()
events = {}
nbytes = 0
acked = 0
counts = {}

def send(b):
    p.stdin.write(bytes(b)); p.stdin.flush()

send(b'v' + bytes([VX, VY, VW, VH]))

def parse():
    """Decode whole messages from buf; return False if one is incomplete."""
    global small, status
    while buf:
        op = chr(buf[0]); i = 1
        try:
            if op == 'h':
                need = 2; i = 2
            elif op == 'p':
                need = 1 + 2 * NT
                if len(buf) < need: return
                small = [struct.unpack('>H', buf[1 + 2 * t:3 + 2 * t])[0] for t in range(NT)]
                i = need
            elif op == 'a':
                if len(buf) < 5: return
                t, mask = struct.unpack('>HH', buf[1:5]); i = 5
                rows = []; prev = 0
                for r in range(16):
                    if mask & (1 << r):
                        if len(buf) < i + 2: return
                        prev = struct.unpack('>H', buf[i:i + 2])[0]; i += 2
                    rows.append(prev)
                art[t] = rows
            elif op == 'b':
                if len(buf) < 4: return
                idx = struct.unpack('>H', buf[1:3])[0]; n = buf[3]; i = 4
                vals = []
                while len(vals) < n:
                    if len(buf) < i + 1: return
                    k = buf[i]; i += 1
                    if k < 128:
                        if len(buf) < i + 2: return
                        v = struct.unpack('>H', buf[i:i + 2])[0]; i += 2
                        vals += [v] * (k + 1)
                    else:
                        m = k - 127
                        if len(buf) < i + 2 * m: return
                        vals += [struct.unpack('>H', buf[i + 2 * j:i + 2 * j + 2])[0] for j in range(m)]
                        i += 2 * m
                assert len(vals) == n, (len(vals), n)
                for j, v in enumerate(vals): cells[idx + j] = v
            elif op in 'sm':
                z = buf.find(0, 1)
                if z < 0: return
                s = buf[1:z].decode('latin-1'); i = z + 1
                if op == 's': status = s
                else: msgs.append((round(time.time() - t0, 1), s))
            elif op == 'g':
                if len(buf) < 3: return
                i = 3
            elif op in 'kq':
                i = 1
            else:
                raise ValueError('bad op %r at byte %d' % (op, nbytes))
        except IndexError:
            return
        counts[op] = counts.get(op, 0) + 1
        del buf[:i]

def check_events():
    now = round(time.time() - t0, 1)
    vis = [cells[x * H + y] for x in range(VX, VX + VW) for y in range(VY, VY + VH)]
    if 'view cells' not in events and all(v is not None for v in vis):
        events['view cells'] = now
    if 'view cells' in events and 'view art' not in events and all((v & 1023) in art for v in vis):
        events['view art'] = now
    if 'map' not in events and all(v is not None for v in cells):
        events['map'] = now
    if 'map art' not in events and all(v is not None for v in cells) and all((v & 1023) in art for v in cells):
        events['map art'] = now
    if 'all art' not in events and len(art) == NT:
        events['all art'] = now

def pump(secs):
    global nbytes, acked
    end = time.time() + secs
    while time.time() < end:
        r, _, _ = select.select([p.stdout], [], [], 0.05)
        if r:
            d = p.stdout.read(65536)
            if d:
                buf.extend(d); nbytes += len(d); parse()
                while nbytes - acked >= 64:   # the DMD's 'a' for the host's window
                    send(b'a'); acked += 64
        check_events()

pump(float(os.environ.get('LOADSECS', '40')))
print('bytes %d in %.1f s; messages %s' % (nbytes, time.time() - t0, counts))
print('events (s):', events)
print('status:', status)

# use tools: residential zone, a road by dragging, query
def cellat(x, y): return cells[x * H + y]
x, y = VX + 10, VY + 10
before = [cellat(x + dx, y + dy) for dx in (-1, 0, 1) for dy in (-1, 0, 1)]
send(b'd' + bytes([7, x, y]))          # bulldoze first
pump(1.5)
send(b'd' + bytes([0, x, y]))          # residential, 3x3 round x, y
pump(2.0)
after = [cellat(x + dx, y + dy) for dx in (-1, 0, 1) for dy in (-1, 0, 1)]
print('zone at', x, y, 'before', [v & 1023 for v in before], 'after', [v & 1023 for v in after])
send(b'd' + bytes([9, VX + 2, VY + 30])); send(b'm' + bytes([VX + 20, VY + 30])); pump(2.0)
print('road row', [cellat(VX + i, VY + 30) & 1023 for i in range(2, 21)])
send(b'd' + bytes([4, x, y])); pump(1.5)
print('messages', msgs[-6:])
send(b'q'); pump(1.0)
p.wait(timeout=5)

# pictures
try:
    from PIL import Image
except ImportError:
    sys.exit(0)
img = Image.new('1', (VW * 16, VH * 16), 0)
px = img.load()
for cx in range(VW):
    for cy in range(VH):
        v = cells[(VX + cx) * H + VY + cy]
        if v is None: continue
        t = v & 1023
        for r in range(16):
            if t in art: row = art[t][r]
            else:
                nib = (small[t] >> (12 - 4 * (r // 4))) & 15
                row = 0
                for c in range(4):
                    if nib & (8 >> c): row |= 0xF000 >> (4 * c)
            for c in range(16):
                if row & (0x8000 >> c): px[cx * 16 + c, cy * 16 + r] = 1
img.save(os.path.join(outdir, 'view.png'))
m = Image.new('1', (W * 4, H * 4), 0); mp = m.load()
for x in range(W):
    for y in range(H):
        v = cells[x * H + y]
        if v is None: continue
        s = small[v & 1023]
        for r in range(4):
            for c in range(4):
                if s & (0x8000 >> (r * 4 + c)): mp[x * 4 + c, y * 4 + r] = 1
m.save(os.path.join(outdir, 'map.png'))
print('wrote view.png map.png')

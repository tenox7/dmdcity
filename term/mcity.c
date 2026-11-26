/* mcity.c -- Micropolis for the AT&T DMD 5620: the main window.
 *
 * Micropolis, Unix Version.  Copyright (C) 1989 - 2007 Electronic Arts Inc.
 * Licensed under the GNU GPL v3 or later, with the additional EA terms.
 * No trademark or publicity rights granted.  This is a modified version of
 * Micropolis; it is not the original program.
 *
 * The host (mcity on the 3B2) runs the simulation.  This program keeps the
 * whole 120 x 100 map and the tile art as a cache, and does by itself what
 * needs no simulation: drawing, scrolling, the animation of tiles, the
 * blinking of zones without power, the menus and the tool palette.  It
 * sends the host its view, the tools you use and the menu items you pick.
 * The other windows (map, graph, budget, ...) are layers of their own that
 * the host makes; the map window reads this program's view (share.h).
 *
 * Until the art of a tile comes, the tile shows as its 4 x 4 picture,
 * 4 times as big.
 *
 * From the host:  h v (hello)   p 960 x 2 (4x4 pictures)
 *   a t(2) mask(2) rows (art)   b i(2) n items (n cells from i)
 *   s text0 status   m text0 message   g x y (look here)   k bell   q
 *   D r c i (demand, signed)   O bits(2) (options)   T n (speed 0-3)   E (quake)
 *   I tool w h PackBits (a palette icon, rows of (w+7)/8 bytes, 1 = ink)
 * To the host:  v x y w h (view)  d tool x y  m x y (drag)  A addr(4)
 *   M menu item   q   r (send all again)   a (64 bytes read)
 *
 * K&R C for the DMD's cc (dmdcc).  mpx.h has macros with common names
 * (sw, min, max, add, point, string, wait, exit): do not use them as names.
 */
#include <dmd.h>
#include <font.h>
#include "anitab.h"
#include "share.h"

#define MW	120
#define MH	100
#define NCELL	12000
#define NT	960
#define LOMASK	01777
#define ZONEBIT	02000
#define ANIMBIT	04000
#define PWRBIT	0100000
#define NOCELL	0xffff
#define BOLT	827
#define TS	16
#define LINEH	14
#define HZ	60
#define ACKUNIT	64	/* send 'a' for every 64 bytes read: the host's window */
/* the palette's colors: lit ground and dark ink, as the tiles (sh.inv) */
#define PBG	(sh.inv ? F_STORE : F_CLR)
#define PINK	(sh.inv ? F_CLR : F_STORE)
#define PTXT	(sh.inv ? F_CLR : F_OR)
#define NTOOL	16
#define SB	16	/* a scroll bar's width */

/* not static and without values: bss, not sent down the line */
unsigned short cell[NCELL];	/* the map, as the host gave it (plus animation) */
unsigned short smallt[NT];	/* each tile at 4 x 4, a nibble a row */
unsigned char have[NT];		/* the real art came */
int nsmall;
Bitmap *art;			/* 32 x 7680: tiles 2t and 2t+1 side by side */
struct share sh;
int vx, vy, vw, vh;		/* the view, in tiles */
Point org;			/* its corner on the screen */
Rectangle view;			/* the view on the screen */
Rectangle vbar, hbar;		/* the scroll bars, right of and below the view */
int grab;			/* where button 1 went down: 1 view, 2 vbar, 3 hbar */
void vscroll();
int hy;				/* the top of the palette and the view */
int blink;			/* zones without power show the bolt */
int tool = 9;			/* the engine's road tool */
int nopal;			/* no palette */
int speed = 3, opts;
int dem[3];
char status[130], msg1[130], msg2[130];
long cells_in, bytes_in, acked;
int barx[6];			/* the menu bar: the titles' x */

/* ---- tools and menus ----------------------------------------------------- */

char *toolname[] = {
	"Query", "Bulldozer", "Road", "Rail", "Wire", "Park", "Residential",
	"Commercial", "Industrial", "Police", "Fire", "Stadium", "Seaport",
	"Coal power", "Nuclear", "Airport", 0
};
char toolstate[] = { 4, 7, 9, 8, 6, 13, 0, 1, 2, 5, 3, 12, 14, 15, 16, 17 };
short toolcost[] = { 0, 1, 10, 20, 5, 10, 100, 100, 100, 500, 500, 5000,
	3000, 3000, 5000, 10000 };
Menu toolmenu = { toolname };

/* The palette: 2 columns of 16x16 icons in framed buttons on a grey
 * ground, in the classic Macintosh style.  The host sends the icons ('I');
 * tools/mkicons.py draws them and makes palette.h.  The tool's name and
 * cost are on the status line. */
struct icon {
	char tool;		/* in toolname[] */
	short x, y;		/* of the icon in the palette */
	Bitmap *b;		/* the icon, when it came */
};
#include "palette.h"
Texture grey = {
	0xAAAAAAAA, 0x55555555, 0xAAAAAAAA, 0x55555555, 0xAAAAAAAA, 0x55555555,
	0xAAAAAAAA, 0x55555555, 0xAAAAAAAA, 0x55555555, 0xAAAAAAAA, 0x55555555,
	0xAAAAAAAA, 0x55555555, 0xAAAAAAAA, 0x55555555, 0xAAAAAAAA, 0x55555555,
	0xAAAAAAAA, 0x55555555, 0xAAAAAAAA, 0x55555555, 0xAAAAAAAA, 0x55555555,
	0xAAAAAAAA, 0x55555555, 0xAAAAAAAA, 0x55555555, 0xAAAAAAAA, 0x55555555,
	0xAAAAAAAA, 0x55555555
};

char *bartitle[] = { "Micropolis", "Options", "Disasters", "Time", "Windows", 0 };
char *m0[] = { "about", "new city", "load city", "scenario", "save city",
	"save as", "quit", 0 };
char *m1[] = { "auto budget", "auto bulldoze", "disasters", "animation",
	"messages", "notices", "auto goto", "palette", "invert", 0 };
char *m2[] = { "monster", "fire", "flood", "meltdown", "tornado",
	"earthquake", 0 };
char *m3[] = { "pause", "slow", "medium", "fast", 0 };
char *m4[] = { "map", "graph", "budget", "evaluation", "notice",
	"redraw", 0 };
char **mitems[] = { m0, m1, m2, m3, m4 };
char mark[2][16][16];		/* menus 1 and 3 with marks */
char *mptr[16];
Menu barmenu = { mptr };
char *yn[] = { "quit", "keep playing", 0 };
Menu ynmenu = { yn };

int
toolidx()
{
	int i;

	for (i = 0; i < NTOOL; i++)
		if (toolstate[i] == tool)
			return (i);
	return (0);
}

/* ---- to the host --------------------------------------------------------- */

void
tohost(n, a, b, c, d)
int n, a, b, c, d;
{
	char buf[5];

	buf[1] = a;
	buf[2] = b;
	buf[3] = c;
	buf[4] = d;
	buf[0] = n >> 8;
	sendnchars(1 + (n & 0xff), buf);
}
#define H1(c)		tohost((c) << 8, 0, 0, 0, 0)
#define H2(c, a)	tohost(((c) << 8) | 1, a, 0, 0, 0)
#define H3(c, a, b)	tohost(((c) << 8) | 2, a, b, 0, 0)
#define H4(c, a, b, d)	tohost(((c) << 8) | 3, a, b, d, 0)
#define H5(c, a, b, d, e)	tohost(((c) << 8) | 4, a, b, d, e)

/* ---- the art bitmap ------------------------------------------------------ */

void
setrow(t, r, bits)
int t, r, bits;
{
	UWord *w;

	if (sh.inv)
		bits ^= 0xffff;
	w = (UWord *)art->base + ((t >> 1) * TS + r) * art->width;
	if (t & 1)
		*w = (*w & 0xffff0000) | (bits & 0xffff);
	else
		*w = (*w & 0xffff) | ((UWord)(bits & 0xffff) << 16);
}

/* the 4x4 picture of t, each pixel 4 x 4, as t's art */
void
preview(t)
int t;
{
	int r, c, nib, bits;

	for (r = 0; r < TS; r++) {
		nib = (smallt[t] >> (12 - 4 * (r / 4))) & 15;
		bits = 0;
		for (c = 0; c < 4; c++)
			if (nib & (8 >> c))
				bits |= 0xf000 >> (4 * c);
		setrow(t, r, bits);
	}
}

/* ---- drawing the view ---------------------------------------------------- */

int
shown(v)
int v;
{
	if (blink && (v & ZONEBIT) && !(v & PWRBIT))
		return (BOLT);
	return (v & LOMASK);
}

void
drawtile(t, p)
int t;
Point p;
{
	bitblt(art, Rect((t & 1) * TS, (t >> 1) * TS, (t & 1) * TS + TS,
		(t >> 1) * TS + TS), &display, p, F_STORE);
}

void
drawcell(cx, cy)
int cx, cy;
{
	int v = cell[cx * MH + cy], t;
	Point p;

	p.x = org.x + (cx - vx) * TS;
	p.y = org.y + (cy - vy) * TS;
	if (v == NOCELL) {
		rectf(&display, Rect(p.x, p.y, p.x + TS, p.y + TS),
			sh.inv ? F_STORE : F_CLR);
		return;
	}
	drawtile(shown(v), p);
}

int
inview(cx, cy)
int cx, cy;
{
	return (cx >= vx && cx < vx + vw && cy >= vy && cy < vy + vh);
}

void
drawview()
{
	int cx, cy;

	for (cx = vx; cx < vx + vw; cx++)
		for (cy = vy; cy < vy + vh; cy++)
			drawcell(cx, cy);
}

/* the cells that show tile t, after its art came */
void
redrawtile(t)
int t;
{
	int cx, cy, v;

	for (cx = vx; cx < vx + vw; cx++)
		for (cy = vy; cy < vy + vh; cy++) {
			v = cell[cx * MH + cy];
			if (v != NOCELL && shown(v) == t)
				drawcell(cx, cy);
		}
}

/* ---- the panel: menu bar, status, messages, palette, demand -------------- */

void
line(n, s)
int n;
char *s;
{
	int y = Drect.origin.y + 2 + n * LINEH;

	rectf(&display, Rect(Drect.origin.x, y, Drect.corner.x, y + LINEH), F_CLR);
	string(&defont, s, &display, Pt(Drect.origin.x + 4, y), F_OR);
}

void
drawbar()
{
	int i, x = Drect.origin.x + 4, y = Drect.origin.y + 2;

	rectf(&display, Rect(Drect.origin.x, y, Drect.corner.x, y + LINEH), F_CLR);
	for (i = 0; bartitle[i]; i++) {
		barx[i] = x;
		string(&defont, bartitle[i], &display, Pt(x, y), F_OR);
		x += strwidth(&defont, bartitle[i]) + 18;
	}
	barx[i] = x;
	rectf(&display, Rect(Drect.origin.x, y + LINEH + 1, Drect.corner.x,
		y + LINEH + 2), F_STORE);
}

void
drawstatus()
{
	char s[200];
	int i = toolidx();

	if (toolcost[i])
		sprintf(s, "%s   %s $%d", status, toolname[i], toolcost[i]);
	else
		sprintf(s, "%s   %s", status, toolname[i]);
	line(1, s);
}

void
drawmsgs()
{
	line(2, msg1);
	line(3, msg2);
}

void
message(s)
char *s;
{
	strcpy(msg1, msg2);
	strcpy(msg2, s);
	drawmsgs();
}

/* the demand for R, C and I: a box under the icons */
void
drawdemand()
{
	int x = Drect.origin.x + 6, y = hy + ICONSH + 2, cy, i, v, bx;
	Rectangle r;

	if (nopal || y + 105 > Drect.corner.y)
		return;
	r = Rect(x, y, x + 45, y + 104);
	rectf(&display, r, PINK);
	rectf(&display, inset(r, 1), PBG);
	rectf(&display, Rect(r.corner.x, y + 1, r.corner.x + 1, r.corner.y + 1), PINK);
	rectf(&display, Rect(x + 1, r.corner.y, r.corner.x + 1, r.corner.y + 1), PINK);
	cy = y + 46;
	rectf(&display, Rect(x + 3, cy, r.corner.x - 3, cy + 1), PINK);
	for (i = 0; i < 3; i++) {
		v = dem[i] * 2;
		if (v > 40)
			v = 40;
		if (v < -40)
			v = -40;
		bx = x + 5 + i * 13;
		if (v > 0)
			rectf(&display, Rect(bx, cy - v, bx + 9, cy), PINK);
		else if (v < 0)
			rectf(&display, Rect(bx, cy + 1, bx + 9, cy + 1 - v), PINK);
		string(&defont, i == 0 ? "R" : i == 1 ? "C" : "I", &display,
			Pt(bx, y + 88), PTXT);
	}
}

/* the screen rectangle of icon k */
Rectangle
iconrect(k)
int k;
{
	Point p;

	p = add(Pt(Drect.origin.x, hy), Pt(icon[k].x, icon[k].y));
	return (Rect(p.x, p.y, p.x + ICONN, p.y + ICONN));
}

/* icon k in its button: a frame, a shadow right and below.  The selected
 * one is inverted, as on the Mac. */
void
drawicon(k)
int k;
{
	struct icon *ic = &icon[k];
	Rectangle r;

	r = iconrect(k);
	rectf(&display, inset(r, -2), PINK);
	rectf(&display, inset(r, -1), PBG);
	rectf(&display, Rect(r.corner.x + 2, r.origin.y - 1, r.corner.x + 3,
		r.corner.y + 3), PINK);
	rectf(&display, Rect(r.origin.x - 1, r.corner.y + 2, r.corner.x + 3,
		r.corner.y + 3), PINK);
	if (ic->b)
		bitblt(ic->b, ic->b->rect, &display, r.origin, sh.inv ? F_CLR : F_OR);
	if (ic->tool == toolidx())
		rectf(&display, inset(r, -1), F_XOR);
}

void
drawpal()
{
	int k;

	if (nopal)
		return;
	texture(&display, Rect(Drect.origin.x, hy, Drect.origin.x + PALW - 1,
		Drect.corner.y), &grey, F_STORE);
	for (k = 0; k < NTOOL; k++)
		drawicon(k);
	rectf(&display, Rect(Drect.origin.x + PALW - 1, hy, Drect.origin.x + PALW,
		Drect.corner.y), PINK);
	drawdemand();
}

void
settool(t)
int t;
{
	int k, old = toolidx();

	tool = t;
	if (!nopal)
		for (k = 0; k < NTOOL; k++)
			if (icon[k].tool == old || icon[k].tool == toolidx())
				drawicon(k);
	drawstatus();
}

/* ---- scroll bars ---------------------------------------------------------- */

/* A bar: an arrow box at each end, a grey track, a white thumb as long as the
 * view is of the map.  Neighbors share their 1 pixel lines, as on the Mac. */
void
sbbox(r)
Rectangle r;
{
	rectf(&display, r, PINK);
	rectf(&display, inset(r, 1), PBG);
}

/* a filled arrow in box r: 0 up, 1 down, 2 left, 3 right */
void
sbarrow(r, dir)
Rectangle r;
int dir;
{
	int i, cx = r.origin.x + SB / 2, cy = r.origin.y + SB / 2;

	sbbox(r);
	for (i = 0; i < 5; i++)
		switch (dir) {
		case 0: rectf(&display, Rect(cx - i, cy - 3 + i, cx + i + 1, cy - 2 + i), PINK); break;
		case 1: rectf(&display, Rect(cx - i, cy + 1 - i, cx + i + 1, cy + 2 - i), PINK); break;
		case 2: rectf(&display, Rect(cx - 3 + i, cy - i, cx - 2 + i, cy + i + 1), PINK); break;
		case 3: rectf(&display, Rect(cx + 1 - i, cy - i, cx + 2 - i, cy + i + 1), PINK); break;
		}
}

/* the track of a bar, from the up (left) arrow's line to the down (right) one's:
 * start a, length n; the map size, the view's size and place */
void
sbtrack(vert, a, n, total, len, pos)
int vert, *a, *n, *total, *len, *pos;
{
	Rectangle r;

	r = vert ? vbar : hbar;
	*a = (vert ? r.origin.y : r.origin.x) + SB - 1;
	*n = (vert ? r.corner.y : r.corner.x) - SB + 1 - *a;
	*total = vert ? MH : MW;
	*len = vert ? vh : vw;
	*pos = vert ? vy : vx;
}

/* the thumb: start and size along the track */
void
sbthumb(vert, ts, sz)
int vert, *ts, *sz;
{
	int a, n, total, len, pos;

	sbtrack(vert, &a, &n, &total, &len, &pos);
	*sz = n * len / total;
	if (*sz < 10)
		*sz = 10;
	if (*sz > n)
		*sz = n;
	*ts = a + (total > len ? (long)(n - *sz) * pos / (total - len) : 0);
}

void
drawsb(vert)
int vert;
{
	Rectangle r, t;
	int ts, sz;

	r = vert ? vbar : hbar;
	if (vert ? r.corner.y - r.origin.y < 3 * SB : r.corner.x - r.origin.x < 3 * SB)
		return;
	rectf(&display, r, PINK);
	texture(&display, inset(r, 1), &grey, F_STORE);
	if (vert) {
		sbarrow(Rect(r.origin.x, r.origin.y, r.corner.x, r.origin.y + SB), 0);
		sbarrow(Rect(r.origin.x, r.corner.y - SB, r.corner.x, r.corner.y), 1);
	} else {
		sbarrow(Rect(r.origin.x, r.origin.y, r.origin.x + SB, r.corner.y), 2);
		sbarrow(Rect(r.corner.x - SB, r.origin.y, r.corner.x, r.corner.y), 3);
	}
	sbthumb(vert, &ts, &sz);
	t = vert ? Rect(r.origin.x, ts, r.corner.x, ts + sz) :
		Rect(ts, r.origin.y, ts + sz, r.corner.y);
	sbbox(t);
}

void
drawsbs()
{
	drawsb(1);
	drawsb(0);
	sbbox(Rect(view.corner.x, view.corner.y, view.corner.x + SB, view.corner.y + SB));
}

/* Button 1 held in a bar.  Arrows: 2 tiles, again and again.  The grey: a
 * page, until the thumb is under the mouse.  The thumb: the view follows it. */
int sbpart, sboff;
long sbnext;

void
sbmouse(vert, first, now)
int vert, first;
long now;
{
	int m = vert ? mouse.xy.y : mouse.xy.x, a, n, total, len, pos, ts, sz, d = 0;

	sbtrack(vert, &a, &n, &total, &len, &pos);
	sbthumb(vert, &ts, &sz);
	if (first) {
		sbpart = m < a + 1 ? 1 : m >= a + n - 1 ? 5 : m < ts ? 2 : m >= ts + sz ? 4 : 3;
		sboff = m - ts;
		sbnext = now;
	}
	if (sbpart == 3) {
		if (n > sz)
			d = (int)((long)(m - sboff - a) * (total - len) * 2 / (n - sz) + 1) / 2 - pos;
	} else {
		if (now < sbnext)
			return;
		sbnext = now + (first ? HZ / 3 : HZ / 15);
		if (sbpart == 1)
			d = -2;
		else if (sbpart == 5)
			d = 2;
		else if (sbpart == 2 && m < ts)
			d = -(len - 2);
		else if (sbpart == 4 && m >= ts + sz)
			d = len - 2;
	}
	if (d && vert)
		vscroll(0, d);
	else if (d)
		vscroll(d, 0);
}

/* ---- layout -------------------------------------------------------------- */

void
sendview()
{
	sh.vx = vx;
	sh.vy = vy;
	sh.vw = vw;
	sh.vh = vh;
	H5('v', vx, vy, vw, vh);
}

void
clampview()
{
	if (vx > MW - vw)
		vx = MW - vw;
	if (vy > MH - vh)
		vy = MH - vh;
	if (vx < 0)
		vx = 0;
	if (vy < 0)
		vy = 0;
}

void
redraw()
{
	rectf(&display, Drect, F_CLR);
	drawbar();
	drawstatus();
	drawmsgs();
	drawpal();
	rectf(&display, Rect(view.origin.x - 1, view.origin.y - 1, view.corner.x + SB,
		view.origin.y), PINK);	/* the frame: top and left, the bars do the rest */
	rectf(&display, Rect(view.origin.x - 1, view.origin.y - 1, view.origin.x,
		view.corner.y + SB), PINK);
	drawsbs();
	rectf(&display, view, sh.inv ? F_STORE : F_CLR);
	drawview();
}

void
layout()
{
	int x0 = Drect.origin.x + (nopal ? 1 : PALW);	/* the palette's line is the frame */
	int dw = Drect.corner.x - x0 - SB, dh;
	int cx = vx + vw / 2, cy = vy + vh / 2;

	hy = Drect.origin.y + 4 * LINEH + 8;
	dh = Drect.corner.y - hy - SB;
	vw = dw / TS;
	vh = dh / TS;
	if (vw > MW)
		vw = MW;
	if (vh > MH)
		vh = MH;
	if (vw < 1)
		vw = 1;
	if (vh < 1)
		vh = 1;
	vx = cx - vw / 2;
	vy = cy - vh / 2;
	clampview();
	org.x = x0;
	org.y = hy;
	view = Rect(org.x, org.y, org.x + vw * TS, org.y + vh * TS);
	vbar = Rect(view.corner.x, view.origin.y - 1, view.corner.x + SB, view.corner.y + 1);
	hbar = Rect(view.origin.x - 1, view.corner.y, view.corner.x + 1, view.corner.y + SB);
	sendview();
	redraw();
}

void
vscroll(dx, dy)
int dx, dy;
{
	int ox = vx, oy = vy;

	vx += dx;
	vy += dy;
	clampview();
	if (vx == ox && vy == oy)
		return;
	sendview();
	drawview();
	drawsbs();
}

void
lookat(cx, cy)
int cx, cy;
{
	vscroll(cx - vw / 2 - vx, cy - vh / 2 - vy);
}

void
invert()
{
	int i, n;
	UWord *w;

	sh.inv = !sh.inv;
	n = art->width * (art->rect.corner.y - art->rect.origin.y);
	for (w = (UWord *)art->base, i = 0; i < n; i++, w++)
		*w = ~*w;
	redraw();
}

void
quake()
{
	int i;

	for (i = 0; i < 16; i++) {	/* shake inside the view only */
		if (i & 1)
			bitblt(&display, Rect(view.origin.x, view.origin.y, view.corner.x - 6,
				view.corner.y), &display, add(view.origin, Pt(6, 0)), F_STORE);
		else
			bitblt(&display, Rect(view.origin.x + 6, view.origin.y, view.corner.x,
				view.corner.y), &display, view.origin, F_STORE);
		sleep(2);
	}
	rectf(&display, view, sh.inv ? F_STORE : F_CLR);
	drawview();
}

/* ---- from the host ------------------------------------------------------- */

int op;			/* the message being read, 0 = none */
int need, got;		/* bytes wanted in hb, bytes there */
unsigned char hb[8];
int phase, bidx, bleft, brep, blit, aT, aMask, aRow, aPrev, sLen;
char sbuf[130];
int iK, iLit, iRun, iRep, iPos, iSize, iRB, iH;	/* an icon coming */
Bitmap *iBm;

/* the next byte of icon iK's rows */
void
iput(c)
int c;
{
	int r = iPos / iRB, j = iPos % iRB;

	if (iBm && r < iH)
		*((UWord *)iBm->base + r * iBm->width + (j >> 2)) |=
			(UWord)(c & 0xff) << (24 - 8 * (j & 3));
	iPos++;
}

/* PackBits: n < 128, n+1 bytes follow; n > 128, the next byte 257-n times */
void
iconbyte(c)
int c;
{
	if (iLit > 0) {
		iput(c);
		iLit--;
	} else if (iRun) {
		while (iRep-- > 0 && iPos < iSize)
			iput(c);
		iRun = 0;
	} else if (c < 128)
		iLit = c + 1;
	else if (c > 128) {
		iRep = 257 - c;
		iRun = 1;
	}
	if (iPos < iSize)
		return;
	if (iK >= 0 && iBm) {
		if (icon[iK].b)
			bfree(icon[iK].b);
		icon[iK].b = iBm;
		if (!nopal)
			drawicon(iK);
	}
	op = 0;
}

void
gotcell(i, v)
int i, v;
{
	int cx = i / MH, cy = i % MH;

	if (i < 0 || i >= NCELL)
		return;
	cell[i] = v;
	cells_in++;
	if (inview(cx, cy))
		drawcell(cx, cy);
}

/* art rows: fill the rows that repeat; 1 if a new row is wanted */
int
artfill()
{
	while (aRow < TS && !(aMask & (1 << aRow)))
		setrow(aT, aRow++, aPrev);
	if (aRow < TS)
		return (1);
	have[aT] = 1;
	redrawtile(aT);
	return (0);
}

void
finish()
{
	op = 0;
	need = 1;
	got = 0;
}

int
sbyte(c)
int c;
{
	return (c > 127 ? c - 256 : c);
}

/* hb[] has need bytes: go on with the message */
void
step()
{
	int v = (hb[0] << 8) | hb[1], i;

	got = 0;
	switch (op) {
	case 'p':
		smallt[nsmall++] = v;
		if (nsmall < NT) {
			need = 2;
			return;
		}
		for (i = 0; i < NT; i++)
			if (!have[i])
				preview(i);
		sh.gotsmall = 1;
		finish();
		redraw();
		return;
	case 'a':
		if (phase == 0) {
			aT = v;
			aMask = (hb[2] << 8) | hb[3];
			aRow = 0;
			aPrev = 0;
			if (aT >= NT) {
				finish();
				return;
			}
			phase = 1;
			need = 2;
		} else {
			setrow(aT, aRow++, v);
			aPrev = v;
		}
		if (artfill())
			return;
		break;
	case 'b':
		switch (phase) {
		case 0:
			bidx = v;
			bleft = hb[2];
			phase = 1;
			need = 1;
			return;
		case 1:
			if (hb[0] < 128) {
				brep = hb[0] + 1;
				phase = 2;
			} else {
				blit = hb[0] - 127;
				phase = 3;
			}
			need = 2;
			return;
		case 2:
			while (brep-- > 0 && bleft > 0) {
				gotcell(bidx++, v);
				bleft--;
			}
			break;
		case 3:
			gotcell(bidx++, v);
			bleft--;
			if (--blit > 0 && bleft > 0)
				return;
			break;
		}
		if (bleft > 0) {
			phase = 1;
			need = 1;
			return;
		}
		break;
	case 'g':
		lookat(hb[0], hb[1]);
		break;
	case 'D':
		for (i = 0; i < 3; i++)
			dem[i] = sbyte(hb[i]);
		drawdemand();
		break;
	case 'O':
		opts = v;
		break;
	case 'T':
		speed = hb[0];
		break;
	case 'I':
		iK = -1;
		for (i = 0; i < NTOOL; i++)
			if (icon[i].tool == hb[0])
				iK = i;
		iRB = (hb[1] + 7) / 8;
		iH = hb[2];
		iSize = iRB * iH;
		iPos = iLit = iRun = 0;
		iBm = iSize > 0 ? balloc(Rect(0, 0, hb[1], hb[2])) : 0;
		if (iBm)
			rectf(iBm, iBm->rect, F_CLR);
		if (iSize == 0)
			break;
		phase = 1;		/* the rows come byte by byte: iconbyte() */
		return;
	}
	finish();
}

void
fromhost(c)
int c;
{
	bytes_in++;
	if (op == 0) {
		op = c;
		phase = 0;
		got = 0;
		switch (op) {
		case 'h': case 'T': need = 1; return;
		case 'p': need = 2; nsmall = 0; return;
		case 'a': need = 4; return;
		case 'b': need = 3; return;
		case 'g': case 'O': need = 2; return;
		case 'D': case 'I': need = 3; return;
		case 's': case 'm': sLen = 0; return;
		case 'k': ringbell(); op = 0; return;
		case 'E': quake(); op = 0; return;
		case 'q': sh.magic = 0; exit();
		}
		op = 0;			/* not ours: skip it */
		return;
	}
	if (op == 'h') {
		op = 0;
		return;
	}
	if (op == 'I' && phase == 1) {
		iconbyte(c);
		if (op == 0)
			finish();
		return;
	}
	if (op == 's' || op == 'm') {
		if (c != 0) {
			if (sLen < 128)
				sbuf[sLen++] = c;
			return;
		}
		sbuf[sLen] = 0;
		if (op == 's') {
			strcpy(status, sbuf);
			drawstatus();
		} else
			message(sbuf);
		op = 0;
		return;
	}
	hb[got++] = c;
	if (got >= need)
		step();
}

/* ---- by itself: animation and blinking ----------------------------------- */

void
animate()
{
	int i, v, n, cx, cy;

	for (i = 0; i < NCELL; i++) {
		v = cell[i];
		if (v == NOCELL || !(v & ANIMBIT))
			continue;
		n = (v & ~LOMASK) | mc_ani[v & LOMASK];
		if (n == v)
			continue;
		cell[i] = n;
		cx = i / MH;
		cy = i % MH;
		if (inview(cx, cy))
			drawcell(cx, cy);
	}
}

void
doblink()
{
	int cx, cy, v;

	blink = !blink;
	for (cx = vx; cx < vx + vw; cx++)
		for (cy = vy; cy < vy + vh; cy++) {
			v = cell[cx * MH + cy];
			if (v != NOCELL && (v & ZONEBIT) && !(v & PWRBIT))
				drawcell(cx, cy);
		}
}

void
speedtest()
{
	long t0 = realtime(), t1;
	char s[100];

	drawview();
	t1 = realtime();
	sprintf(s, "%d tiles drawn in %d/60 s", vw * vh, (int)(t1 - t0));
	message(s);
}

/* ---- main ---------------------------------------------------------------- */

/* Read what the host sent, and tell it how much: it sends no more than
 * 128 bytes ahead, so the layer's queue never fills (a full queue blocks
 * the xt channel for good). */
void
readhost()
{
	int c, i;

	for (i = 0; i < 2000 && (c = rcvchar()) != -1; i++)
		fromhost(c);
	while (bytes_in - acked >= ACKUNIT) {
		H1('a');
		acked += ACKUNIT;
	}
}

void
quit()
{
	long t0 = realtime();

	sh.magic = 0;			/* the map window stops */
	H1('q');
	while (realtime() - t0 < 10 * HZ) {	/* the host answers q */
		alarm(6);
		wait(RCV | ALARM);
		readhost();		/* q from the host: exit() */
	}
	exit();
}

/* a menu of the bar; marks for options and time */
void
barhit(n, but)
int n, but;
{
	char **it = mitems[n];
	int i, on;

	for (i = 0; it[i]; i++) {
		if (n == 1 || n == 3) {
			on = n == 3 ? (i == speed) : i < 7 ? ((opts >> i) & 1) :
				i == 7 ? !nopal : sh.inv;
			sprintf(mark[n == 3][i], "%c %s", on ? '*' : ' ', it[i]);
			mptr[i] = mark[n == 3][i];
		} else
			mptr[i] = it[i];
	}
	mptr[i] = 0;
	i = menuhit(&barmenu, but);
	if (i < 0)
		return;
	if (n == 0 && i == 6) {		/* the next press shows "quit / keep playing" */
		message("Quit?  Press a button for the menu.");
		while (mouse.buttons & 7)
			sleep(2);
		while (!(mouse.buttons & 7))
			sleep(2);
		but = (mouse.buttons & 4) ? 1 : (mouse.buttons & 2) ? 2 : 3;
		if (menuhit(&ynmenu, but) == 0)
			quit();
		return;
	}
	if (n == 1 && i == 7) {
		nopal = !nopal;
		layout();
		return;
	}
	if (n == 1 && i == 8) {
		invert();
		return;
	}
	if (n == 4 && i == 5) {
		redraw();
		return;
	}
	H3('M', n, i);
}

main()
{
	int c, i, b, lastb = 0, lx = -1, ly = -1, mx, my;
	long now, lastani = 0, lastblink = 0, a;

	request(KBD | MOUSE | RCV | SEND | ALARM);
	for (i = 0; i < NCELL; i++)
		cell[i] = NOCELL;
	art = balloc(Rect(0, 0, 32, TS * NT / 2));
	if (!art) {
		string(&defont, "mcity: not enough memory in the terminal",
			&display, add(Drect.origin, Pt(4, 4)), F_OR);
		sleep(5 * HZ);
		exit();
	}
	sh.inv = 1;
	rectf(art, art->rect, F_STORE);
	sh.cell = cell;
	sh.smallt = smallt;
	sh.gox = -1;
	sh.magic = SHMAGIC;
	a = (long)&sh;
	H5('A', (int)(a >> 24) & 0xff, (int)(a >> 16) & 0xff, (int)(a >> 8) & 0xff,
		(int)a & 0xff);
	strcpy(status, "Micropolis: waiting for the host ...");
	strcpy(msg2, "Button 1: menus, tools.  Button 2: tool menu.  Arrows: scroll.");
	finish();
	vx = 36;
	vy = 20;
	vw = 48;
	vh = 60;
	P->state &= ~(RESHAPED | MOVED);
	layout();

	for (;;) {
		if (P->state & (RESHAPED | MOVED)) {
			P->state &= ~(RESHAPED | MOVED);
			layout();
		}
		readhost();
		if (sh.gox >= 0) {		/* the map window was clicked */
			lookat(sh.gox, sh.goy);
			sh.gox = -1;
		}
		now = realtime();
		if (now - lastani >= HZ / 5) {
			lastani = now;
			animate();
		}
		if (now - lastblink >= HZ / 2) {
			lastblink = now;
			doblink();
		}
		while ((c = kbdchar()) != -1)
			switch (c) {
			case 'h': vscroll(-8, 0); break;
			case 'l': vscroll(8, 0); break;
			case 'k': vscroll(0, -8); break;
			case 'j': vscroll(0, 8); break;
			case 'H': vscroll(-32, 0); break;
			case 'L': vscroll(32, 0); break;
			case 'K': vscroll(0, -32); break;
			case 'J': vscroll(0, 32); break;
			case 'm': H3('M', 4, 0); break;
			case 'g': H3('M', 4, 1); break;
			case 'b': H3('M', 4, 2); break;
			case 'e': H3('M', 4, 3); break;
			case 'i': invert(); break;
			case ' ': H3('M', 3, speed ? 0 : 3); break;
			case 't': speedtest(); break;
			case 'q': quit(); break;
			case 033:		/* the arrow keys: ESC [ A */
				if (kbdchar() != '[')
					break;
				switch (kbdchar()) {
				case 'A': vscroll(0, -8); break;
				case 'B': vscroll(0, 8); break;
				case 'C': vscroll(8, 0); break;
				case 'D': vscroll(-8, 0); break;
				}
				break;
			}
		b = (own() & MOUSE) ? (mouse.buttons & 7) : 0;
		mx = mouse.xy.x;
		my = mouse.xy.y;
		if (!(b & 4))
			grab = 0;
		else if (!(lastb & 4))
			grab = ptinrect(mouse.xy, view) ? 1 : ptinrect(mouse.xy, vbar) ? 2 :
				ptinrect(mouse.xy, hbar) ? 3 : 0;
		if ((b & 5) && !(lastb & 5) && my < Drect.origin.y + LINEH + 3 &&
			ptinrect(mouse.xy, Drect)) {	/* the menu bar */
			for (i = 0; bartitle[i]; i++)
				if (mx >= barx[i] && mx < barx[i + 1])
					barhit(i, (b & 4) ? 1 : 3);
			b = 0;
		} else if ((b & 4) && grab >= 2) {
			sbmouse(grab == 2, !(lastb & 4), now);
		} else if ((b & 4) && grab == 1 && ptinrect(mouse.xy, view)) {
			mx = vx + (mx - org.x) / TS;
			my = vy + (my - org.y) / TS;
			if (!(lastb & 4))
				H4('d', tool, mx, my);
			else if (mx != lx || my != ly)
				H3('m', mx, my);
			lx = mx;
			ly = my;
		} else if ((b & 4) && !(lastb & 4) && !nopal && mx < Drect.origin.x + PALW &&
			my >= hy) {
			for (i = 0; i < NTOOL; i++)
				if (ptinrect(mouse.xy, inset(iconrect(i), -3)))
					settool(toolstate[icon[i].tool]);
		}
		if ((b & 2) && !(lastb & 2)) {
			toolmenu.prevhit = toolidx();
			i = menuhit(&toolmenu, 2);
			if (i >= 0)
				settool(toolstate[i]);
			b = 0;
		}
		if ((b & 1) && !(lastb & 1)) {
			barhit(4, 3);
			b = 0;
		}
		lastb = b;
		alarm(2);
		wait(KBD | RCV | ALARM);
	}
}

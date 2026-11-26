/* mcmap.c -- Micropolis for the AT&T DMD 5620: the overall map window.
 *
 * Micropolis, Unix Version.  Copyright (C) 1989 - 2007 Electronic Arts Inc.
 * Licensed under the GNU GPL v3 or later, with the additional EA terms.
 * No trademark or publicity rights granted.  This is a modified version of
 * Micropolis; it is not the original program.
 *
 * The host loads this program in a layer of its own: 32ld mcmap.m ADDR.
 * The host draws the map, 1 pixel a cell, in the kind this window asks
 * for, and sends the rows that changed.  ADDR is the struct share of
 * mcity.m (share.h): from there come the editor's view (the frame), and
 * there a click asks the editor to look somewhere.  A big window shows
 * the map at 2 pixels a cell.
 *
 * From the host:  h (hello)  r y PackBits (a row: 15 bytes, 1 = ink)  q
 * To the host:  H   O kind   a (64 bytes read)
 * Button 1 on the map: the editor looks there.  Button 2 or 3: the menu.
 */
#include <dmd.h>
#include <font.h>
#include "share.h"

#define MW	120
#define MH	100
#define RB	(MW / 8)	/* bytes a row */
#define ACKUNIT	64
#define OKPTR(p)	((unsigned long)(p) >= 0x700000L && (unsigned long)(p) < 0x7ffff0L)

struct share *sh;
Bitmap *mb;			/* the map, 1 pixel a cell, as the screen shows it */
Bitmap *mb2;			/* the same at 2 pixels a cell */
int sc;				/* pixels a cell: 1 or 2 */
Point org;			/* the map's corner on the screen */
int fx = -1, fy, fw, fh;	/* the view frame drawn */
int dy0 = MH, dy1 = -1;		/* rows that came, not on the screen yet */
int mode, lastinv, ch;
long bytes_in, acked;

char *mname[] = {
	"City", "Residential", "Commercial", "Industrial", "Power grid",
	"Transport", "Population", "Growth", "Traffic", "Pollution",
	"Crime", "Land value", "Fire cover", "Police cover", 0
};
char mitems[14][16];
char *mptr[15];
Menu mmenu = { mptr };

void
put1(c)
int c;
{
	char b[1];

	b[0] = c;
	sendnchars(1, b);
}

void
put2(c, d)
int c, d;
{
	char b[2];

	b[0] = c;
	b[1] = d;
	sendnchars(2, b);
}

int
alive()
{
	return (OKPTR(sh) && sh->magic == SHMAGIC);
}

/* ---- the bitmaps --------------------------------------------------------- */

/* byte k of row y of bitmap b (rows are 32 bit words, MSB left) */
void
setbyte(b, y, k, v)
Bitmap *b;
int y, k, v;
{
	UWord *w = (UWord *)b->base + y * b->width + (k >> 2);
	int s = 24 - 8 * (k & 3);

	*w = (*w & ~((UWord)0xff << s)) | ((UWord)(v & 0xff) << s);
}

int
getbyte(b, y, k)
Bitmap *b;
int y, k;
{
	return ((*((UWord *)b->base + y * b->width + (k >> 2)) >> (24 - 8 * (k & 3))) & 0xff);
}

/* nibble abcd -> aabbccdd */
unsigned char dbl[16] = {
	0x00, 0x03, 0x0c, 0x0f, 0x30, 0x33, 0x3c, 0x3f,
	0xc0, 0xc3, 0xcc, 0xcf, 0xf0, 0xf3, 0xfc, 0xff
};

/* row y of mb at 2 pixels a cell in mb2 */
void
double2(y)
int y;
{
	int k, v;

	if (!mb2)
		return;
	for (k = 0; k < RB; k++) {
		v = getbyte(mb, y, k);
		setbyte(mb2, 2 * y, 2 * k, dbl[v >> 4]);
		setbyte(mb2, 2 * y, 2 * k + 1, dbl[v & 15]);
		setbyte(mb2, 2 * y + 1, 2 * k, dbl[v >> 4]);
		setbyte(mb2, 2 * y + 1, 2 * k + 1, dbl[v & 15]);
	}
}

/* ---- the screen ---------------------------------------------------------- */

/* r, clipped to the map on the screen; 0 if nothing is left */
int
mclip(r)
Rectangle *r;
{
	if (r->origin.x < org.x)
		r->origin.x = org.x;
	if (r->origin.y < org.y)
		r->origin.y = org.y;
	if (r->corner.x > org.x + MW * sc)
		r->corner.x = org.x + MW * sc;
	if (r->corner.y > org.y + MH * sc)
		r->corner.y = org.y + MH * sc;
	return (r->origin.x < r->corner.x && r->origin.y < r->corner.y);
}

/* fill r with code f, or (f < 0) put the map there again */
void
mfill(r, f)
Rectangle r;
int f;
{
	if (!mclip(&r))
		return;
	if (f >= 0)
		rectf(&display, r, f);
	else
		bitblt(sc == 2 ? mb2 : mb, Rect(r.origin.x - org.x, r.origin.y - org.y,
			r.corner.x - org.x, r.corner.y - org.y), &display, r.origin, F_STORE);
}

void
edges(r, f)
Rectangle r;
int f;
{
	mfill(Rect(r.origin.x, r.origin.y, r.corner.x, r.origin.y + 1), f);
	mfill(Rect(r.origin.x, r.corner.y - 1, r.corner.x, r.corner.y), f);
	mfill(Rect(r.origin.x, r.origin.y, r.origin.x + 1, r.corner.y), f);
	mfill(Rect(r.corner.x - 1, r.origin.y, r.corner.x, r.corner.y), f);
}

/* the editor's view: an ink line in a ground line, so it shows on any
 * pattern.  off: the map again. */
void
vframe(on)
int on;
{
	Rectangle r;

	if (fx < 0)
		return;
	r = Rect(org.x + fx * sc, org.y + fy * sc, org.x + (fx + fw) * sc,
		org.y + (fy + fh) * sc);
	edges(r, on ? (sh->inv ? F_CLR : F_STORE) : -1);
	edges(inset(r, -1), on ? (sh->inv ? F_STORE : F_CLR) : -1);
}

void
title()
{
	rectf(&display, Rect(Drect.origin.x, Drect.origin.y, Drect.corner.x,
		Drect.origin.y + ch + 2), F_CLR);
	string(&defont, mname[mode], &display, add(Drect.origin, Pt(4, 1)), F_OR);
}

void
layout()
{
	int w = Drect.corner.x - Drect.origin.x, h = Drect.corner.y - Drect.origin.y - ch - 4;
	int y;

	sc = (w >= MW * 2 && h >= MH * 2) ? 2 : 1;
	if (mb2)
		bfree(mb2);
	mb2 = 0;
	if (sc == 2 && (mb2 = balloc(Rect(0, 0, MW * 2, MH * 2))) != 0)
		for (y = 0; y < MH; y++)
			double2(y);
	if (sc == 2 && !mb2)
		sc = 1;
	org.x = Drect.origin.x + (w - MW * sc) / 2;
	org.y = Drect.origin.y + ch + 4 + (h - MH * sc) / 2;
	rectf(&display, Drect, F_CLR);
	title();
	fx = -1;
	dy0 = 0;
	dy1 = MH - 1;
}

/* the rows that came and the view frame */
void
update()
{
	if (sh->inv != lastinv) {	/* the ground is lit or dark: all again */
		lastinv = sh->inv;
		rectf(mb, mb->rect, F_XOR);
		if (mb2)
			rectf(mb2, mb2->rect, F_XOR);
		dy0 = 0;
		dy1 = MH - 1;
	}
	if (dy1 < dy0 && fx == sh->vx && fy == sh->vy && fw == sh->vw && fh == sh->vh)
		return;
	vframe(0);
	if (dy1 >= dy0)
		bitblt(sc == 2 ? mb2 : mb, Rect(0, dy0 * sc, MW * sc, (dy1 + 1) * sc),
			&display, Pt(org.x, org.y + dy0 * sc), F_STORE);
	dy0 = MH;
	dy1 = -1;
	fx = sh->vx;
	fy = sh->vy;
	fw = sh->vw;
	fh = sh->vh;
	vframe(1);
}

void
setmode(m)
int m;
{
	mode = m;
	put2('O', mode);
	title();
}

/* ---- from the host ------------------------------------------------------- */

int op, ry, rk, rlit, rrep;

/* a row: PackBits; n < 128: n+1 bytes follow; n > 128: the next byte 257-n times */
void
rowput(v)
int v;
{
	if (rk < RB)
		setbyte(mb, ry, rk++, sh->inv ? ~v : v);
}

void
rowbyte(c)
int c;
{
	if (rlit > 0) {
		rowput(c);
		rlit--;
	} else if (rrep > 0) {
		while (rrep-- > 0)
			rowput(c);
		rrep = 0;
	} else if (c < 128)
		rlit = c + 1;
	else if (c > 128)
		rrep = 257 - c;
	if (rk < RB)
		return;
	double2(ry);
	if (ry < dy0)
		dy0 = ry;
	if (ry > dy1)
		dy1 = ry;
	op = 0;
}

void
fromhost(c)
int c;
{
	bytes_in++;
	if (op == 'r') {
		rowbyte(c);
		return;
	}
	if (op == 'y') {		/* the row's number */
		ry = c < MH ? c : MH - 1;
		rk = rlit = rrep = 0;
		op = 'r';
		return;
	}
	switch (c) {
	case 'r':
		op = 'y';
		return;
	case 'h':
		put1('H');
		put2('O', mode);
		return;
	case 'q':
		exit();
	}
}

void
readhost()
{
	int c, i;

	for (i = 0; i < 2000 && (c = rcvchar()) != -1; i++)
		fromhost(c);
	while (bytes_in - acked >= ACKUNIT) {
		put1('a');
		acked += ACKUNIT;
	}
}

unsigned long
hex(s)
char *s;
{
	unsigned long v = 0;

	for (; *s; s++)
		v = v * 16 + (*s >= 'a' ? *s - 'a' + 10 : *s >= 'A' ? *s - 'A' + 10 : *s - '0');
	return (v);
}

main(argc, argv)
int argc;
char **argv;
{
	int b, lastb = 0, i, x, y;
	long now, last = 0;

	request(MOUSE | RCV | SEND | ALARM);
	ch = defont.height;
	sh = argc > 1 ? (struct share *)hex(argv[1]) : 0;
	mb = balloc(Rect(0, 0, MW, MH));
	if (!alive() || !mb) {
		string(&defont, mb ? "mcmap: no mcity" : "mcmap: no memory", &display,
			add(Drect.origin, Pt(4, 4)), F_OR);
		sleep(120);
		exit();
	}
	lastinv = sh->inv;
	rectf(mb, mb->rect, sh->inv ? F_STORE : F_CLR);	/* blank ground */
	for (i = 0; mname[i]; i++)
		mptr[i] = mitems[i];
	mptr[i] = 0;
	P->state &= ~(RESHAPED | MOVED);
	layout();
	for (;;) {
		if (!alive())
			exit();
		if (P->state & (RESHAPED | MOVED)) {
			P->state &= ~(RESHAPED | MOVED);
			layout();
		}
		readhost();
		now = realtime();
		if (now - last >= 10) {
			last = now;
			update();
		}
		b = (own() & MOUSE) ? (mouse.buttons & 7) : 0;
		if (b & 4) {
			x = (mouse.xy.x - org.x) / sc;
			y = (mouse.xy.y - org.y) / sc;
			if (x >= 0 && x < MW && y >= 0 && y < MH) {
				sh->gox = x;
				sh->goy = y;
			}
		}
		if ((b & 3) && !(lastb & 3)) {
			for (i = 0; mname[i]; i++)
				sprintf(mitems[i], "%c %s", i == mode ? '*' : ' ', mname[i]);
			i = menuhit(&mmenu, (b & 1) ? 3 : 2);
			if (i >= 0 && i != mode)
				setmode(i);
			b = 0;
		}
		lastb = b;
		alarm(3);
		wait(RCV | ALARM);
	}
}

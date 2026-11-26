/* mcform.c -- Micropolis for the AT&T DMD 5620: a window whose contents
 * the host lays out (budget, evaluation, graph, notice, files, ...).
 *
 * Micropolis, Unix Version.  Copyright (C) 1989 - 2007 Electronic Arts Inc.
 * Licensed under the GNU GPL v3 or later, with the additional EA terms.
 * No trademark or publicity rights granted.  This is a modified version of
 * Micropolis; it is not the original program.
 *
 * The host (mcity on the 3B2) makes this layer and loads this program in
 * it.  It sends a display list; this program keeps the list, so that it
 * can draw it again after the layer is reshaped, and tells the host which
 * buttons and menu items the user picks.
 *
 * From the host:  h  hello (the load is done)
 *   F n(2) list    a new display list      M n(2) items   the button 3 menu
 *   k  bell        q  quit
 * List items:  t row col attr text0   b id row col text0 (a button)
 *   r row col len val (a bar, val of 255)   i id row col len text0 (input)
 *   g x(2) y(2) w(2) h(2) style n v... (a line graph, v of 255)
 *   x x(2) y(2) w(2) h(2) (a box)      attr: 1 bar  2 center  4 bold
 * To the host:  H cols rows w(2) h(2) cw ch   c id   n item   i id text0   a
 */
#include <dmd.h>
#include <font.h>

#define LSZ	6000
#define ACKUNIT	64
#define HZ	60

unsigned char list[LSZ];	/* the list on the screen */
int nlist;
unsigned char nbuf[LSZ];	/* a list or menu coming in */
int nn, nwant;
char mtext[600];		/* the menu items */
char *mitem[33];
Menu wmenu = { mitem };
int cw, ch;			/* the font's cell */
long bytes_in, acked;
int op, need, got;
unsigned char hb[4];
int focus = -1;			/* the input field that has the keyboard */
char itext[80];

void
mcopy(d, s, n)
char *d, *s;
int n;
{
	while (n-- > 0)
		*d++ = *s++;
}

/* ---- to the host --------------------------------------------------------- */

void
put1(c)
int c;
{
	char b[1];

	b[0] = c;
	sendnchars(1, b);
}

void
sendsize()
{
	char b[9];
	int w = Drect.corner.x - Drect.origin.x, h = Drect.corner.y - Drect.origin.y;

	b[0] = 'H';
	b[1] = (w - 8) / cw;
	b[2] = (h - 4) / ch;
	b[3] = w >> 8;
	b[4] = w;
	b[5] = h >> 8;
	b[6] = h;
	b[7] = cw;
	b[8] = ch;
	sendnchars(9, b);
}

/* ---- drawing ------------------------------------------------------------- */

int
g2(p)
unsigned char *p;
{
	return (p[0] << 8) | p[1];
}

Point
cpt(row, col)
int row, col;
{
	return (Pt(Drect.origin.x + 4 + col * cw, Drect.origin.y + 2 + row * ch));
}

void
frame(r)
Rectangle r;
{
	rectf(&display, Rect(r.origin.x, r.origin.y, r.corner.x, r.origin.y + 1), F_STORE);
	rectf(&display, Rect(r.origin.x, r.corner.y - 1, r.corner.x, r.corner.y), F_STORE);
	rectf(&display, Rect(r.origin.x, r.origin.y, r.origin.x + 1, r.corner.y), F_STORE);
	rectf(&display, Rect(r.corner.x - 1, r.origin.y, r.corner.x, r.corner.y), F_STORE);
}

Rectangle
btnrect(row, col, s)
int row, col;
char *s;
{
	Point p;

	p = cpt(row, col);
	return (Rect(p.x - 3, p.y - 1, p.x + strwidth(&defont, s) + 3, p.y + ch + 1));
}

/* item at p: its length; *kind, *id and the rectangle it takes */
int
item(p, kind, id, r)
unsigned char *p;
int *kind, *id;
Rectangle *r;
{
	char *s;
	int n;

	*kind = p[0];
	*id = -1;
	switch (p[0]) {
	case 't':
		s = (char *)p + 4;
		*r = Rect(0, 0, 0, 0);
		return (4 + strlen(s) + 1);
	case 'b':
		s = (char *)p + 4;
		*id = p[1];
		*r = btnrect(p[2], p[3], s);
		return (4 + strlen(s) + 1);
	case 'r':
		*r = Rect(0, 0, 0, 0);
		return (5);
	case 'i':
		s = (char *)p + 5;
		*id = p[1];
		*r = Rect(cpt(p[2], p[3]).x - 3, cpt(p[2], p[3]).y - 1,
			cpt(p[2], p[3] + p[4]).x + 3, cpt(p[2], p[3]).y + ch + 1);
		return (5 + strlen(s) + 1);
	case 'g':
		n = p[10];
		*r = Rect(0, 0, 0, 0);
		return (11 + n);
	case 'x':
		*r = Rect(0, 0, 0, 0);
		return (9);
	}
	return (0);		/* unknown: stop */
}

void
drawgraph(p)
unsigned char *p;
{
	int x = Drect.origin.x + g2(p + 1), y = Drect.origin.y + g2(p + 3);
	int w = g2(p + 5), h = g2(p + 7), style = p[9], n = p[10], i;
	Point a, b;

	/* style: 0 line, 1 dashed, 2 dots, 3 thick, 4 thick dashed,
	 * 5 big dots far apart */
	for (i = 0; i < n; i++) {
		b.x = x + (n > 1 ? i * (w - 1) / (n - 1) : 0);
		b.y = y + h - 1 - p[11 + i] * (h - 1) / 255;
		if (i > 0) {
			if ((style == 1 || style == 4) && ((i >> 1) & 1))
				;		/* dashed: 2 pieces on, 2 off */
			else if (style == 2)
				rectf(&display, Rect(b.x, b.y, b.x + 2, b.y + 2), F_STORE);
			else if (style == 5) {
				if ((i & 3) == 0)
					rectf(&display, Rect(b.x - 2, b.y - 2, b.x + 2, b.y + 2), F_STORE);
			} else {
				segment(&display, a, b, F_STORE);
				if (style >= 3)
					segment(&display, Pt(a.x, a.y + 1), Pt(b.x, b.y + 1), F_STORE);
			}
		}
		a = b;
	}
}

void
drawlist()
{
	unsigned char *p = list, *e = list + nlist;
	int n, kind, id, attr, w;
	Rectangle r;
	Point q;
	char *s;

	rectf(&display, Drect, F_CLR);
	while (p < e && (n = item(p, &kind, &id, &r)) > 0) {
		switch (kind) {
		case 't':
			attr = p[3];
			s = (char *)p + 4;
			q = cpt(p[1], p[2]);
			if (attr & 2) {
				w = strwidth(&defont, s);
				q.x = (Drect.origin.x + Drect.corner.x - w) / 2;
			}
			if (attr & 1)
				rectf(&display, Rect(Drect.origin.x, q.y - 1,
					Drect.corner.x, q.y + ch + 1), F_STORE);
			string(&defont, s, &display, q, (attr & 1) ? F_XOR : F_OR);
			if (attr & 4)
				string(&defont, s, &display, Pt(q.x + 1, q.y),
					(attr & 1) ? F_XOR : F_OR);
			break;
		case 'b':
			string(&defont, (char *)p + 4, &display, cpt(p[2], p[3]), F_OR);
			frame(r);
			break;
		case 'r':
			q = cpt(p[1], p[2]);
			w = p[3] * cw;
			frame(Rect(q.x, q.y + 2, q.x + w, q.y + ch - 2));
			rectf(&display, Rect(q.x, q.y + 2, q.x + w * p[4] / 255, q.y + ch - 2),
				F_STORE);
			break;
		case 'i':
			s = id == focus ? itext : (char *)p + 5;
			string(&defont, s, &display, cpt(p[2], p[3]), F_OR);
			frame(r);
			if (id == focus) {
				q = cpt(p[2], p[3]);
				q.x += strwidth(&defont, s);
				rectf(&display, Rect(q.x, q.y, q.x + 2, q.y + ch), F_STORE);
			}
			break;
		case 'g':
			drawgraph(p);
			break;
		case 'x':
			q = add(Drect.origin, Pt(g2(p + 1), g2(p + 3)));
			frame(Rect(q.x, q.y, q.x + g2(p + 5), q.y + g2(p + 7)));
			break;
		}
		p += n;
	}
}

/* button 1 at m: press a button or take the keyboard to a field */
void
hit(m)
Point m;
{
	unsigned char *p = list, *e = list + nlist;
	int n, kind, id;
	Rectangle r;
	char b[2];

	while (p < e && (n = item(p, &kind, &id, &r)) > 0) {
		if (id >= 0 && ptinrect(m, r)) {
			if (kind == 'b') {
				rectf(&display, r, F_XOR);
				b[0] = 'c';
				b[1] = id;
				sendnchars(2, b);
				sleep(8);
				rectf(&display, r, F_XOR);
			} else {
				focus = id;
				strncpy(itext, (char *)p + 5, sizeof(itext) - 1);
				itext[sizeof(itext) - 1] = 0;
				drawlist();
			}
			return;
		}
		p += n;
	}
}

void
typed(c)
int c;
{
	int l = strlen(itext);
	char b[90];

	if (focus < 0)
		return;
	if (c == '\b' || c == 0177) {
		if (l > 0)
			itext[l - 1] = 0;
	} else if (c == '\r' || c == '\n') {
		b[0] = 'i';
		b[1] = focus;
		strcpy(b + 2, itext);
		sendnchars(2 + l + 1, b);
		focus = -1;
	} else if (c >= ' ' && c < 0177 && l < 40) {
		itext[l] = c;
		itext[l + 1] = 0;
	}
	drawlist();
}

/* ---- from the host ------------------------------------------------------- */

void
setmenu()
{
	int i = 0, k = 0;

	if (nn > sizeof(mtext) - 1)
		nn = sizeof(mtext) - 1;
	mcopy(mtext, nbuf, nn);
	mtext[nn] = 0;
	while (k < nn && i < 32 && mtext[k]) {
		mitem[i++] = mtext + k;
		k += strlen(mtext + k) + 1;
	}
	mitem[i] = 0;
}

void
fromhost(c)
int c;
{
	bytes_in++;
	if (op == 'F' || op == 'M') {
		if (need) {		/* the length */
			hb[got++] = c;
			if (got < 2)
				return;
			nwant = (hb[0] << 8) | hb[1];
			need = 0;
			nn = 0;
			if (nwant > 0)
				return;
		} else if (nn < LSZ)
			nbuf[nn++] = c;
		if (nn < nwant)
			return;
		if (op == 'F') {
			mcopy(list, nbuf, nn);
			nlist = nn;
			drawlist();
		} else
			setmenu();
		op = 0;
		return;
	}
	switch (c) {
	case 'F':
	case 'M':
		op = c;
		need = 1;
		got = 0;
		return;
	case 'h':
		sendsize();
		return;
	case 'k':
		ringbell();
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

main()
{
	int b, lastb = 0, c, i;
	char m[2];

	request(KBD | MOUSE | RCV | SEND | ALARM);
	cw = strwidth(&defont, "m");
	ch = defont.height;
	P->state &= ~(RESHAPED | MOVED);
	rectf(&display, Drect, F_CLR);
	string(&defont, "Micropolis ...", &display, cpt(0, 0), F_OR);
	for (;;) {
		if (P->state & (RESHAPED | MOVED)) {
			P->state &= ~(RESHAPED | MOVED);
			drawlist();
			sendsize();
		}
		readhost();
		while ((c = kbdchar()) != -1)
			typed(c);
		b = (own() & MOUSE) ? (mouse.buttons & 7) : 0;
		if ((b & 4) && !(lastb & 4) && ptinrect(mouse.xy, Drect))
			hit(mouse.xy);
		if ((b & 1) && !(lastb & 1) && mitem[0]) {
			i = menuhit(&wmenu, 3);
			if (i >= 0) {
				m[0] = 'n';
				m[1] = i;
				sendnchars(2, m);
			}
			b = 0;
		}
		lastb = b;
		alarm(3);
		wait(KBD | RCV | ALARM);
	}
}

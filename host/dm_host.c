/* dm_host.c -- Micropolis for the AT&T DMD 5620: the host side.
 *
 * Micropolis, Unix Version.  Copyright (C) 1989 - 2007 Electronic Arts Inc.
 * Licensed under the GNU GPL v3 or later, with the additional EA terms
 * (see sim.h).  No trademark or publicity rights granted.  This is a
 * modified version of Micropolis; it is not the original program.
 *
 * The simulation (s_*.c, w_*.c from Micropolis for SunView) runs here, on
 * the 3B2.  The terminal program in the user's layer (mcity.m) is the main
 * window: it keeps the map and the tile art as a cache, and draws, scrolls
 * and animates by itself.  This program streams to it, most important
 * first, at about the speed of the layers link (450 B/s):
 *
 *   1. status, messages, demand, options
 *   2. cells of the view the terminal never had, then the palette icons
 *   3. the art of the tiles in the view, and changed cells in the view
 *   4. changed cells outside the view
 *   5. the rest of the art
 *
 * The other windows are layers that this program makes with the host agent
 * (libwindows): the overall map (mcmap.m, which shows the map this program
 * draws, 1 pixel a cell) and the budget, evaluation, graph, notice
 * and file windows (mcform.m, which shows what this program lays out).
 * Each layer has its own xt channel, so each has its own stream here.
 *
 * Old-style (K&R) C, like the engine.  Build: see the makefile.
 */

#include "sim.h"
#include <fcntl.h>
#include <errno.h>
#ifdef DMDHOST
#include <termio.h>
#include <signal.h>
#include <sys/times.h>
#include <sys/param.h>
#include <poll.h>
#include <dirent.h>
#include <sys/sysmacros.h>	/* sim.h has types.h and stat.h */
#endif
#include "icons.h"		/* the tool palette (tools/mkicons.py) */

/* ---- the engine's front-end globals ------------------------------------ */

Sim *sim = NULL;
int sim_skips = 0, sim_skip = 0, sim_paused = 0, sim_paused_speed = 3;
int heat_steps = 0, heat_flow = -7, heat_rule = 0;
char *CityFileName = NULL;
int Startup = 0;
char *StartupName = NULL;
int DoAnimation = 1, DoMessages = 1, DoNotices = 1;
int UpdateDelayed = 0;
short NewGraph = 0;
short Graph10Max, Graph120Max;
struct timeval start_time, now_time, beat_time, last_now_time;
int StartupGameLevel = 0;
int ExitReturn = 0;
int tkMustExit = 0;

extern short aniTile[];

static SimView *editor, *mapview;
extern char *getenv();
Sim *MakeNewSim();
SimView *MakeNewView();
static char homebuf[256], resbuf[300], namebuf[64];

/* ---- channels: one per layer --------------------------------------------- */

#define NCELL	(WORLD_X * WORLD_Y)
#define CELLMASK (LOMASK | ANIMBIT | ZONEBIT | PWRBIT)
#define RATE	1100		/* bytes a second, without a window (pipes) */
#define TICKMS	100		/* one loop */
#define LOWQ	64		/* with a window: fill the queue below this */
#define HIGHQ	192		/* up to this */
#define OBUFSZ	8192
#define ACKUNIT	64

#define W_MAIN	0
#define W_MAP	1
#define W_GRAPH	2
#define W_BUDGET 3
#define W_EVAL	4
#define W_NOTICE 5
#define W_DIALOG 6
#define NWIN	7

#define S_FREE	0
#define S_LOAD	1		/* 32ld runs on it */
#define S_HELLO	2		/* loaded, 'h' sent, waiting for 'H' */
#define S_READY	3

/* The output queue of a channel.  The terminal reads what it gets, and
 * every ACKUNIT bytes it sends 'a'.  The host keeps at most win bytes in
 * flight, so the layer's receive queue in the 5620 never fills: a full
 * queue blocks the xt channel for good (the 5620 ACKs without C_UNBLK). */
typedef struct Chan {
  int state, ifd, ofd, xchan, pid;
  unsigned char ob[OBUFSZ];
  int oh, ol;			/* ob[oh..ol) waits */
  long unacked, win;
  unsigned char ib[512];
  int il;
  int cols, rows, pw, ph, cw, chh;	/* a form window's size */
  int dirty;			/* a form: lay it out and send it again */
  int ovkind;			/* the map: what it shows (mname[] in mcmap.c) */
  long beat;			/* when we last checked that it lives */
  int want;			/* open it once the main loop runs */
  int dead;			/* 32ld failed */
  int retried;			/* we loaded it again once */
} Chan;

static Chan wins[NWIN];
static Chan *co;		/* put() writes here */
static int agent = -1;		/* the layers control channel */
static int mychan = 1;		/* the main window's channel */
static int chld[16][2], nchld;	/* 32ld exits: pid, status (for the log) */
static char *progs[NWIN] = { 0, "mcmap.m", "mcform.m", "mcform.m",
			     "mcform.m", "mcform.m", "mcform.m" };
static short wrect[NWIN][4] = {
  { 0, 0, 0, 0 }, { 660, 888, 796, 1020 }, { 300, 560, 796, 820 },
  { 330, 250, 796, 600 }, { 330, 250, 796, 640 }, { 8, 800, 520, 1016 },
  { 200, 180, 620, 720 }
};
static long shaddr;		/* the main program's struct share */
static char ldpath[220] = "/usr/dmd/bin/32ld";

static unsigned short shadow[NCELL];	/* what the terminal has; 0xffff = nothing */
static unsigned char artsent[TILE_COUNT];
static unsigned short art[TILE_COUNT][16];	/* 1 = ink, 0x8000 = left */
static int vx = 36, vy = 20, vw = 48, vh = 60;	/* the terminal's view */
static int othercur, artcur;		/* where the background scans go on */
static long credit;			/* bytes we may send now */
static long rate = RATE;		/* without a window; env MCRATE */
static int need_small = 1;		/* the 4x4 table is not sent yet */
static int iconpos;			/* the icons sent: bytes of icondata */
static char status[128], sentstatus[128];
static char msgq[8][100];		/* messages not sent yet */
static int nmsg;
static int budget_wait, quitting;
static long sentbytes, sentcells, sentart;
static int verbose;
static FILE *mclog;
static int sentopts = -1, sentspeed = -1, sentdem[3] = { 99, 99, 99 };
static int demr, demc, demi;
static long nowms;
static int looping;		/* the main program is loaded */

static void readcmds();
static void pause_ms();
static void openwin();
static void closewin();

static void
mlog(s, a)
  char *s;
  long a;
{
  if (mclog) {
    fprintf(mclog, s, a);
    fflush(mclog);
  }
}

static void
flushc(c)
  Chan *c;
{
  int n, want;

  for (;;) {
    want = c->ol - c->oh;
    if (c->win > 0 && want > c->win - c->unacked)
      want = c->win - c->unacked;
    if (want <= 0)
      break;
    n = write(c->ofd, (char *)c->ob + c->oh, want);
    if (n < 0 && errno != EINTR && c != &wins[W_MAIN]) {
      closewin(c);			/* the user deleted the layer */
      return;
    }
    if (n <= 0)
      break;
    c->oh += n;
    c->unacked += n;
    if (c == &wins[W_MAIN])
      sentbytes += n;
  }
  if (c->oh == c->ol)
    c->oh = c->ol = 0;
  else if (c->oh >= OBUFSZ / 2) {
    memcpy((char *)c->ob, (char *)c->ob + c->oh, c->ol - c->oh);
    c->ol -= c->oh;
    c->oh = 0;
  }
}

static void
flush()
{
  int i;

  for (i = 0; i < NWIN; i++)
    if (wins[i].state >= S_HELLO || (i == W_MAIN && wins[i].state == S_READY))
      flushc(&wins[i]);
}

static void
put(c)
  int c;
{
  while (co->ol >= OBUFSZ) {	/* full: wait for the terminal */
    if (co->oh > 0) {
      memcpy((char *)co->ob, (char *)co->ob + co->oh, co->ol - co->oh);
      co->ol -= co->oh;
      co->oh = 0;
      break;
    }
    if (co != &wins[W_MAIN])
      return;			/* a form that does not fit: drop the rest */
    flushc(co);
    if (co->ol >= OBUFSZ) {
      pause_ms(20);
      readcmds();
    }
  }
  co->ob[co->ol++] = c;
  if (co == &wins[W_MAIN])
    credit--;
}

static void
put2(v)
  int v;
{
  put((v >> 8) & 0xff);
  put(v & 0xff);
}

static void
puts0(s)
  char *s;
{
  while (*s)
    put(*s++ & 0x7f);
  put(0);
}

static int
queued(c)
  Chan *c;
{
  return c->ol - c->oh;
}

/* On the 3B2, gettimeofday() (WIN/3B libnet) counts whole seconds and
 * nap(100) sleeps 750 ms: use times() and poll() there. */
static long
now_ms()
{
#ifdef DMDHOST
  struct tms tb;

  return (long)times(&tb) * (1000L / HZ);
#else
  struct timeval tv;

  gettimeofday(&tv, (struct timezone *)NULL);
  return (long)tv.tv_sec * 1000L + tv.tv_usec / 1000;
#endif
}

static void
pause_ms(ms)
  int ms;
{
#ifdef DMDHOST
  struct pollfd p;

  if (ms <= 0)
    return;
  p.fd = -1;			/* no file: poll() only waits */
  p.events = 0;
  poll(&p, 1L, ms);
#else
  if (ms > 0)
    usleep((unsigned)ms * 1000);
#endif
}

/* ---- the map and the art, to the main window ----------------------------- */

/* The value of cell i for the terminal: the tile and the bits it uses. */
static int
cellval(i)
  int i;
{
  int v = Map[i / WORLD_Y][i % WORLD_Y] & 0xffff;

  if ((v & LOMASK) >= TILE_COUNT)
    v -= TILE_COUNT;
  return v & CELLMASK;
}

/* The terminal gets from old to cur by its own animation. */
static int
anisame(old, cur)
  int old, cur;
{
  int t, n;

  if (!(old & ANIMBIT) || !(cur & ANIMBIT)
      || (old & ~LOMASK) != (cur & ~LOMASK))
    return 0;
  t = old & LOMASK;
  for (n = 0; n < 32; n++) {
    t = aniTile[t] & LOMASK;
    if (t == (cur & LOMASK))
      return 1;
  }
  return 0;
}

static int
changed(i)
  int i;
{
  int v = cellval(i), s = shadow[i];

  if (v == s)
    return 0;
  if (s != 0xffff && anisame(s, v))
    return 0;
  return 1;
}

/* Send cells x, y0 .. y1-1 of one column, as a block:
 *   'b' idx(2) n, then items: k < 128 = k+1 times the next value,
 *   k >= 128 = k-127 values follow.  Values are 2 bytes, high first.
 */
static void
sendcol(x, y0, y1)
  int x, y0, y1;
{
  int base = x * WORLD_Y, i, j, k, n = y1 - y0;
  unsigned short v[WORLD_Y];

  for (i = 0; i < n; i++) {
    v[i] = cellval(base + y0 + i);
    shadow[base + y0 + i] = v[i];
  }
  put('b');
  put2(base + y0);
  put(n);
  for (i = 0; i < n; ) {
    for (j = i; j + 1 < n && v[j + 1] == v[i] && j - i < 127; j++)
      ;
    if (j > i) {			/* a run */
      put(j - i);
      put2(v[i]);
      i = j + 1;
      continue;
    }
    for (k = i; k < n && k - i < 127; k++)	/* literals up to a run */
      if (k + 1 < n && v[k + 1] == v[k])
	break;
    if (k == i)
      k = i + 1;
    put(127 + (k - i));
    for (j = i; j < k; j++)
      put2(v[j]);
    i = k;
  }
  sentcells += n;
}

/* Send the changed cells of column x between y0 and y1, as blocks. */
static int
sendchanges(x, y0, y1)
  int x, y0, y1;
{
  int y, a, any = 0, gap;

  for (y = y0; y < y1; ) {
    if (!changed(x * WORLD_Y + y)) {
      y++;
      continue;
    }
    a = y;
    gap = 0;
    for (y++; y < y1 && gap < 3; y++)	/* small gaps go in the block */
      if (changed(x * WORLD_Y + y))
	gap = 0;
      else
	gap++;
    sendcol(x, a, y - gap);
    any = 1;
  }
  return any;
}

/* Send the cells of column x between y0 and y1 that the terminal has
 * never had, as blocks. */
static void
sendunknown(x, y0, y1)
  int x, y0, y1;
{
  int y, a, gap;

  for (y = y0; y < y1; ) {
    if (shadow[x * WORLD_Y + y] != 0xffff) {
      y++;
      continue;
    }
    a = y;
    gap = 0;
    for (y++; y < y1 && gap < 3; y++)
      if (shadow[x * WORLD_Y + y] == 0xffff)
	gap = 0;
      else
	gap++;
    sendcol(x, a, y - gap);
  }
}

/* 'a' tile(2) mask(2) rows: the rows with a mask bit are new, the others
 * repeat the row above. */
static void
sendart(t)
  int t;
{
  int r, mask = 0, prev = 0;

  for (r = 0; r < 16; r++) {
    if (art[t][r] != prev)
      mask |= 1 << r;
    prev = art[t][r];
  }
  put('a');
  put2(t);
  put2(mask);
  for (r = 0; r < 16; r++)
    if (mask & (1 << r))
      put2(art[t][r]);
  artsent[t] = 1;
  sentart++;
}

/* 'p' and 960 times 2 bytes: each tile at 4x4, a nibble a row (row 0 in
 * the high nibble, the left pixel in the high bit), for the overall map
 * and as a stand-in until the art comes. */
static void
sendsmall()
{
  int t, r, c, x, y, n, v;

  put('p');
  for (t = 0; t < TILE_COUNT; t++) {
    v = 0;
    for (r = 0; r < 4; r++)
      for (c = 0; c < 4; c++) {
	n = 0;
	for (y = 0; y < 4; y++)
	  for (x = 0; x < 4; x++)
	    if (art[t][r * 4 + y] & (0x8000 >> (c * 4 + x)))
	      n++;
	if (n >= 6)
	  v |= 0x8000 >> (r * 4 + c);
      }
    put2(v);
  }
}

/* 'I' tool w h, then the PackBits rows of one palette icon. */
static void
sendicon()
{
  unsigned char *p = icondata + iconpos;
  int n = (p[3] << 8) | p[4], i;

  put('I');
  put(p[0]);
  put(p[1]);
  put(p[2]);
  for (i = 0; i < n; i++)
    put(p[5 + i]);
  iconpos += 5 + n;
}

static int turn, viewcur;

static void
viewart()
{
  int x, y, t, x1, y1;

  x1 = vx + vw > WORLD_X ? WORLD_X : vx + vw;
  y1 = vy + vh > WORLD_Y ? WORLD_Y : vy + vh;
  for (x = vx; x < x1 && credit > 0; x++)
    for (y = vy; y < y1 && credit > 0; y++) {
      if (shadow[x * WORLD_Y + y] == 0xffff)
	continue;
      t = shadow[x * WORLD_Y + y] & LOMASK;
      if (!artsent[t])
	sendart(t);
    }
}

static void
viewchanges(x1, y1)
  int x1, y1;
{
  int n, w = x1 - vx;

  for (n = 0; n < w && credit > 0; n++) {
    if (viewcur >= w)
      viewcur = 0;
    sendchanges(vx + viewcur, vy, y1);
    viewcur++;
  }
}

static int
optbits()
{
  return (autoBudget ? 1 : 0) | (autoBulldoze ? 2 : 0) | (NoDisasters ? 0 : 4) |
    (DoAnimation ? 8 : 0) | (DoMessages ? 16 : 0) | (DoNotices ? 32 : 0) |
    (autoGo ? 64 : 0);
}

static void
schedule()
{
  int x, y, t, n, x1, y1, guard, sp;

  co = &wins[W_MAIN];
  if (need_small) {
    sendsmall();
    sendart(LIGHTNINGBOLT);
    need_small = 0;
  }
  if (strcmp(status, sentstatus) != 0) {
    put('s');
    puts0(status);
    strcpy(sentstatus, status);
  }
  for (n = 0; n < nmsg; n++) {
    put('m');
    puts0(msgq[n]);
  }
  nmsg = 0;
  if (demr != sentdem[0] || demc != sentdem[1] || demi != sentdem[2]) {
    put('D');
    put(sentdem[0] = demr);
    put(sentdem[1] = demc);
    put(sentdem[2] = demi);
  }
  if (optbits() != sentopts) {
    put('O');
    put2(sentopts = optbits());
  }
  sp = sim_paused ? 0 : SimMetaSpeed;
  if (sp != sentspeed) {
    put('T');
    put(sentspeed = sp);
  }

  x1 = vx + vw > WORLD_X ? WORLD_X : vx + vw;
  y1 = vy + vh > WORLD_Y ? WORLD_Y : vy + vh;

  /* 2. cells in the view the terminal has never had, then the icons */
  for (x = vx; x < x1 && credit > 0; x++)
    sendunknown(x, vy, y1);
  while (credit > 0 && iconpos < NICONDATA)
    sendicon();
  if (credit <= 0)
    return;

  /* 3. art for the view and changed cells in the view take turns, so
   * that a busy city does not starve the art; the change scan goes on
   * where it stopped, so that the right side is not starved either */
  if (++turn & 1) {
    viewart();
    viewchanges(x1, y1);
  } else {
    viewchanges(x1, y1);
    viewart();
  }
  if (credit <= 0)
    return;

  /* 4. changed cells outside the view, a column at a time */
  for (guard = 0; guard < WORLD_X && credit > 0; guard++) {
    x = othercur;
    othercur = (othercur + 1) % WORLD_X;
    if (x >= vx && x < x1) {
      if (vy > 0)
	sendchanges(x, 0, vy);
      if (y1 < WORLD_Y)
	sendchanges(x, y1, WORLD_Y);
    } else
      sendchanges(x, 0, WORLD_Y);
  }
  if (credit <= 0)
    return;

  /* 5. the rest of the art, the tiles on the map first */
  for (guard = 0; guard < NCELL && credit > 0; guard++) {
    t = shadow[artcur] == 0xffff ? -1 : (shadow[artcur] & LOMASK);
    artcur = (artcur + 1) % NCELL;
    if (t >= 0 && !artsent[t])
      sendart(t);
  }
  for (t = 0; t < TILE_COUNT && credit > 0; t++)
    if (!artsent[t])
      sendart(t);
}

static void
msg(s)
  char *s;
{
  mlog("msg: %s\n", (long)s);
  if (nmsg < 8) {
    strncpy(msgq[nmsg], s, sizeof(msgq[0]) - 1);
    msgq[nmsg][sizeof(msgq[0]) - 1] = 0;
    nmsg++;
  }
}

static char datestr[32], fundstr[48];

static void
mkstatus()
{
  sprintf(status, "%s  %s  Pop %ld  %s%s", datestr, fundstr, (long)CityPop,
	  sim_paused || SimSpeed == 0 ? "Paused" : SimSpeed == 1 ? "Slow" :
	  SimSpeed == 2 ? "Medium" : "Fast",
	  budget_wait ? "  (budget)" : "");
}

static void
lookhere(x, y)
  int x, y;
{
  Chan *o = co;

  co = &wins[W_MAIN];
  put('g');
  put(x);
  put(y);
  co = o;
}

/* ---- form windows: the host lays them out ------------------------------- */

#define FLSZ	5800
static unsigned char fl[FLSZ];
static int fn;

static void fb(c) int c; { if (fn < FLSZ) fl[fn++] = c; }
static void fb2(v) int v; { fb((v >> 8) & 0xff); fb(v & 0xff); }
static void fs(s) char *s; { while (*s) fb(*s++ & 0x7f); fb(0); }

static void ftext(row, col, attr, s) int row, col, attr; char *s;
{ fb('t'); fb(row); fb(col); fb(attr); fs(s); }
static void fbutton(id, row, col, s) int id, row, col; char *s;
{ fb('b'); fb(id); fb(row); fb(col); fs(s); }
static void fbar(row, col, len, val) int row, col, len, val;
{ fb('r'); fb(row); fb(col); fb(len); fb(val < 0 ? 0 : val > 255 ? 255 : val); }
static void finput(id, row, col, len, s) int id, row, col, len; char *s;
{ fb('i'); fb(id); fb(row); fb(col); fb(len); fs(s); }
static void fbox(x, y, w, h) int x, y, w, h;
{ fb('x'); fb2(x); fb2(y); fb2(w); fb2(h); }

static void
fgraph(x, y, w, h, style, n, v)
  int x, y, w, h, style, n;
  unsigned char *v;
{
  int i;

  fb('g'); fb2(x); fb2(y); fb2(w); fb2(h); fb(style); fb(n);
  for (i = 0; i < n; i++)
    fb(v[i]);
}

/* send the list in fl to the window, and its button 3 menu */
static void
fsend(c, menu)
  Chan *c;
  char *menu;
{
  int i, n;

  co = c;
  put('F');
  put2(fn);
  for (i = 0; i < fn; i++)
    put(fl[i]);
  if (menu) {
    for (n = 0; menu[n] || menu[n + 1]; n++)
      ;
    put('M');
    put2(n + 1);
    for (i = 0; i <= n; i++)
      put(menu[i]);
  }
  co = &wins[W_MAIN];
  c->dirty = 0;
}

/* text wrapped to the window, from row; returns the next row */
static int
fwrap(row, text, cols)
  int row, cols;
  char *text;
{
  char lb[200], *p = text, *brk;
  int n;

  if (cols > 190)
    cols = 190;
  if (cols < 10)
    cols = 10;
  while (*p) {
    for (n = 0, brk = NULL; p[n] && p[n] != '\n' && n < cols; n++)
      if (p[n] == ' ')
	brk = p + n;
    if (p[n] && p[n] != '\n' && brk != NULL)
      n = brk - p;
    strncpy(lb, p, n);
    lb[n] = 0;
    ftext(row++, 0, 0, lb);
    p += n;
    if (*p == '\n' || *p == ' ')
      p++;
  }
  return row;
}

/* -- the notice -- */

typedef struct Notice {
  int id;
  char *title, *body;
} Notice;

#define NT_MAX	64
static Notice nt_tab[NT_MAX];
static int nt_count, nt_loaded;
static char nt_title[120], nt_text[2048];
static int nt_x = -1, nt_y = -1;
static char tw_buf[4096];

/* the next Tcl word at *pp: {braced}, "quoted" or bare; NULL at the end
 * of the command */
static char *
tcl_word(pp)
  char **pp;
{
  char *p = *pp, *r;
  int n = 0, depth, wmax = sizeof(tw_buf) - 2;

  for (;;) {
    if (*p == ' ' || *p == '\t')
      p++;
    else if (p[0] == '\\' && p[1] == '\n')
      p += 2;
    else
      break;
  }
  if (*p == 0 || *p == '\n' || *p == ';') {
    *pp = p;
    return NULL;
  }
  if (*p == '{') {
    depth = 1;
    p++;
    while (*p) {
      if (p[0] == '\\' && p[1] == '\n') {
	p += 2;
	while (*p == ' ' || *p == '\t')
	  p++;
	if (n < wmax)
	  tw_buf[n++] = ' ';
	continue;
      }
      if (*p == '{')
	depth++;
      else if (*p == '}' && --depth == 0) {
	p++;
	break;
      }
      if (n < wmax)
	tw_buf[n++] = *p;
      p++;
    }
  } else {
    while (*p && *p != ' ' && *p != '\t' && *p != '\n') {
      if (n < wmax)
	tw_buf[n++] = *p;
      p++;
    }
  }
  tw_buf[n] = 0;
  *pp = p;
  r = (char *)malloc(n + 1);
  strcpy(r, tw_buf);
  return r;
}

static void
nt_load()
{
  char path[400], *buf, *p, *w[4];
  FILE *f;
  long len;
  int n;

  nt_loaded = 1;
  sprintf(path, "%s/micropolis.tcl", ResourceDir);
  if ((f = fopen(path, "r")) == NULL)
    return;
  fseek(f, 0L, 2);
  len = ftell(f);
  fseek(f, 0L, 0);
  buf = (char *)malloc(len + 1);
  len = fread(buf, 1, len, f);
  buf[len] = 0;
  fclose(f);
  for (p = buf; *p; ) {
    if (strncmp(p, "Message ", 8) == 0 && p[8] >= '0' && p[8] <= '9' &&
	nt_count < NT_MAX) {
      p += 8;
      for (n = 0; n < 4; n++)
	if ((w[n] = tcl_word(&p)) == NULL)
	  break;
      if (n == 4) {
	nt_tab[nt_count].id = atoi(w[0]);
	nt_tab[nt_count].title = w[2];
	nt_tab[nt_count].body = w[3];
	nt_count++;
      }
    }
    while (*p && *p != '\n')
      p++;
    if (*p)
      p++;
  }
  free(buf);
}

/* body with %s replaced by parms, tabs as spaces, ends trimmed */
static void
nt_format(body, parms, nparms)
  char *body;
  char **parms;
  int nparms;
{
  char *o = nt_text, *e = nt_text + sizeof(nt_text) - 1, *s;
  int col = 0, k = 0;

  while (*body == '\n' || *body == ' ')
    body++;
  for (; *body && o < e; body++) {
    if (body[0] == '%' && body[1] == 's') {
      s = (k < nparms && parms[k] != NULL) ? parms[k] : "";
      k++;
      while (*s && o < e) {
	*o++ = *s++;
	col++;
      }
      body++;
    } else if (*body == '\t') {
      do {
	*o++ = ' ';
	col++;
      } while ((col & 7) && o < e);
    } else {
      *o++ = *body;
      col = (*body == '\n') ? 0 : col + 1;
    }
  }
  while (o > nt_text && (o[-1] == '\n' || o[-1] == ' '))
    o--;
  *o = 0;
}

static void
notice(id, parms, nparms, x, y)
  int id, nparms, x, y;
  char **parms;
{
  int i;

  if (!nt_loaded)
    nt_load();
  for (i = 0; i < nt_count; i++)
    if (nt_tab[i].id == id)
      break;
  if (i >= nt_count)
    return;
  strncpy(nt_title, nt_tab[i].title, sizeof(nt_title) - 1);
  nt_format(nt_tab[i].body, parms, nparms);
  nt_x = x;
  nt_y = y;
  mlog("notice: %s\n", (long)nt_title);
  if (wins[W_NOTICE].state == S_FREE)
    openwin(W_NOTICE);
  wins[W_NOTICE].dirty = 1;
}

static void
form_notice(c)
  Chan *c;
{
  int r;

  ftext(0, 0, 1 | 2, nt_title[0] ? nt_title : "Notice");
  r = fwrap(2, nt_text[0] ? nt_text : "There have been no notices yet.", c->cols);
  r++;
  if (r > c->rows - 1)
    r = c->rows - 1;
  fbutton(1, r, 1, "Dismiss");
  if (nt_x >= 0)
    fbutton(2, r, 12, "Go there");
}

/* -- the budget -- */

static char bd_s[4][32];		/* collected, flow, previous, current */
static char bd_got[3][32], bd_want[3][32];
static int bd_old_tax, bd_old_pct[3], bd_saved;

#define BD_PCT(f)	((int)((f) * 100.0 + 0.5))

static int
bd_pct(i)
  int i;
{
  return BD_PCT(i == 0 ? roadPercent : i == 1 ? firePercent : policePercent);
}

static void
bd_set_fund(which, pct)
  int which, pct;
{
  if (pct < 0)
    pct = 0;
  if (pct > 100)
    pct = 100;
  switch (which) {
  case 0:
    roadPercent = pct / 100.0;
    RoadSpend = (roadMaxValue * pct) / 100;
    break;
  case 1:
    firePercent = pct / 100.0;
    FireSpend = (fireMaxValue * pct) / 100;
    break;
  case 2:
    policePercent = pct / 100.0;
    PoliceSpend = (policeMaxValue * pct) / 100;
    break;
  }
  UpdateFundEffects();
}

static void
form_budget(c)
  Chan *c;
{
  static char *fname[3] = { "Roads", "Fire", "Police" };
  char s[120];
  int i, r;

  drawBudgetWindow();
  drawCurrPercents();
  UpdateBudgetWindow();
  sprintf(s, "Budget  %d%s", CurrentYear(), budget_wait ? "  (paused)" : "");
  ftext(0, 0, 1 | 2, s);
  sprintf(s, "Taxes collected  %s", bd_s[0]); ftext(2, 1, 0, s);
  sprintf(s, "Cash flow        %s", bd_s[1]); ftext(3, 1, 0, s);
  sprintf(s, "Previous funds   %s", bd_s[2]); ftext(4, 1, 0, s);
  sprintf(s, "Current funds    %s", bd_s[3]); ftext(5, 1, 0, s);
  sprintf(s, "Tax rate %2d%%", CityTax);
  ftext(7, 1, 4, s);
  fbutton(1, 7, 15, "-");
  fbutton(2, 7, 18, "+");
  fbar(7, 22, 12, CityTax * 255 / 20);
  for (i = 0; i < 3; i++) {
    r = 9 + 2 * i;
    sprintf(s, "%-7s %3d%%", fname[i], bd_pct(i));
    ftext(r, 1, 4, s);
    fbutton(3 + 2 * i, r, 15, "-");
    fbutton(4 + 2 * i, r, 18, "+");
    fbar(r, 22, 12, bd_pct(i) * 255 / 100);
    sprintf(s, "%s of %s", bd_got[i], bd_want[i]);
    ftext(r + 1, 9, 0, s);
  }
  r = 16;
  if (budget_wait) {
    fbutton(10, r, 1, "Continue");
    fbutton(11, r, 12, "Reset");
  } else
    fbutton(13, r, 1, "Close");
  fbutton(12, r, 20, autoBudget ? "Auto budget: on" : "Auto budget: off");
}

static void
budget_continue()
{
  budget_wait = 0;
  bd_saved = 0;
  Resume();
  msg("Budget set; time goes on.");
}

static void
budget_click(c, id)
  Chan *c;
  int id;
{
  int i;

  switch (id) {
  case 1: case 2:
    CityTax += id == 1 ? -1 : 1;
    if (CityTax < 0) CityTax = 0;
    if (CityTax > 20) CityTax = 20;
    break;
  case 3: case 4: case 5: case 6: case 7: case 8:
    i = (id - 3) / 2;
    bd_set_fund(i, bd_pct(i) + ((id & 1) ? -10 : 10));
    break;
  case 10:			/* Continue: like the original, it closes */
    budget_continue();
    closewin(c);
    return;
  case 11:
    CityTax = bd_old_tax;
    for (i = 0; i < 3; i++)
      bd_set_fund(i, bd_old_pct[i]);
    break;
  case 12:
    autoBudget = !autoBudget;
    MustUpdateOptions = 1;
    break;
  case 13:
    closewin(c);
    return;
  }
  drawBudgetWindow();
  c->dirty = 1;
}

/* -- the evaluation -- */

static char ev_s[18][80];
static int ev_have;

static void
form_eval(c)
  Chan *c;
{
  char s[120];
  int i;

  if (!ev_have) {
    EvalChanged = 1;
    scoreDoer();
  }
  ftext(0, 0, 1 | 2, ev_s[17][0] ? ev_s[17] : "City Evaluation");
  ftext(2, 1, 4, "Is the mayor doing a good job?");
  sprintf(s, "Yes %s   No %s", ev_s[15], ev_s[16]);
  ftext(3, 3, 0, s);
  ftext(5, 1, 4, "What are the worst problems?");
  for (i = 0; i < 4; i++)
    if (ev_s[2 + i][0] && ev_s[2 + i][0] != ' ') {
      sprintf(s, "%-20s %s", ev_s[2 + i], ev_s[6 + i]);
      ftext(6 + i, 3, 0, s);
    }
  ftext(11, 1, 4, "Statistics");
  sprintf(s, "Population:      %s", ev_s[10]); ftext(12, 3, 0, s);
  sprintf(s, "Net migration:   %s", ev_s[11]); ftext(13, 3, 0, s);
  sprintf(s, "Assessed value:  %s", ev_s[12]); ftext(14, 3, 0, s);
  sprintf(s, "Category:        %s", ev_s[13]); ftext(15, 3, 0, s);
  sprintf(s, "Game level:      %s", ev_s[14]); ftext(16, 3, 0, s);
  sprintf(s, "Overall city score (0 - 1000):  %s", ev_s[1]);
  ftext(18, 1, 4, s);
  sprintf(s, "Annual change:  %s", ev_s[0]);
  ftext(19, 1, 0, s);
}

/* -- the graph -- */

static int gr_mask = 63, gr_range = 10;
static char *gr_name[6] = { "Residential", "Commercial", "Industrial",
			    "Cash flow", "Crime", "Pollution" };

static void
gr_series(i, out)
  int i;
  unsigned char *out;
{
  short *h;
  float scale = 1.0;
  int max, x, v, off = gr_range == 10 ? 0 : 120;

  h = i == 0 ? ResHis : i == 1 ? ComHis : i == 2 ? IndHis :
    i == 3 ? MoneyHis : i == 4 ? CrimeHis : PollutionHis;
  if (i < 3) {
    max = gr_range == 10 ? Graph10Max : Graph120Max;
    if (max > 128)
      scale = 128.0 / max;
  }
  for (x = 0; x < 120; x++) {
    v = h[off + x] * scale;
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    out[119 - x] = v;
  }
}

static void
form_graph(c)
  Chan *c;
{
  unsigned char v[120];
  int i, gx, gy, gw, gh, top;

  ftext(0, 0, 1 | 2, gr_range == 10 ? "Graph: 10 years" : "Graph: 120 years");
  fbutton(20, 2, 1, "10 years");
  fbutton(21, 2, 11, "120 years");
  for (i = 0; i < 9; i++)
    v[i] = 128;
  for (i = 0; i < 6; i++) {
    fbutton(1 + i, 4 + i, 1, gr_name[i]);
    if (gr_mask & (1 << i))	/* a sample of the line */
      fgraph(4 + 13 * c->cw, 2 + (4 + i) * c->chh, 4 * c->cw, c->chh, i, 9, v);
  }
  top = 2 * c->chh;
  gx = 18 * c->cw;
  gy = top + 4;
  gw = c->pw - gx - 8;
  gh = c->ph - gy - 8;
  if (gw < 40 || gh < 30)
    return;
  fbox(gx - 1, gy - 1, gw + 2, gh + 2);
  for (i = 0; i < 6; i++)
    if (gr_mask & (1 << i)) {
      gr_series(i, v);
      fgraph(gx, gy, gw, gh, i, 120, v);
    }
}

/* -- the dialog: about, new city, load, scenarios, save as -- */

#define D_ABOUT	0
#define D_NEW	1
#define D_LOAD	2
#define D_SCEN	3
#define D_SAVE	4
static int dlg;
static char files[40][32];
static int nfiles;
static char *scen_name[8] = { "Dullsville 1900", "San Francisco 1906",
  "Hamburg 1944", "Bern 1965", "Tokyo 1957", "Detroit 1972", "Boston 2010",
  "Rio de Janeiro 2047" };

static void
listcities()
{
#ifdef DMDHOST
  DIR *d;
  struct dirent *e;
  char path[400];
  int n;

  nfiles = 0;
  sprintf(path, "%s/cities", homebuf);
  if ((d = opendir(path)) == NULL)
    return;
  while ((e = readdir(d)) != NULL && nfiles < 40) {
    n = strlen(e->d_name);
    if (n > 4 && n < 31 && strcmp(e->d_name + n - 4, ".cty") == 0)
      strcpy(files[nfiles++], e->d_name);
  }
  closedir(d);
  for (n = 1; n < nfiles; n++) {	/* sort the names */
    char t[32];
    int j;

    strcpy(t, files[n]);
    for (j = n; j > 0 && strcmp(files[j - 1], t) > 0; j--)
      strcpy(files[j], files[j - 1]);
    strcpy(files[j], t);
  }
#endif
}

static void
form_dialog(c)
  Chan *c;
{
  int i, r, col, w;
  char s[80];

  switch (dlg) {
  case D_ABOUT:
    ftext(0, 0, 1 | 2, "About Micropolis");
    fwrap(2, "Micropolis for the AT&T DMD 5620 and the 3B2.\n\n\
Copyright (C) 1989 - 2007 Electronic Arts Inc.  Free software under the \
GNU General Public License, version 3 or later, with additional terms \
from Electronic Arts.  No trademark or publicity rights are granted.\n\n\
Original game design by Will Wright.  Unix version by Don Hopkins.  \
The simulation runs on the 3B2; the DMD draws the city.", c->cols);
    fbutton(1, c->rows - 2, 1, "Close");
    break;
  case D_NEW:
    ftext(0, 0, 1 | 2, "Start a New City");
    ftext(2, 1, 0, "A new map, and the game level:");
    fbutton(1, 4, 1, "Easy");
    fbutton(2, 4, 8, "Medium");
    fbutton(3, 4, 17, "Hard");
    fbutton(9, 6, 1, "Cancel");
    break;
  case D_LOAD:
    ftext(0, 0, 1 | 2, "Load a City");
    listcities();
    for (i = 0, w = 6; i < nfiles; i++)	/* a column: the longest name */
      if (strlen(files[i]) - 2 > w)	/* less .cty, and a gap */
	w = strlen(files[i]) - 2;
    for (i = 0, r = 2, col = 1; i < nfiles; i++) {
      strcpy(s, files[i]);
      s[strlen(s) - 4] = 0;
      fbutton(10 + i, r, col, s);
      r += 2;
      if (r >= c->rows - 2) {
	r = 2;
	col += w;
      }
    }
    fbutton(9, c->rows - 2, 1, "Cancel");
    break;
  case D_SCEN:
    ftext(0, 0, 1 | 2, "Load a Scenario");
    for (i = 0; i < 8; i++)
      fbutton(1 + i, 2 + 2 * i, 1, scen_name[i]);
    fbutton(9, 19, 1, "Cancel");
    break;
  case D_SAVE:
    ftext(0, 0, 1 | 2, "Save the City");
    ftext(2, 1, 0, "Name (in the cities folder):");
    strcpy(s, CityName ? CityName : "mycity");
    finput(1, 4, 1, 24, s);
    ftext(6, 1, 0, "Type the name, then Return.");
    fbutton(9, 8, 1, "Cancel");
    break;
  }
}

static void
reset_terminal_map()
{
  int i;

  for (i = 0; i < NCELL; i++)
    if (shadow[i] != 0xffff)
      shadow[i] = 0xfffe;	/* not like any value: sends it again */
}

static void
dialog_click(c, id)
  Chan *c;
  int id;
{
  char path[400];

  switch (dlg) {
  case D_ABOUT:
    break;
  case D_NEW:
    if (id >= 1 && id <= 3) {
      StartupGameLevel = id - 1;
      GenerateNewCity();
      strcpy(namebuf, "Micropolis");
      setCityName(namebuf);
      SetGameLevelFunds(id - 1);
      if (CityFileName) {
	ckfree(CityFileName);
	CityFileName = NULL;
      }
      sim_paused = 0;
      setSpeed(3);
      reset_terminal_map();
      msg("A new city.");
    }
    break;
  case D_LOAD:
    if (id >= 10 && id < 10 + nfiles) {
      sprintf(path, "%s/cities/%s", homebuf, files[id - 10]);
      if (LoadCity(path)) {	/* LoadCity runs DoSimInit */
	if (SimSpeed == 0)	/* saved while paused */
	  setSpeed(3);
	sim_paused = 0;
	reset_terminal_map();
	ev_have = 0;
	wins[W_EVAL].dirty = wins[W_BUDGET].dirty = wins[W_GRAPH].dirty = 1;
	msg("City loaded.");
      }
    }
    break;
  case D_SCEN:
    if (id >= 1 && id <= 8) {
      LoadScenario(id);
      sim_paused = 0;
      setSpeed(3);
      reset_terminal_map();
      notice(id, (char **)NULL, 0, -1, -1);
    }
    break;
  }
  closewin(c);
}

static void
dialog_input(c, id, text)
  Chan *c;
  int id;
  char *text;
{
  char path[400], name[64];
  int i, n = 0;

  for (i = 0; text[i] && n < 40; i++)
    if (isalnum(text[i]) || text[i] == '_' || text[i] == '-')
      name[n++] = text[i];
  name[n] = 0;
  if (n == 0)
    return;
  sprintf(path, "%s/cities/%s.cty", homebuf, name);
  SaveCityAs(path);
  closewin(c);
}

static void
dialog(kind)
  int kind;
{
  dlg = kind;
  if (wins[W_DIALOG].state == S_FREE)
    openwin(W_DIALOG);
  wins[W_DIALOG].dirty = 1;
}

/* lay out and send the form windows that changed */
static void
forms()
{
  static char closemenu[] = "close\0";
  Chan *c;
  int i;

  for (i = W_GRAPH; i < NWIN; i++) {
    c = &wins[i];
    if (c->state != S_READY || !c->dirty || queued(c) > 0)
      continue;
    fn = 0;
    switch (i) {
    case W_GRAPH: form_graph(c); break;
    case W_BUDGET: form_budget(c); break;
    case W_EVAL: form_eval(c); break;
    case W_NOTICE: form_notice(c); break;
    case W_DIALOG: form_dialog(c); break;
    }
    fsend(c, closemenu);
  }
}

/* -- the map window --
 * This program draws the map, 1 pixel a cell, in the kind the window asks
 * for, and sends the rows that changed: 'r' y, then PackBits of MROWB
 * bytes (1 = ink).  A few rows each tick, so a whole map takes about 2 s
 * here, and its first copy about 3 s on the line.  Classes and grey
 * levels are 2x2 patterns, read at the pixel itself: bit 3 = even x even
 * y, 2 = odd x even y, 1 = even x odd y, 0 = odd x odd y. */
#define MROWB	(WORLD_X / 8)
#define MAPROWS	5		/* rows looked at each tick */
#define P_NONE	0
#define P_DOTS	8		/* 10/00 */
#define P_GREY	9		/* 10/01 */
#define P_FULL	15

enum { C_LAND, C_WATER, C_TREES, C_ROAD, C_WIRE, C_RES, C_COM, C_IND, C_OTHER };
static unsigned char tclass[1024];
static unsigned char mrows[WORLD_Y][MROWB];	/* what the window has */
static int mapall;			/* the window has nothing: send every row */
static int mapcur;			/* the next row to look at */
static int ovmax;			/* the overlay's highest value, this pass */

static void
mkclasses()
{
  int t, k;

  for (t = 0; t < 1024; t++) {
    if (t >= 2 && t <= 20)
      k = C_WATER;
    else if (t >= 21 && t <= 43)
      k = C_TREES;
    else if (t < 64)
      k = C_LAND;		/* dirt, rubble, flood, radiation, fire */
    else if (t <= 206 || (t >= 224 && t <= 238) || (t >= 828 && t <= 831) ||
	     (t >= 948 && t <= 951))
      k = C_ROAD;		/* roads, rails, bridges */
    else if (t >= 208 && t <= 222)
      k = C_WIRE;
    else if (t >= 240 && t <= 422)
      k = C_RES;
    else if (t >= 423 && t <= 611)
      k = C_COM;
    else if ((t >= 612 && t <= 692) || (t >= 844 && t <= 851))
      k = C_IND;
    else
      k = C_OTHER;		/* port, airport, power, police, fire, stadium */
    tclass[t] = k;
  }
}

static int
ovlevel(v, max)
  int v, max;
{
  if (v <= 0 || max <= 0)
    return 0;
  v = (v * 4) / (max + 1);
  return v < 1 ? 1 : v > 3 ? 3 : v;
}

/* the overlay at block bx, by; big ones are half the map, small 1/8 */
static int
ovbig(kind)
  int kind;
{
  return kind <= 11 && kind != 7;
}

static int
ovval(kind, bx, by)
  int kind, bx, by;
{
  switch (kind) {
  case 6: return PopDensity[bx][by];
  case 7: return RateOGMem[bx][by];
  case 8: return TrfDensity[bx][by];
  case 9: return PollutionMem[bx][by];
  case 10: return CrimeMem[bx][by];
  case 11: return LandValueMem[bx][by];
  case 12: return FireRate[bx][by];
  }
  return PoliceMapEffect[bx][by];
}

static int
ovgetmax(kind)
  int kind;
{
  int w = ovbig(kind) ? HWLDX : SmX, h = ovbig(kind) ? HWLDY : SmY, x, y, v, m = 0;

  for (x = 0; x < w; x++)
    for (y = 0; y < h; y++)
      if ((v = ovval(kind, x, y)) > m)
	m = v;
  return m;
}

/* the pattern of cell x, y in map kind k */
static int
mappat(k, x, y)
  int k, x, y;
{
  int v = Map[x][y], c = tclass[v & LOMASK];
  /* a street map: roads and water ink, trees light dots, the rest ground.
     Lines (roads) are never DOTS: on odd rows or columns they would vanish. */
  static unsigned char all[] = { P_NONE, P_FULL, P_DOTS, P_FULL, P_NONE,
				 P_NONE, P_NONE, P_NONE, P_NONE };

  switch (k) {
  case 0:			/* all */
    return all[c];
  case 1:
  case 2:
  case 3:			/* residential, commercial, industrial */
    return c == C_RES + k - 1 ? P_FULL : c == C_ROAD ? P_GREY :
      c == C_WATER ? P_DOTS : P_NONE;
  case 4:			/* power grid: powered, not powered */
    if (v & CONDBIT)
      return (PowerMap[POWERWORD(x, y)] & (1 << (x & 15))) ? P_FULL : P_GREY;
    return c == C_WATER ? P_DOTS : P_NONE;
  case 5:			/* transport */
    return c == C_ROAD ? P_FULL : c == C_WATER ? P_DOTS : P_NONE;
  }
  if (ovbig(k))
    v = ovval(k, x >> 1, y >> 1);
  else
    v = ovval(k, x >> 3, y >> 3);
  switch (ovlevel(v, ovmax)) {
  case 1: return P_DOTS;
  case 2: return P_GREY;
  case 3: return P_FULL;
  }
  return P_NONE;
}

static int
packbits(in, n, out)
  unsigned char *in, *out;
  int n;
{
  int i = 0, j, k, o = 0;

  while (i < n) {
    for (j = i; j + 1 < n && in[j + 1] == in[i] && j - i < 127; j++)
      ;
    if (j > i) {			/* a run */
      out[o++] = 257 - (j - i + 1);
      out[o++] = in[i];
      i = j + 1;
      continue;
    }
    for (k = i; k < n && k - i < 128 && !(k + 1 < n && in[k + 1] == in[k]); k++)
      ;
    if (k == i)
      k = i + 1;
    out[o++] = k - i - 1;
    while (i < k)
      out[o++] = in[i++];
  }
  return o;
}

static void
maptick(c)
  Chan *c;
{
  unsigned char row[MROWB], pk[2 * MROWB + 2];
  int i, x, y, n, bit;
  static int made;

  if (!made) {
    mkclasses();
    made = 1;
  }
  for (i = 0; i < MAPROWS && queued(c) < 64; i++) {
    y = mapcur;
    if (y == 0 && c->ovkind >= 6)
      ovmax = ovgetmax(c->ovkind);
    for (x = 0; x < MROWB; x++)
      row[x] = 0;
    bit = (y & 1) ? 2 : 8;
    for (x = 0; x < WORLD_X; x++)
      if (mappat(c->ovkind, x, y) & (bit >> (x & 1)))
	row[x >> 3] |= 0x80 >> (x & 7);
    if (mapall || memcmp((char *)row, (char *)mrows[y], MROWB) != 0) {
      n = packbits(row, MROWB, pk);
      co = c;
      put('r');
      put(y);
      for (x = 0; x < n; x++)
	put(pk[x]);
      co = &wins[W_MAIN];
      memcpy((char *)mrows[y], (char *)row, MROWB);
    }
    if (++mapcur >= WORLD_Y) {
      mapcur = 0;
      mapall = 0;
    }
  }
}

/* ---- windows: layers made with the host agent ---------------------------- */

#ifdef DMDHOST
extern int openagent(), Newlayer(), openchan(), Delete(), Top(), Current();

static struct termio tsave;

static void
rawfd(fd)
  int fd;
{
  struct termio t;

  /* Never TCSETAW here: a channel may hold output from an older layer
   * that will never drain, and the host would sleep for good. */
  ioctl(fd, TCFLSH, 2);
  ioctl(fd, TCGETA, &t);
  t.c_iflag = IGNBRK;
  t.c_oflag = 0;
  t.c_lflag = 0;
  t.c_cflag = (t.c_cflag & (CBAUD | CLOCAL)) | CS8 | CREAD;
  t.c_cc[VMIN] = 0;
  t.c_cc[VTIME] = 0;
  ioctl(fd, TCSETA, &t);
}

static void
onchld(sig)
  int sig;
{
  int st, pid, i;

  pid = wait(&st);
  if (nchld < 16) {
    chld[nchld][0] = pid;
    chld[nchld++][1] = st;
  }
  for (i = 1; i < NWIN; i++)
    if (wins[i].state == S_LOAD && wins[i].pid == pid) {
      wins[i].pid = 0;
      if (st == 0) {
	wins[i].state = S_HELLO;
	wins[i].oh = wins[i].ol = 0;
	wins[i].ob[wins[i].ol++] = 'h';
      } else {
	wins[i].state = S_FREE;
	wins[i].dead = 1;	/* the loop deletes its layer */
      }
    }
  signal(SIGCLD, onchld);
}
#endif

/* the form window to give up when the layers run out: the least wanted */
static Chan *
victim(k)
  int k;
{
  static int order[] = { W_NOTICE, W_EVAL, W_GRAPH, W_DIALOG, W_BUDGET };
  int i, w;

  for (i = 0; i < 5; i++) {
    w = order[i];
    if (w != k && wins[w].state == S_READY && !(w == W_BUDGET && budget_wait))
      return &wins[w];
  }
  return NULL;
}

static void
openwin(k)
  int k;
{
#ifdef DMDHOST
  Chan *c = &wins[k], *v;
  char path[400], arg[20];
  int pid;

  if (!looping) {		/* not now: 32ld of the main program runs */
    c->want = 1;
    return;
  }
  if (c->state != S_FREE) {
    if (c->state == S_READY && Top(agent, c->xchan) == -1)
      closewin(c);
    else
      return;
  }
  if (agent < 0 && (agent = openagent()) < 0) {
    msg("No layers agent: no windows.");
    return;
  }
  if (k == W_MAP && shaddr == 0)
    return;
  c->xchan = Newlayer(agent, wrect[k][0], wrect[k][1], wrect[k][2], wrect[k][3]);
  mlog("Newlayer %ld\n", (long)c->xchan);
  if (c->xchan < 0 && k != W_NOTICE && (v = victim(k)) != NULL) {
    if (k != W_MAP) {		/* the same program: take its layer over */
      *c = *v;
      v->state = S_FREE;
      v->ofd = v->ifd = -1;
      v->xchan = v->pid = 0;
      v->oh = v->ol = v->il = 0;
      c->dirty = 1;
      mlog("steal: Top %ld\n", (long)Top(agent, c->xchan) * 100 + c->xchan);
      msg("The window took the layer of another window.");
      return;
    }
    closewin(v);		/* the map needs its own program */
    c->xchan = Newlayer(agent, wrect[k][0], wrect[k][1], wrect[k][2], wrect[k][3]);
  }
  if (c->xchan < 0) {
    msg(k == W_NOTICE && nt_title[0] ? nt_title : "No free layer for the window: close one.");
    return;
  }
  if ((c->ofd = c->ifd = openchan(c->xchan)) < 0) {
    Delete(agent, c->xchan);
    msg("Cannot open the window's channel.");
    return;
  }
  rawfd(c->ofd);	/* before 32ld: the program may answer at once */
  sprintf(path, "%s/%s", homebuf, progs[k]);
  sprintf(arg, "%lx", shaddr);
  c->oh = c->ol = c->il = 0;
  c->unacked = 0;
  c->win = 128;
  c->dirty = 1;
  c->state = S_LOAD;
  c->beat = now_ms();		/* when the load started */
  if ((pid = fork()) == 0) {
    close(0);
    dup(c->ofd);
    close(1);
    dup(c->ofd);
    execl(ldpath, "32ld", path, arg, (char *)0);
    _exit(127);
  }
  c->pid = pid;
  mlog("window %ld: loading\n", (long)k);
#endif
}

static void
closewin(c)
  Chan *c;
{
#ifdef DMDHOST
  if (c == &wins[W_MAIN] || c->state == S_FREE && c->ofd <= 0)
    return;
  if (c->state == S_LOAD && c->pid > 0)
    kill(c->pid, SIGKILL);
  if (c->ofd > 0) {		/* flush first: close waits for output */
    ioctl(c->ofd, TCFLSH, 2);
    close(c->ofd);
  }
  if (agent >= 0 && c->xchan > 0 && Delete(agent, c->xchan) == -1) {
    Current(agent, mychan);	/* the agent cannot delete the current layer */
    if (Delete(agent, c->xchan) == -1)
      mlog("cannot delete layer %ld\n", (long)c->xchan);
  }
  c->ofd = c->ifd = -1;
  c->xchan = 0;
  c->state = S_FREE;
  c->oh = c->ol = c->il = 0;
#endif
}

static void
closeall()
{
  int i;

  for (i = 1; i < NWIN; i++)
    if (wins[i].state != S_FREE || wins[i].ofd > 0)
      closewin(&wins[i]);
}

/* ---- the terminal's input ------------------------------------------------ */

static void
tool_down(tool, x, y)
  int tool, x, y;
{
  editor->tool_state = tool;
  ToolDown(editor, (x << 4) + 8, (y << 4) + 8);
}

static void
tool_drag(x, y)
  int x, y;
{
  ToolDrag(editor, (x << 4) + 8, (y << 4) + 8);
}

static void
menu(m, item)
  int m, item;
{
  switch (m) {
  case 0:
    switch (item) {
    case 0: dialog(D_ABOUT); break;
    case 1: dialog(D_NEW); break;
    case 2: dialog(D_LOAD); break;
    case 3: dialog(D_SCEN); break;
    case 4:
      if (CityFileName)
	SaveCity();
      else
	dialog(D_SAVE);
      break;
    case 5: dialog(D_SAVE); break;
    }
    break;
  case 1:
    switch (item) {
    case 0: autoBudget = !autoBudget; break;
    case 1: autoBulldoze = !autoBulldoze; break;
    case 2: NoDisasters = !NoDisasters; break;
    case 3: DoAnimation = !DoAnimation; break;
    case 4: DoMessages = !DoMessages; break;
    case 5: DoNotices = !DoNotices; break;
    case 6: autoGo = !autoGo; break;
    }
    MustUpdateOptions = 1;
    break;
  case 2:
    switch (item) {
    case 0: MakeMonster(); break;
    case 1: MakeFire(); break;
    case 2: MakeFlood(); break;
    case 3: MakeMeltdown(); break;
    case 4: MakeTornado(); break;
    case 5: MakeEarthquake(); break;
    }
    break;
  case 3:
    if (item == 0)
      Pause();
    else {
      if (sim_paused) {
	sim_paused_speed = item;
	Resume();
      } else
	setSpeed(item);
    }
    break;
  case 4:
    switch (item) {
    case 0: openwin(W_MAP); break;
    case 1: openwin(W_GRAPH); wins[W_GRAPH].dirty = 1; break;
    case 2:
      openwin(W_BUDGET);
      wins[W_BUDGET].dirty = 1;
      break;
    case 3: openwin(W_EVAL); wins[W_EVAL].dirty = 1; break;
    case 4: openwin(W_NOTICE); wins[W_NOTICE].dirty = 1; break;
    }
    break;
  }
}

/* Commands, by window.  Main: v x y w h, d tool x y, m x y, A addr(4),
 * M menu item, q, r, a.  Map: H, O kind, a.  Form: H cols rows w(2) h(2)
 * cw ch, c id, n item, i id text0, a. */
static int
cmdlen(k, p, n)
  int k, n;
  unsigned char *p;
{
  int i;

  if (k == W_MAIN)
    switch (p[0]) {
    case 'v': case 'A': return 5;
    case 'd': return 4;
    case 'm': case 'M': return 3;
    default: return 1;
    }
  if (k == W_MAP)
    return p[0] == 'O' ? 2 : 1;
  switch (p[0]) {
  case 'H': return 9;
  case 'c': case 'n': return 2;
  case 'i':
    for (i = 2; i < n; i++)
      if (p[i] == 0)
	return i + 1;
    return n + 1;		/* not all here yet */
  default: return 1;
  }
}

static void
docmd(k, p)
  int k;
  unsigned char *p;
{
  Chan *c = &wins[k];
  int i;

  if (mclog && verbose && p[0] != 'a')
    fprintf(mclog, "cmd %d %c %d %d %d %d\n", k, p[0], p[1], p[2], p[3], p[4]);
  if (p[0] == 'a') {
    c->unacked -= ACKUNIT;
    if (c->unacked < 0)
      c->unacked = 0;
    return;
  }
  if (quitting)			/* only the acks count now */
    return;
  if (k == W_MAIN) {
    switch (p[0]) {
    case 'v':
      vx = p[1];
      vy = p[2];
      vw = p[3];
      vh = p[4];
      break;
    case 'd':
      if (p[2] < WORLD_X && p[3] < WORLD_Y)
	tool_down(p[1], p[2], p[3]);
      break;
    case 'm':
      if (p[1] < WORLD_X && p[2] < WORLD_Y)
	tool_drag(p[1], p[2]);
      break;
    case 'A':
      shaddr = ((long)p[1] << 24) | ((long)p[2] << 16) | ((long)p[3] << 8) | p[4];
      mlog("share at %lx\n", shaddr);
      openwin(W_MAP);
      break;
    case 'M':
      menu(p[1], p[2]);
      break;
    case 'q':
      quitting = 1;
      break;
    case 'r':
      for (i = 0; i < NCELL; i++)
	shadow[i] = 0xffff;
      for (i = 0; i < TILE_COUNT; i++)
	artsent[i] = 0;
      need_small = 1;
      iconpos = 0;
      sentstatus[0] = 0;
      break;
    }
    return;
  }
  if (p[0] == 'H') {
    if (c->state == S_HELLO)
      c->state = S_READY;
    if (k == W_MAP) {
      mapall = 1;		/* a new window: all of the map */
      mapcur = 0;
    } else {
      c->cols = p[1];
      c->rows = p[2];
      c->pw = (p[3] << 8) | p[4];
      c->ph = (p[5] << 8) | p[6];
      c->cw = p[7] ? p[7] : 9;
      c->chh = p[8] ? p[8] : 14;
    }
    c->dirty = 1;
    return;
  }
  if (k == W_MAP) {
    if (p[0] == 'O' && p[1] < 14) {
      c->ovkind = p[1];
      mapcur = 0;		/* the new kind, from the top */
    }
    return;
  }
  switch (p[0]) {
  case 'n':			/* the only menu item: close */
    closewin(c);
    return;
  case 'c':
    switch (k) {
    case W_BUDGET:
      budget_click(c, p[1]);
      break;
    case W_GRAPH:
      if (p[1] >= 1 && p[1] <= 6)
	gr_mask ^= 1 << (p[1] - 1);
      else if (p[1] == 20 || p[1] == 21)
	gr_range = p[1] == 20 ? 10 : 120;
      c->dirty = 1;
      break;
    case W_NOTICE:
      if (p[1] == 2 && nt_x >= 0)
	lookhere(nt_x, nt_y);
      else
	closewin(c);
      break;
    case W_DIALOG:
      if (p[1] == 9 || dlg == D_ABOUT)
	closewin(c);
      else
	dialog_click(c, p[1]);
      break;
    case W_EVAL:
      closewin(c);
      break;
    }
    return;
  case 'i':
    if (k == W_DIALOG)
      dialog_input(c, p[1], (char *)p + 2);
    return;
  }
}

static void
readcmds()
{
  int n, k, i;
  Chan *c;

  for (i = 0; i < NWIN; i++) {
    c = &wins[i];
    if (c->state < S_HELLO)
      continue;
    n = read(c->ifd, (char *)c->ib + c->il, sizeof(c->ib) - c->il);
    if (n > 0)
      c->il += n;
    while (c->il > 0 && c->il >= (k = cmdlen(i, c->ib, c->il))) {
      docmd(i, c->ib);
      if (c->state == S_FREE)
	break;
      memcpy((char *)c->ib, (char *)c->ib + k, c->il - k);
      c->il -= k;
    }
  }
}

/* ---- the engine's UI hooks --------------------------------------------- */

Kick() {}
StartMicropolisTimer() {}
StopMicropolisTimer() {}
InvalidateEditors() {}
InvalidateMaps() {}
EventuallyRedrawView(view) SimView *view; {}
setUpMapProcs() {}

static int graph_changed, eval_changed;

ChangeCensus() { CensusChanged = 1; }
doAllGraphs() {}
initGraphs() {}

graphDoer()
{
  if (CensusChanged || NewGraph) {
    CensusChanged = 0;
    NewGraph = 0;
    graph_changed = 1;
  }
}

InitGraphMax()
{
  register x;

  ResHisMax = ComHisMax = IndHisMax = 0;
  for (x = 118; x >= 0; x--) {
    if (ResHis[x] > ResHisMax) ResHisMax = ResHis[x];
    if (ComHis[x] > ComHisMax) ComHisMax = ComHis[x];
    if (IndHis[x] > IndHisMax) IndHisMax = IndHis[x];
    if (ResHis[x] < 0) ResHis[x] = 0;
    if (ComHis[x] < 0) ComHis[x] = 0;
    if (IndHis[x] < 0) IndHis[x] = 0;
  }
  Graph10Max = ResHisMax;
  if (ComHisMax > Graph10Max) Graph10Max = ComHisMax;
  if (IndHisMax > Graph10Max) Graph10Max = IndHisMax;
  Res2HisMax = Com2HisMax = Ind2HisMax = 0;
  for (x = 238; x >= 120; x--) {
    if (ResHis[x] > Res2HisMax) Res2HisMax = ResHis[x];
    if (ComHis[x] > Com2HisMax) Com2HisMax = ComHis[x];
    if (IndHis[x] > Ind2HisMax) Ind2HisMax = IndHis[x];
    if (ResHis[x] < 0) ResHis[x] = 0;
    if (ComHis[x] < 0) ComHis[x] = 0;
    if (IndHis[x] < 0) IndHis[x] = 0;
  }
  Graph120Max = Res2HisMax;
  if (Com2HisMax > Graph120Max) Graph120Max = Com2HisMax;
  if (Ind2HisMax > Graph120Max) Graph120Max = Ind2HisMax;
}

DoEarthQuake()
{
  Chan *o = co;

  co = &wins[W_MAIN];
  put('E');
  co = o;
}
StopEarthquake() {}

ViewToPixelCoords(view, x, y, outx, outy)
  SimView *view;
  int x, y, *outx, *outy;
{
  *outx = x;			/* the terminal sends map pixels */
  *outy = y;
}

Ink *
NewInk()
{
  Ink *ink = (Ink *)calloc(1, sizeof(Ink));

  ink->maxlength = 1;
  ink->points = (SimPoint *)malloc(sizeof(SimPoint));
  return ink;
}

FreeInk(ink) Ink *ink; { if (ink) { free((char *)ink->points); free((char *)ink); } }
StartInk(ink, x, y) Ink *ink; int x, y; {}
AddInk(ink, x, y) Ink *ink; int x, y; {}

sim_exit(val) int val; { tkMustExit = 1; ExitReturn = val; }

void sv_set_date(str, m, y) char *str; int m, y; { strncpy(datestr, str, 31); }
void sv_set_funds(str) char *str;
{
  if (strncmp(str, "Funds: ", 7) == 0)	/* short: the tool is on the line too */
    str += 7;
  strncpy(fundstr, str, 47);
  wins[W_BUDGET].dirty = 1;
}
void sv_set_demand(r, c, i) int r, c, i; { demr = r; demc = c; demi = i; }
void sv_set_message(str) char *str; { if (DoMessages) msg(str); }
void sv_popup_message(str) char *str; { msg(str); }
void sv_auto_goto(x, y) int x, y; { lookhere(x, y); }

void sv_show_picture(id) int id;
{
  int x = -1, y = -1;

  switch (id) {
  case 10: x = PolMaxX; y = PolMaxY; break;
  case 11: x = CrimeMaxX; y = CrimeMaxY; break;
  case 12: x = TrafMaxX; y = TrafMaxY; break;
  case 20: case 24: case 25: case 26: case 27: case 30: x = CrashX; y = CrashY; break;
  case 23: case 35: case 36: case 37: case 38: case 39: x = CCx; y = CCy; break;
  case 42: x = FloodX; y = FloodY; break;
  case 43: x = MeltX; y = MeltY; break;
  }
  if (x < 0 && (MesX || MesY)) {
    x = MesX;
    y = MesY;
  }
  if (DoNotices)
    notice(id, (char **)NULL, 0, x, y);
}

void sv_show_zone_status(str, s0, s1, s2, s3, s4, x, y)
  char *str, *s0, *s1, *s2, *s3, *s4;
  int x, y;
{
  char *parms[6], s[200];

  parms[0] = str; parms[1] = s0; parms[2] = s1;
  parms[3] = s2; parms[4] = s3; parms[5] = s4;
  sprintf(s, "%s: %s, %s", str, s0, s1);
  msg(s);
  notice(9, parms, 6, x, y);
}
void sv_did_tool(name, x, y) char *name; int x, y; {}
void sv_set_tool_state(view, state) SimView *view; int state; {}
void sv_set_map_state(view, state) SimView *view; int state; {}
void sv_set_speed(speed) int speed; {}
void sv_set_game_level(level) int level; {}
void sv_set_city_name(name) char *name; {}
void sv_set_options(options) int options; {}

void sv_set_budget(flow, previous, current, collected, tax)
  char *flow, *previous, *current, *collected;
  int tax;
{
  strncpy(bd_s[0], collected, 31);
  strncpy(bd_s[1], flow, 31);
  strncpy(bd_s[2], previous, 31);
  strncpy(bd_s[3], current, 31);
}
void sv_set_budget_values(roadGot, roadWant, roadPct, policeGot, policeWant,
			  policePct, fireGot, fireWant, firePct)
  char *roadGot, *roadWant, *policeGot, *policeWant, *fireGot, *fireWant;
  int roadPct, policePct, firePct;
{
  strncpy(bd_got[0], roadGot, 31); strncpy(bd_want[0], roadWant, 31);
  strncpy(bd_got[1], fireGot, 31); strncpy(bd_want[1], fireWant, 31);
  strncpy(bd_got[2], policeGot, 31); strncpy(bd_want[2], policeWant, 31);
}
void sv_update_budget() { wins[W_BUDGET].dirty = 1; }
void sv_show_budget_and_wait()
{
  if (!bd_saved) {
    bd_old_tax = CityTax;
    bd_old_pct[0] = bd_pct(0);
    bd_old_pct[1] = bd_pct(1);
    bd_old_pct[2] = bd_pct(2);
    bd_saved = 1;
  }
  budget_wait = 1;		/* Continue in the budget window resumes */
  msg("Pausing to set the budget ...");
  openwin(W_BUDGET);
  wins[W_BUDGET].dirty = 1;
}

void sv_set_evaluation(changed, score, ps0, ps1, ps2, ps3, pv0, pv1, pv2, pv3,
		       pop, delta, assessed, cityclass, citylevel, goodyes, goodno,
		       title)
  char *changed, *score, *ps0, *ps1, *ps2, *ps3, *pv0, *pv1, *pv2, *pv3;
  char *pop, *delta, *assessed, *cityclass, *citylevel, *goodyes, *goodno;
  char *title;
{
  char *v[18];
  int i;

  v[0] = changed; v[1] = score;
  v[2] = ps0; v[3] = ps1; v[4] = ps2; v[5] = ps3;
  v[6] = pv0; v[7] = pv1; v[8] = pv2; v[9] = pv3;
  v[10] = pop; v[11] = delta; v[12] = assessed;
  v[13] = cityclass; v[14] = citylevel;
  v[15] = goodyes; v[16] = goodno; v[17] = title;
  for (i = 0; i < 18; i++) {
    strncpy(ev_s[i], v[i] ? v[i] : "", sizeof(ev_s[i]) - 1);
    ev_s[i][sizeof(ev_s[i]) - 1] = 0;
  }
  ev_have = 1;
  wins[W_EVAL].dirty = 1;
}
void sv_init_sound() {}
void sv_shutdown_sound() {}
void sv_play_sound(channel, id, repeat) char *channel, *id; int repeat;
{
  Chan *o = co;

  if (strncmp(id, "UhUh", 4) == 0 || strncmp(id, "Sorry", 5) == 0) {
    co = &wins[W_MAIN];
    put('k');
    co = o;
  }
}
void sv_stop_sound(id) char *id; {}
void sv_stop_all_sounds() {}
void sv_win_game() { notice(100, (char **)NULL, 0, -1, -1); }
void sv_lose_game() { notice(200, (char **)NULL, 0, -1, -1); }
void sv_new_game() { InitGame(); }
void sv_drop_fire_bombs() { FireBomb(); }
void sv_did_generate_new_city() {}
void sv_did_load_city() {}
void sv_didnt_load_city(m) char *m; { msg(m); }
void sv_did_load_scenario() {}
void sv_did_save_city() { msg("City saved."); }
void sv_didnt_save_city(m) char *m; { msg(m); }
void sv_save_city_as() { dialog(D_SAVE); }

static void
new_city(level)
  int level;
{
  GenerateNewCity();
  strcpy(namebuf, "Micropolis");
  setCityName(namebuf);
  SetGameLevelFunds(level);
  sim_paused = 0;
  setSpeed(3);
}

void sv_play_new_city() { new_city(StartupGameLevel); }
void sv_really_start_game() { new_city(StartupGameLevel); }
void sv_start_load() { DoSimInit(); setSpeed(3); }
void sv_start_scenario(n) int n; { LoadScenario(n); sim_paused = 0; setSpeed(3); }

/* ---- start --------------------------------------------------------------- */

static void
engine_init()
{
  gettimeofday(&start_time, NULL);
  gettimeofday(&beat_time, NULL);
  gettimeofday(&now_time, NULL);
  last_now_time = now_time;

  UserSoundOn = 0;
  MustUpdateOptions = 1;
  HaveLastMessage = 0;
  ScenarioID = 0;
  StartingYear = 1900;
  tileSynch = 0x01;
  sim_skips = sim_skip = 0;
  autoGo = 1;
  CityTax = 7;
  CityTime = 50;
  NoDisasters = 0;
  PunishCnt = 0;
  autoBulldoze = 1;
  autoBudget = 1;
  MesNum = 0;
  LastMesTime = 0;
  flagBlink = 1;
  SimSpeed = 3;
  ChangeEval();
  MessagePort = 0;
  MesX = MesY = 0;
  sim_paused = 0;
  InitSimLoad = 2;
  tkMustExit = 0;
  ExitReturn = 0;

  sim = MakeNewSim();
  editor = MakeNewView();
  editor->title = "Editor";
  editor->type = X_Mem_View;
  editor->class = Editor_Class;
  editor->visible = 1;
  editor->tool_state = residentialState;
  editor->tool_state_save = -1;
  editor->tool_x_const = editor->tool_y_const = -1;
  editor->map_state = ALMAP;
  editor->show_me = 1;
  editor->i_width = WORLD_X << 4;
  editor->i_height = WORLD_Y << 4;
  editor->pan_x = WORLD_X << 3;
  editor->pan_y = WORLD_Y << 3;
  sim->editor = editor;
  sim->editors = 1;
  mapview = MakeNewView();
  mapview->title = "Map";
  mapview->type = X_Mem_View;
  mapview->class = Map_Class;
  mapview->map_state = ALMAP;
  mapview->tool_x_const = mapview->tool_y_const = -1;
  sim->map = mapview;
  sim->maps = 1;

  InitializeSound();
  initMapArrays();
  initGraphs();
  InitFundingLevel();
  setUpMapProcs();
  StopEarthquake();
  ResetMapState();
  ResetEditorState();
  ClearMap();
  InitWillStuff();
  SetFunds(5000);
  SetGameLevelFunds(StartupGameLevel);
  setSpeed(0);
  setSkips(0);
}

Sim *MakeNewSim() { return (Sim *)calloc(1, sizeof(Sim)); }
SimView *MakeNewView() { return (SimView *)calloc(1, sizeof(SimView)); }

static void
load_art(path)
  char *path;
{
  FILE *f = fopen(path, "r");
  int t, r, a, b;

  if (f == NULL) {
    fprintf(stderr, "mcity: no tile art %s\n", path);
    exit(1);
  }
  for (t = 0; t < TILE_COUNT; t++)
    for (r = 0; r < 16; r++) {
      a = getc(f);
      b = getc(f);
      art[t][r] = (a << 8) | b;
    }
  fclose(f);
}

#ifdef DMDHOST
static int
psystem(path, a0, a1)
  char *path, *a0, *a1;
{
  int pid, st, l;

  if ((pid = fork()) == 0) {
    execl(path, a0, a1, (char *)0);
    _exit(127);
  }
  while ((l = wait(&st)) != pid && l != -1)
    ;
  return l == -1 ? -1 : st;
}

static void
rawtty()
{
  struct termio t;

  ioctl(0, TCGETA, &tsave);
  t = tsave;
  t.c_iflag = IGNBRK;
  t.c_oflag = 0;
  t.c_lflag = 0;
  t.c_cflag = (tsave.c_cflag & (CBAUD | CLOCAL)) | CS8 | CREAD;
  t.c_cc[VMIN] = 0;
  t.c_cc[VTIME] = 0;
  ioctl(0, TCSETAW, &t);
}

/* Back to the shell: the last 'a' from the terminal may still come, so
 * read for a moment, then flush what is left (TCSETAF). */
static void
cooktty()
{
  char junk[64];
  int i;

  for (i = 0; i < 15; i++) {
    pause_ms(20);
    while (read(0, junk, sizeof(junk)) > 0)
      ;
  }
  ioctl(0, TCSETAF, &tsave);
}

static void
onsig(sig)
  int sig;
{
  mlog("signal %ld\n", (long)sig);
  signal(SIGCLD, SIG_DFL);
  closeall();
  ioctl(0, TCSETA, &tsave);
  _exit(1);
}
#endif

static void
usage()
{
  fprintf(stderr, "usage: mcity [-l 0|1|2] [-s 1-8] [-seed n] [-v] [city.cty]\n");
  exit(2);
}

main(argc, argv)
  int argc;
  char **argv;
{
  char *loadfile = NULL, *home, path[400];
  int scenario = 0, seed = -1, i;
  long t0, t1, last, tick = 0, started;
  Chan *m = &wins[W_MAIN];

  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-l") == 0 && i + 1 < argc)
      StartupGameLevel = atoi(argv[++i]);
    else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
      scenario = atoi(argv[++i]);
    else if (strcmp(argv[i], "-seed") == 0 && i + 1 < argc)
      seed = atoi(argv[++i]);
    else if (strcmp(argv[i], "-v") == 0)
      verbose = 1;
    else if (argv[i][0] == '-' || loadfile)
      usage();
    else
      loadfile = argv[i];
  }
  home = getenv("MCHOME");
  strcpy(homebuf, home && *home ? home : "/usr/dmd/lib/mcity");
  sprintf(resbuf, "%s/res", homebuf);
  HomeDir = homebuf;
  ResourceDir = resbuf;
  HostName = "localhost";
  if (getenv("MCRATE"))
    rate = atol(getenv("MCRATE"));
  if (getenv("MCLOG"))
    mclog = fopen(getenv("MCLOG"), "w");
  sprintf(path, "%s/tilesbw.dat", homebuf);
  load_art(path);

  m->ifd = 0;
  m->ofd = 1;
  m->state = S_READY;
#ifdef DMDHOST
  m->win = 128;
#endif
  if (getenv("MCWIN"))
    m->win = atol(getenv("MCWIN"));
  co = m;

  engine_init();
  if (loadfile) {
    char *copy = (char *)malloc(strlen(loadfile) + 1);

    strcpy(copy, loadfile);
    if (!LoadCity(copy)) {
      fprintf(stderr, "mcity: cannot load %s\n", loadfile);
      exit(1);
    }
    DoSimInit();
  } else if (scenario > 0) {
    LoadScenario(scenario);
    notice(scenario, (char **)NULL, 0, -1, -1);
  } else if (seed >= 0) {
    GenerateSomeCity(seed);
    strcpy(namebuf, "Micropolis");
    setCityName(namebuf);
    SetGameLevelFunds(StartupGameLevel);
  } else
    new_city(StartupGameLevel);
  sim_paused = 0;
  setSpeed(3);

#ifdef DMDHOST
  /* Raw before 32ld: the program in the terminal sends at once, and its
   * bytes must not meet erase, kill or the signal keys.  32ld keeps the
   * modes it finds and puts them back at its end. */
  rawtty();
  signal(SIGHUP, onsig);
  signal(SIGINT, onsig);
  signal(SIGQUIT, onsig);
  signal(SIGTERM, onsig);
  if (!getenv("MCNOLOAD")) {
    char dmd[200], term[400];

    strcpy(dmd, getenv("DMD") ? getenv("DMD") : "/usr/dmd");
    sprintf(ldpath, "%s/bin/32ld", dmd);
    sprintf(term, "%s/mcity.m", homebuf);
    if (psystem(ldpath, "32ld", term) != 0) {
      ioctl(0, TCSETAF, &tsave);
      fprintf(stderr, "mcity: 32ld %s failed\n", term);
      exit(1);
    }
  }
  mlog("32ld done\n", 0L);
  {
    struct stat st;		/* the xt minor: link * 8 + channel */

    if (fstat(0, &st) == 0)
      mychan = minor(st.st_rdev) & 7;
  }
  signal(SIGCLD, onchld);	/* the windows' 32ld */
#else
  fcntl(0, F_SETFL, fcntl(0, F_GETFL, 0) | O_NONBLOCK);
#endif
  for (i = 0; i < NCELL; i++)
    shadow[i] = 0xffff;
  put('h');			/* hello: the host runs */
  put(1);

  started = last = now_ms();
  looping = 1;
  while (!quitting && !tkMustExit) {
    t0 = nowms = now_ms();
    if (m->win > 0)		/* the window paces: keep the queue short */
      credit = queued(m) < LOWQ ? HIGHQ - queued(m) : 0;
    else {			/* no window (pipe tests): a fixed rate */
      credit += (t0 - last) * rate / 1000;
      if (credit > rate / 2)
	credit = rate / 2;
    }
    last = t0;
    readcmds();
    gettimeofday(&now_time, NULL);
    flagBlink = ((t0 / 500) & 1) ? 1 : -1;
    if (SimSpeed) {
      SimFrame();
      MoveObjects();
      animateTiles();
    }
    DoUpdateHeads();
    updateOptions();
    graphDoer();
    if (graph_changed && wins[W_GRAPH].state == S_READY) {
      graph_changed = 0;
      wins[W_GRAPH].dirty = 1;
    }
    mkstatus();
    if (credit > 0)
      schedule();
    forms();
    if (budget_wait && wins[W_BUDGET].state == S_FREE)
      budget_continue();	/* no window, or the user closed it */
    if (wins[W_MAP].state == S_READY)
      maptick(&wins[W_MAP]);
    for (i = 1; i < NWIN; i++)
      if (wins[i].want) {
	wins[i].want = 0;
	openwin(i);
      }
    while (nchld > 0) {
      nchld--;
      mlog("32ld exit %ld\n", (long)chld[nchld][0] * 1000 + chld[nchld][1] / 256);
    }
    for (i = 1; i < NWIN; i++)
      if (wins[i].dead) {
	wins[i].dead = 0;
	closewin(&wins[i]);
	if (!wins[i].retried) {
	  wins[i].retried = 1;	/* once more */
	  wins[i].want = 1;
	} else
	  msg("A window did not load.");
      }
    for (i = 1; i < NWIN; i++)	/* a load that hangs */
      if (wins[i].state == S_LOAD && t0 - wins[i].beat > 150000L) {
	msg("A window did not load.");
	closewin(&wins[i]);
      }
    for (i = 1; i < NWIN; i++)	/* is it still there? a byte tells */
      if (wins[i].state == S_READY && t0 - wins[i].beat > 3000) {
	wins[i].beat = t0;
	co = &wins[i];
	if (queued(co) == 0)
	  put('z');
      }
    co = m;
    flush();
    tick++;
    t1 = now_ms();
    if (mclog && (tick % 50) == 0) {
      int unknown = 0, behind = 0;

      for (i = 0; i < NCELL; i++)
	if (shadow[i] == 0xffff)
	  unknown++;
	else if (changed(i))
	  behind++;
      fprintf(mclog, "t=%lds sent %ld B, %ld cells, %ld art, unknown %d, behind %d, queued %d, in flight %ld, loop %ld ms, %s\n",
	      (t1 - started) / 1000, sentbytes, sentcells, sentart, unknown, behind, queued(m), m->unacked, t1 - t0, status);
      fflush(mclog);
    }
    while ((t1 = now_ms()) < t0 + TICKMS) {	/* take 'a' and send on */
      pause_ms((int)(t0 + TICKMS - t1) < 20 ? (int)(t0 + TICKMS - t1) : 20);
      readcmds();
      flush();
      if (m->win > 0 && queued(m) < LOWQ) {	/* the queue runs dry */
	credit = HIGHQ - queued(m);
	mkstatus();
	schedule();
	flush();
      }
    }
  }
  /* Send what waits first: it ends with a whole command, so the 'q'
   * after it is read as a command.  The queue is short (HIGHQ). */
  co = m;
  for (i = 0; i < 250 && m->ol > m->oh; i++) {
    flushc(m);
    pause_ms(20);
    readcmds();
  }
  m->oh = m->ol = 0;
  put('q');
  for (i = 0; i < 250 && m->ol > m->oh; i++) {
    flushc(m);
    pause_ms(20);
    readcmds();
  }
#ifdef DMDHOST
  signal(SIGCLD, SIG_DFL);
  closeall();
  cooktty();
#endif
  exit(0);
}

/* share.h -- what mcity.m shares with mcmap.m.  The 5620 has no MMU: the
 * map window reads the editor's view and the colors (inv) here, and asks
 * the editor to look somewhere (gox, goy).  mcmap.m gets the address of this struct from
 * the host (32ld mcmap.m ADDR) and checks magic before each use; mcity.m
 * clears magic before it ends.  The DMD does not preempt a process, so a
 * reader never sees half of an update. */
#define SHMAGIC	0x4d435459L	/* MCTY */

struct share {
	long	magic;
	unsigned short *cell;	/* 120 x 100, x * 100 + y */
	unsigned short *smallt;	/* each tile at 4 x 4, a nibble a row */
	int	gotsmall;	/* smallt is complete */
	int	vx, vy, vw, vh;	/* the editor's view, in tiles */
	int	gox, goy;	/* the map asks the editor to look here; -1 none */
	int	inv;		/* dark ink on lit ground */
	long	gen;		/* changes when all of the map comes again */
};

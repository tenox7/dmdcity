/* palette.h -- made by tools/mkicons.py: the tool palette.
 * icon: tool, x y of the 16x16 icon in the palette (its button
 * is 2 pixels more each side, its shadow 1 more right and below). */
#define PALW	58
#define ICONSH	201
#define ICONN	16

struct icon icon[NTOOL] = {
	{  0,  32, 176 },
	{  1,   8,   8 },
	{  2,  32,   8 },
	{  3,  32,  32 },
	{  4,   8,  32 },
	{  5,   8,  56 },
	{  6,  32,  56 },
	{  7,   8,  80 },
	{  8,  32,  80 },
	{  9,   8, 104 },
	{ 10,  32, 104 },
	{ 11,   8, 128 },
	{ 12,   8, 152 },
	{ 13,  32, 128 },
	{ 14,   8, 176 },
	{ 15,  32, 152 },
};

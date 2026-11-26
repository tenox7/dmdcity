# Micropolis for the DMD 5620, the terminal side:
#   mcity.m   the main window (the editor), in the user's layer
#   mcmap.m   the overall map, in a layer the host makes
#   mcform.m  budget, evaluation, graph, notice and file windows
DMD=/usr/dmd
MCC=$(DMD)/bin/dmdcc

all:	mcity.m mcmap.m mcform.m

mcity.m:	mcity.c anitab.h share.h palette.h
	DMD=$(DMD) $(MCC) -o mcity.m mcity.c

mcmap.m:	mcmap.c share.h
	DMD=$(DMD) $(MCC) -o mcmap.m mcmap.c

mcform.m:	mcform.c
	DMD=$(DMD) $(MCC) -o mcform.m mcform.c

clean:
	rm -f mcity.m mcmap.m mcform.m

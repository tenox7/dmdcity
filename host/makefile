# Micropolis for the DMD 5620, the host side (the 3B2).
#   make             mcity, with gcc 2.7.2.3 (/usr/gnu/bin/gcc)
#   make install     as root: mcity to $(DMD)/bin, the data to $(LIB)
DMD=/usr/dmd
LIB=$(DMD)/lib/mcity
CC=gcc
CFLAGS=-O -DDMDHOST -Iinc -Iheaders -Du_int=unsigned
LIBS=-lwindows -lnet -lnsl_s -lm
OBJS=dm_host.o g_ani.o rand.o random.o s_alloc.o s_disast.o s_eval.o \
	s_fileio.o s_gen.o s_init.o s_msg.o s_power.o s_scan.o s_sim.o \
	s_traf.o s_zone.o w_budget.o w_con.o w_eval.o w_keys.o w_resrc.o \
	w_sound.o w_sprite.o w_stubs.o w_tool.o w_update.o w_util.o

mcity:	$(OBJS)
	$(CC) -o mcity $(OBJS) $(LIBS)

$(OBJS):	headers/sim.h
dm_host.o:	icons.h

clean:
	rm -f mcity *.o

# as root, after "make" here and in ../term
install:	mcity
	cp mcity $(DMD)/bin/mcity
	-mkdir $(LIB) $(LIB)/res $(LIB)/cities
	cp ../term/mcity.m ../term/mcmap.m ../term/mcform.m ../data/tilesbw.dat $(LIB)
	cp ../data/res/* $(LIB)/res
	cp ../data/cities/*.cty $(LIB)/cities
	chmod 777 $(LIB)/cities

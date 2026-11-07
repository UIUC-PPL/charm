SHELL := /bin/bash

# to be set appropiately
CHARMBASE      = $(HOME)/charms/charm/net-linux-x86_64
CHARMC         = $(CHARMBASE)/bin/charmc

OPTS            = -O3

DECL=
SUFFIX=

ifeq ($(FT), yes)
DECL=-DCMK_MEM_CHECKPOINT=1
FT=-syncft
endif

all: leanmd

leanmd: Main.o Cell.o Compute.o leanmd.decl.h
	$(CHARMC) $(OPTS) -module CkMulticast -module CommonLBs \
	-language charm++ -o leanmd$(SUFFIX) Main.o Cell.o Compute.o

projections: Main.o Cell.o Compute.o leanmd.decl.h
	$(CHARMC) $(OPTS) -module CkMulticast -module CommonLBs \
	 -tracemode projections -language charm++ -o leanmd$(SUFFIX).prj \
	Main.o Cell.o Compute.o

Main.o: Main.cc Main.h leanmd.decl.h defs.h
	$(CHARMC) $(OPTS) -o $@ -c $<

Cell.o: Cell.cc Cell.h leanmd.decl.h defs.h
	$(CHARMC) $(OPTS) -o $@ -c $<

leanmd.decl.h:	leanmd.ci
	$(CHARMC) -E leanmd.ci $(DECL)

Compute.o: Compute.cc Compute.h leanmd.decl.h defs.h physics.h
	$(CHARMC) $(OPTS) -o $@ -c $<

test: leanmd
	./charmrun +p4 ./leanmd 4 4 4 10 3 3 +balancer GreedyLB +LBDebug 1 ++local

clean:
	rm -f *.decl.h *.def.h *.o leanmd leanmd-ft leanmd.prj charmrun

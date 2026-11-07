SHELL := /bin/bash

-include ../../common.mk
# Defaults to the enclosing Charm++ build; to use another, pass it on the
# command line: make CHARMBASE=/path/to/charm/build
CHARMBASE      = ../../..
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
	$(call run, +p4 ./leanmd 5 5 3 20 10 5 +balancer GreedyRefineLB)
	$(call run, +p4 ./leanmd -place away -migratable-cells 5 5 3 20 5 5 +balancer RotateLB)
	$(call run, +p4 ./leanmd -hop between -hopevery 2 -place random 5 5 3 20 1000 1000)
	$(call run, +p4 ./leanmd -hop computes -hopevery 2 5 5 3 20 1000 1000)

# Known failures (each hangs; the program aborts after 60 s): cells (the
# section roots) migrating while their contributions are in flight, and
# migrating twice in a row (charm #4132).
test-known-failures: leanmd
	$(call run, +p4 ./leanmd -hop inflight -hopevery 3 5 5 3 30 1000 1000)
	$(call run, +p4 ./leanmd -hop twice -hopevery 3 5 5 3 30 1000 1000)

clean:
	rm -f *.decl.h *.def.h *.o leanmd leanmd-ft leanmd.prj charmrun

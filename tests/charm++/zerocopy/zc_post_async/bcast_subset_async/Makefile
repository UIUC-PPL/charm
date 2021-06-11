-include ../../../../common.mk
-include ../../../../../include/conv-mach-opt.mak
CHARMDIR = ../../../../../
CHARMC = $(CHARMDIR)/bin/charmc $(OPTS)

all: bcast_subset_async_delayed bcast_subset_async_early

cifiles: bcast_subset_async.ci
	$(CHARMC)  bcast_subset_async.ci
	touch cifiles

bcast_subset_async_delayed.o: bcast_subset_async.C cifiles
	$(CHARMC) -DDELAYED_POST=1 -c bcast_subset_async.C -o bcast_subset_async_delayed.o

bcast_subset_async_delayed: bcast_subset_async_delayed.o
	$(CHARMC) -language charm++ -o bcast_subset_async_delayed bcast_subset_async_delayed.o

bcast_subset_async_early: bcast_subset_async_early.o
	$(CHARMC) -language charm++ -o bcast_subset_async_early bcast_subset_async_early.o

bcast_subset_async_early.o: bcast_subset_async.C cifiles
	$(CHARMC) -DEARLY_POST=1 -c bcast_subset_async.C -o bcast_subset_async_early.o

test: all
	$(call run, +p1 ./bcast_subset_async_delayed)
	$(call run, +p4 ./bcast_subset_async_delayed)
	$(call run, +p6 ./bcast_subset_async_delayed)
	$(call run, +p1 ./bcast_subset_async_early)
	$(call run, +p4 ./bcast_subset_async_early)
	$(call run, +p6 ./bcast_subset_async_early)
ifeq ($(CMK_USE_CMA),1)
	$(call run, +p4 ./bcast_subset_async_delayed +noCMAForZC)
	$(call run, +p6 ./bcast_subset_async_delayed +noCMAForZC)
	$(call run, +p4 ./bcast_subset_async_early +noCMAForZC)
	$(call run, +p6 ./bcast_subset_async_early +noCMAForZC)
endif

testp: all
	$(call run, +p$(P) ./bcast_subset_async_delayed )
	$(call run, +p$(P) ./bcast_subset_async_early )
ifeq ($(CMK_USE_CMA),1)
	$(call run, +p$(P) ./bcast_subset_async_delayed +noCMAForZC)
	$(call run, +p$(P) ./bcast_subset_async_early +noCMAForZC)
endif

smptest: all
	$(call run, +p2 ./bcast_subset_async_delayed ++ppn 2)
	$(call run, +p4 ./bcast_subset_async_delayed ++ppn 2)
	$(call run, +p2 ./bcast_subset_async_early ++ppn 2)
	$(call run, +p4 ./bcast_subset_async_early ++ppn 2)
ifeq ($(CMK_USE_CMA),1)
	$(call run, +p2 ./bcast_subset_async_delayed +noCMAForZC ++ppn 2)
	$(call run, +p4 ./bcast_subset_async_delayed +noCMAForZC ++ppn 2)
	$(call run, +p2 ./bcast_subset_async_early +noCMAForZC ++ppn 2)
	$(call run, +p4 ./bcast_subset_async_early +noCMAForZC ++ppn 2)
endif

clean:
	rm -f *.decl.h *.def.h *.o
	rm -f bcast_subset_async_early bcast_subset_async_delayed charmrun cifiles

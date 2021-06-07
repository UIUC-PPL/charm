-include ../../../../common.mk
-include ../../../../../include/conv-mach-opt.mak
CHARMDIR = ../../../../../
CHARMC = $(CHARMDIR)/bin/charmc $(OPTS)

all: post_and_match_delayed post_and_match_early post_and_match_sync

cifiles: post_and_match.ci
	$(CHARMC)  post_and_match.ci
	touch cifiles

post_and_match_delayed.o: post_and_match.C cifiles
	$(CHARMC) -DDELAYED_POST=1 -c post_and_match.C -o post_and_match_delayed.o

post_and_match_delayed: post_and_match_delayed.o
	$(CHARMC) -language charm++ -o post_and_match_delayed post_and_match_delayed.o

post_and_match_early: post_and_match_early.o
	$(CHARMC) -language charm++ -o post_and_match_early post_and_match_early.o

post_and_match_early.o: post_and_match.C cifiles
	$(CHARMC) -DEARLY_POST=1 -c post_and_match.C -o post_and_match_early.o

post_and_match_sync: post_and_match_sync.o
	$(CHARMC) -language charm++ -o post_and_match_sync post_and_match_sync.o

post_and_match_sync.o: post_and_match.C cifiles
	$(CHARMC) -DSYNC_POST=1 -c post_and_match.C -o post_and_match_sync.o

test: all
	$(call run, +p1 ./post_and_match_delayed)
	$(call run, +p4 ./post_and_match_delayed)
	$(call run, +p6 ./post_and_match_delayed)
	$(call run, +p1 ./post_and_match_early)
	$(call run, +p4 ./post_and_match_early)
	$(call run, +p6 ./post_and_match_early)
	$(call run, +p1 ./post_and_match_sync)
	$(call run, +p4 ./post_and_match_sync)
	$(call run, +p6 ./post_and_match_sync)
ifeq ($(CMK_USE_CMA),1)
	$(call run, +p4 ./post_and_match_delayed +noCMAForZC)
	$(call run, +p6 ./post_and_match_delayed +noCMAForZC)
	$(call run, +p4 ./post_and_match_early +noCMAForZC)
	$(call run, +p6 ./post_and_match_early +noCMAForZC)
	$(call run, +p4 ./post_and_match_sync +noCMAForZC)
	$(call run, +p6 ./post_and_match_sync +noCMAForZC)
endif

testp: all
	$(call run, +p$(P) ./post_and_match_delayed )
	$(call run, +p$(P) ./post_and_match_early )
ifeq ($(CMK_USE_CMA),1)
	$(call run, +p$(P) ./post_and_match_delayed +noCMAForZC)
	$(call run, +p$(P) ./post_and_match_early +noCMAForZC)
endif

smptest: all
	$(call run, +p2 ./post_and_match_delayed ++ppn 2)
	$(call run, +p4 ./post_and_match_delayed ++ppn 2)
	$(call run, +p2 ./post_and_match_early ++ppn 2)
	$(call run, +p4 ./post_and_match_early ++ppn 2)
ifeq ($(CMK_USE_CMA),1)
	$(call run, +p2 ./post_and_match_delayed +noCMAForZC ++ppn 2)
	$(call run, +p4 ./post_and_match_delayed +noCMAForZC ++ppn 2)
	$(call run, +p2 ./post_and_match_early +noCMAForZC ++ppn 2)
	$(call run, +p4 ./post_and_match_early +noCMAForZC ++ppn 2)
endif

clean:
	rm -f *.decl.h *.def.h *.o
	rm -f post_and_match_sync post_and_match_early post_and_match_delayed charmrun cifiles

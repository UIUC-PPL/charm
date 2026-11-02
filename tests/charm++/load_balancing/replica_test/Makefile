-include ../../../common.mk
# Replication as a balancing decision: ShedLB asks a heavy worker for a copy
# (ckClone) and takes it back once the worker is light (ckRetire); the sums
# must stay bit-identical to a sequential evaluation.

OPTS	= -O3
CHARMC	= ../../../../bin/charmc $(OPTS)

OBJS	= replica_test.o

all:	replica_test

replica_test: $(OBJS)
	$(CHARMC) -language charm++ -o replica_test $(OBJS) -module ShedLB

replica_test.decl.h replica_test.def.h: replica_test.ci
	$(CHARMC) replica_test.ci

replica_test.o: replica_test.C replica_test.decl.h
	$(CHARMC) -c replica_test.C

test: replica_test
	$(call run, +p4 ./replica_test +balancer ShedLB +LBDebug 1 )
	$(call run, +p4 ./replica_test -norep +balancer ShedLB +LBDebug 1 )

testp: replica_test
	$(call run, +p$(P) ./replica_test +balancer ShedLB +LBDebug 1 )

smptest: replica_test
	$(call run, +p4 ./replica_test +balancer ShedLB +LBDebug 1 ++ppn 2 )

clean:
	rm -f *.decl.h *.def.h conv-host *.o replica_test charmrun

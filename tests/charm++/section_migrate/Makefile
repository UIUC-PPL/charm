-include ../../common.mk
-include ../../../include/conv-mach-opt.mak
CHARMC=../../../bin/charmc $(OPTS)

all: section_migrate

section_migrate.decl.h section_migrate.def.h: section_migrate.ci
	$(CHARMC) section_migrate.ci

section_migrate: section_migrate.C section_migrate.decl.h section_migrate.def.h
	$(CHARMC) -language charm++ section_migrate.C -o section_migrate -module CkMulticast -module CommonLBs

# The program aborts on its own after 20 s without progress.
test: section_migrate
	$(call run, ./section_migrate +p4)
	$(call run, ./section_migrate +p4 -auto)
	$(call run, ./section_migrate +p3 -inflight)
	$(call run, ./section_migrate +p4 -inflight +randomized_msgq +randomized_seed 1)
	$(call run, ./section_migrate +p4 -inflight -auto +randomized_msgq +randomized_seed 3)
	$(call run, ./section_migrate +p4 -stillroot)
	$(call run, ./section_migrate +p4 -stillroot -inflight +randomized_msgq +randomized_seed 4)
	$(call run, ./section_migrate +p4 -lb +balancer RotateLB)
	$(call run, ./section_migrate +p4 -lb -auto +balancer RotateLB +randomized_msgq +randomized_seed 2)

# Known failures (each hangs, and the program aborts after 20 s):
# the root moves while section reductions are in flight (migrateMe, then LB);
# #4132 item 1, the root moves again before its new tree is set up;
# #4132 item 3, the root moves before its tree is ready.
test-known-failures: section_migrate
	$(call run, ./section_migrate +p3 -inflight -root)
	$(call run, ./section_migrate +p3 -lb -inflight +balancer RotateLB)
	$(call run, ./section_migrate +p4 -double +randomized_msgq +randomized_seed 2)
	$(call run, ./section_migrate +p3 -lb -inflight -every +balancer RotateLB +randomized_msgq +randomized_seed 2)

smptest: section_migrate

clean:
	rm -f *.decl.h *.def.h *.o section_migrate charmrun

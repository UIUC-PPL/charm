-include ../../common.mk
CHARMC=../../../bin/charmc $(OPTS)

all: migrate6

migrate6: migrate6.decl.h migrate6.C
	$(CHARMC) migrate6.C -o migrate6 -module CommonLBs

migrate6.decl.h: migrate6.ci
	$(CHARMC) migrate6.ci

# A 6-component index that migrates: hashed kind (no bounds) and packed kind (-b),
# elements inserted from every PE; migrateMe rounds, AtSync under a balancer, and
# a stream of messages to an element moving between the PEs of one process.
# Set TESTRUN_PROCS=2 (or 4) to spread the PEs over processes.
test: migrate6
	$(call run, ./migrate6 +p4 +objid_diag)
	$(call run, ./migrate6 +p4 -b +objid_diag)
	$(call run, ./migrate6 +p4 -lb +balancer RotateLB +objid_diag)
	$(call run, ./migrate6 +p4 -lb -b +balancer GreedyLB +objid_diag)
	$(call run, ./migrate6 +p4 -stream -c 2000 +objid_diag)
	$(call run, ./migrate6 +p4 -stream -b -c 2000 +objid_diag)

smptest: migrate6
	$(call run, ./migrate6 +p4 +objid_diag)
	$(call run, ./migrate6 +p4 -b +objid_diag)
	$(call run, ./migrate6 +p4 -stream -c 2000 +objid_diag)

clean:
	rm -rf *.decl.h *.def.h migrate6 charmrun

-include ../../common.mk
CHARMC=../../../bin/charmc $(OPTS)

all: loctable

loctable: loctable.decl.h loctable.C
	$(CHARMC) loctable.C -o loctable

loctable.decl.h: loctable.ci
	$(CHARMC) loctable.ci

# The process-level location and index tables under concurrent use by all PEs
# of a process (private instances). Set TESTRUN_PROCS to split the PEs over
# processes; the checks are per process and summed.
test: loctable
	$(call run, ./loctable +p4)
	$(call run, ./loctable +p4 5000 10)
	$(call run, ./loctable +p8 2000 20)

smptest: loctable
	$(call run, ./loctable +p4)

clean:
	rm -rf *.decl.h *.def.h loctable charmrun

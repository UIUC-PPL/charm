-include ../../common.mk
-include ../../../include/conv-mach-opt.mak
CHARMC=../../../bin/charmc $(OPTS)

all: bcast_zcpup

bcast_zcpup.decl.h bcast_zcpup.def.h: bcast_zcpup.ci
	$(CHARMC) bcast_zcpup.ci

bcast_zcpup: bcast_zcpup.C bcast_zcpup.decl.h bcast_zcpup.def.h
	$(CHARMC) -language charm++ bcast_zcpup.C -o bcast_zcpup

# Elements migrate to the PE half the machine away while broadcasts arrive.
# The case this covers needs two processes: set TESTRUN_PROCS=2.
test: bcast_zcpup
	$(call run, ./bcast_zcpup +p2)
	$(call run, ./bcast_zcpup +p4)

smptest: bcast_zcpup
	$(call run, ./bcast_zcpup +p4 ++ppn 2)

clean:
	rm -f *.decl.h *.def.h *.o bcast_zcpup charmrun

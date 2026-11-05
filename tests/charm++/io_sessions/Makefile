-include ../../common.mk
CHARMC=../../../bin/charmc $(OPTS)

all: iosessions

iosessions: iosessions.ci iosessions.C
	$(CHARMC) iosessions.ci
	$(CHARMC) iosessions.C -o $@ -module CkIO

# More CkIO sessions than the standard layout has collection numbers (4095),
# next to two standard arrays. Set TESTRUN_PROCS=2 to spread the PEs over processes.
test: iosessions
	$(call run, ./iosessions +p4)

smptest: iosessions
	$(call run, ./iosessions +p4)

clean:
	rm -f *.o *.decl.h *.def.h iosessions sessions.dat charmrun

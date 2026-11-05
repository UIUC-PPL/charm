-include ../../common.mk
CHARMC=../../../bin/charmc $(OPTS)

all: boundrecreate

boundrecreate: boundrecreate.decl.h boundrecreate.C
	$(CHARMC) boundrecreate.C -o boundrecreate

boundrecreate.decl.h: boundrecreate.ci
	$(CHARMC) boundrecreate.ci

# The case needs two processes: set TESTRUN_PROCS=2. One process prints a note and exits.
test: boundrecreate
	$(call run, ./boundrecreate +p4)

smptest: boundrecreate
	$(call run, ./boundrecreate +p4)

clean:
	rm -f *.decl.h *.def.h *.o boundrecreate charmrun

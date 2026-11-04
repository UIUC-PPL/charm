-include ../../common.mk
CHARMC=../../../bin/charmc $(OPTS)

all: threaded_redn

threaded_redn: threaded_redn.decl.h threaded_redn.C
	$(CHARMC) threaded_redn.C -o threaded_redn

threaded_redn.decl.h: threaded_redn.ci
	$(CHARMC) threaded_redn.ci

test: threaded_redn
	$(call run, ./threaded_redn +p2)
	$(call run, ./threaded_redn +p4)

testp: threaded_redn
	$(call run, ./threaded_redn +p$(P))

smptest: threaded_redn
	$(call run, ./threaded_redn +p2 ++ppn 2)
	$(call run, ./threaded_redn +p4 ++ppn 2)

clean:
	rm -f *.decl.h *.def.h *.o threaded_redn charmrun

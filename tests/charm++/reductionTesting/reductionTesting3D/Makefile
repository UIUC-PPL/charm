-include ../../../common.mk
CHARMDIR = ../../../..
OPTS = -g -Wno-deprecated
CHARMC = $(CHARMDIR)/bin/charmc $(OPTS)

all: reductionTesting3D

reductionTesting3D: Main.o sectionReduction.o
	$(CHARMC) -o reductionTesting3D sectionReduction.o Main.o -module CkMulticast

Main.o: Main.C Main.h main.decl.h main.def.h sectionReduction.decl.h
	$(CHARMC) -c Main.C

sectionReduction.o: sectionReduction.C sectionReduction.h sectionReduction.decl.h sectionReduction.def.h main.decl.h
	$(CHARMC) -c sectionReduction.C

main.decl.h main.def.h: main.ci
	$(CHARMC) main.ci

sectionReduction.decl.h sectionReduction.def.h: sectionReduction.ci
	$(CHARMC) sectionReduction.ci

test: all
	$(call run, ./reductionTesting3D +p4 20 20 20 5 )

testp: all
	$(call run, ./reductionTesting3D +p$(P) $$(( $(P) * 4 )) 20 20 5 )

smptest: all
	$(call run, ./reductionTesting3D 20 20 20 5 +p2 ++ppn 2)
	$(call run, ./reductionTesting3D 20 20 20 5 +p4 ++ppn 2)

clean:
	rm -f *.decl.h *.def.h *.o
	rm -f reductionTesting3D charmrun

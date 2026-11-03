-include ../../common.mk
CHARMC=../../../bin/ampicxx $(OPTS)

all: commdup

commdup: test.o
	$(CHARMC) -o commdup test.o

test.o: test.C
	$(CHARMC) -c test.C

test: commdup
	$(call run, ./commdup +p1 +vp2)
	$(call run, ./commdup +p1 +vp4)
	$(call run, ./commdup +p2 +vp4)

clean:
	rm -f *.o *.mod commdup *~ conv-host charmrun charmrun.exe commdup.exe commdup.pdb commdup.ilk ampirun

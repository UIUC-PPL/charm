-include ../../common.mk
CHARMDIR=../../..
CHARMC=$(CHARMDIR)/bin/charmc $(OPTS)
AMPICC=$(CHARMDIR)/bin/ampicxx $(OPTS)

all: hello

AmpiInterop.decl.h: AmpiInterop.ci
	$(CHARMC) AmpiInterop.ci

AmpiInterop.o: AmpiInterop.C AmpiInterop.decl.h AmpiInterop.h
	$(AMPICC) -c AmpiInterop.C

hello.decl.h: hello.ci
	$(CHARMC) hello.ci

hello.o: hello.C hello.decl.h
	$(CHARMC) -c hello.C

exampleMpi.o: exampleMpi.C
	$(AMPICC) -c exampleMpi.C

hello: AmpiInterop.o  hello.o exampleMpi.o
	$(AMPICC) -o hello hello.o exampleMpi.o AmpiInterop.o

test: all
	$(call run, ./hello +p4 8 +vp8 )

clean:
	rm -f hello *.o *.a charmrun ampirun AmpiInterop.decl.h AmpiInterop.def.h hello.decl.h hello.def.h


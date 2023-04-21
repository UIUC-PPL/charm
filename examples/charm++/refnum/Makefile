CHARMC = ../../../bin/charmc $(OPTS)

all: test

test: test.o
	$(CHARMC) -language charm++ -o $@ $^

test.def.h: test.ci
	$(CHARMC) $<

test.decl.h: test.ci

test.o: test.C test.decl.h test.def.h
	$(CHARMC) -c $<

clean:
	rm -f test *.decl.h *.def.h *.o charmrun

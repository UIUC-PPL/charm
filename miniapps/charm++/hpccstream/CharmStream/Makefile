-include ../../../common.mk
# Override on the command line: make CHARMC=/path/to/charm/bin/charmc
CHARMC=../../../../bin/charmc $(OPTS)


default: all
all:	 stream

stream	:	stream.o
	$(CHARMC)	-o	stream	stream.o 

stream.o : stream.C stream.decl.h stream.def.h
	$(CHARMC)	-c	-o	stream.o	stream.C 

stream.decl.h stream.def.h : stream.ci
	 $(CHARMC) stream.ci

# 10 iterations of the triad over 10^6 doubles per PE, then the program
# recomputes every element and aborts on a mismatch.
test: stream
	$(call run, +p4 ./stream)

clean:
	rm -f stream.decl.h stream.def.h stream.o stream	charmrun





















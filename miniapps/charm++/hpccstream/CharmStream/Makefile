CHARMC=$(HOME)/charm/net-darwin-x86_64/bin/charmc $(OPTS)


default: all
all:	 stream

stream	:	stream.o
	$(CHARMC)	-o	stream	stream.o 

stream.o : stream.C stream.decl.h stream.def.h
	$(CHARMC)	-o	stream.o	stream.C 

stream.decl.h stream.def.h : stream.ci
	 $(CHARMC) stream.ci

clean:
	rm -f stream.decl.h stream.def.h stream.o stream	charmrun





















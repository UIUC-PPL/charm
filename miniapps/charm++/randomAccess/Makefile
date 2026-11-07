-include ../../common.mk
# Override on the command line: make CHARMHOME=/path/to/charm/build
CHARMHOME = ../../..
CHARMC = $(CHARMHOME)/bin/charmc
BINARY = random_access 

SRCFILES = $(wildcard *.cc) 
OBJFILES = $(SRCFILES:.cc=.o) 
CIFILES = $(wildcard *.ci)
HFILES = $(CIFILES:.ci=.decl.h) $(CIFILES:.ci=.def.h)
OPTS ?= -optimize -production
CHARMCFLAGS= $(OPTS)

# This code uses the TRAM interface that Charm++ removed in 2020 (#2775):
# GroupMeshStreamer::ckNew(bufferSize, dims, ...), init(), insertData() and
# done(). It needs porting to the [aggregate] entry-method interface, as in
# examples/charm++/TRAM/aggregateRandomAccessGroup. Until then make all and
# make test only print a message. make random_access tries the build.
SKIP = randomAccess: skipped, uses the removed non-aggregate TRAM (GroupMeshStreamer) interface

all:
	@echo "$(SKIP)"

random_access: $(OBJFILES)
	$(CHARMC) $(CHARMCFLAGS) -language charm++ -o $@ $+ -module NDMeshStreamer

.SECONDARY: $(patsubst %.cc,%.decl.h,$(wildcard *.cc))
.SECONDARY: $(patsubst %.cc,%.def.h,$(wildcard *.cc))

%.def.h %.decl.h: %.ci
	$(CHARMC) $(CHARMCFLAGS) $<

%.o: %.cc

%.o: %.cc $(HFILES)
	$(CHARMC) $(CHARMCFLAGS) $<

# Once ported: 2^14 words per PE; the second update pass undoes the first
# and the program reports the number of wrong table entries. Needs a
# power-of-2 PE count.
#	$(call run, +p4 ./random_access 14)
test:
	@echo "$(SKIP)"

testprj:random_access.prj
	poe ./random_access.prj 20 1 +traceroot /u/home/ac/yanhuas/git/HPCC/charmversion_messages/projections_result -llfile jobpoelapi.cmd +gz-trace

clean:
	rm -f *.o *.decl.h *.def.h $(BINARY) charmrun* 

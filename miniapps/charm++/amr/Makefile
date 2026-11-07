-include ../../common.mk
# Override on the command line: make CHARMHOME=/path/to/charm/build
CHARMHOME = ../../..
CHARMC = $(CHARMHOME)/bin/charmc -I.
CXX=$(CHARMC)

OPTS ?= -O3
CXXFLAGS += -DAMR_REVISION=$(REVNUM) $(OPTS)

OBJS = OctIndex.o Advection.o Main.o 

all: advection

advection: $(OBJS)
	$(CHARMC) $(CXXFLAGS) $(LDFLAGS) -language charm++ -o $@ $^ -module DistributedLB 

Advection.decl.h Main.decl.h: advection.ci.stamp
advection.ci.stamp: advection.ci
	$(CHARMC) $<
	touch $@

Advection.o: Advection.C Advection.h OctIndex.h Main.decl.h Advection.decl.h
Main.o: Main.C Advection.h OctIndex.h Main.decl.h Advection.decl.h
OctIndex.o: OctIndex.C OctIndex.h Advection.decl.h

# max_depth 3, 32^3 zones per block, 60 iterations, load balancing every
# 9. The program checks no result (only internal asserts).
test: all
	$(call run, +p4 ./advection 3 32 60 9 +balancer DistributedLB)

clean:
	rm -f *.decl.h *.def.h conv-host *.o advection charmrun advection.ci.stamp

bgtest: all
	./charmrun advection +p4 10 +x2 +y2 +z2

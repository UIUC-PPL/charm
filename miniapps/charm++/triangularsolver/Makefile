-include ../../common.mk
# Override on the command line: make CHARMDIR=/path/to/charm/build
CHARMDIR = ../../..
CHARMC=$(CHARMDIR)/bin/charmc $(OPTS)

OBJS = sparse_solve.o 

all: sparse_solve

sparse_solve: $(OBJS)
	$(CHARMC) -language charm++ -module CkMulticast -o sparse_solve $(OBJS)  #-tracemode projections 

sparse_solve.decl.h: sparse_solve.ci
	$(CHARMC)  sparse_solve.ci

clean:
	rm -f *.log *.sts *.projrc *.decl.h *.def.h conv-host *.o sparse_solve charmrun

sparse_solve.o: sparse_solve.C ColumnsSolve.h sparse_solve.decl.h MessagePool.h
	$(CHARMC) -c sparse_solve.C
test: all
	$(call run, +p2 ./sparse_solve 2 ./nv_example.rcm)

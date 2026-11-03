-include ../../common.mk
CHARMC=../../../bin/charmc $(OPTS)
MPICXX ?= mpicxx
# The launcher must start one process per MPI rank and give Charm++ the same
# process numbering as MPI, e.g. LAUNCH="srun --mpi=pmi2 -n 2".
LAUNCH ?= mpirun -np 2
PES ?= 2

all: mpi_interop

phaselib.decl.h: phaselib.ci
	$(CHARMC) $<

phaselib.o: phaselib.C phaselib.decl.h phaselib.h
	$(CHARMC) -c phaselib.C

libmodulephaselib.a: phaselib.o
	$(CHARMC) -o $@ phaselib.o

mpi_interop.o: mpi_interop.cpp phaselib.h
	$(CHARMC) -c++ $(MPICXX) -c $< -o $@

mpi_interop: mpi_interop.o libmodulephaselib.a
	$(CHARMC) -ld++ $(MPICXX) -mpi -nomain-module -o $@ mpi_interop.o -L. -module phaselib

test: all
	$(LAUNCH) ./mpi_interop +pe $(PES)

clean:
	rm -f mpi_interop *.o *.a *.decl.h *.def.h charmrun charm_all_libs.sh

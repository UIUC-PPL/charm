-include ../../common.mk
CHARMC=../../../bin/charmc $(OPTS)
MPICXX ?= mpicxx
# LAUNCH must be the launcher the MPI library requires, starting NP
# processes, e.g. LAUNCH="srun --mpi=cray_shasta -n 2" for Cray MPICH.
NP ?= 2
LAUNCH ?= mpirun -np $(NP)
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
	$(LAUNCH) ./mpi_interop $(NP) +pe $(PES)

clean:
	rm -f mpi_interop *.o *.a *.decl.h *.def.h charmrun charm_all_libs.sh

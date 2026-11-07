#@HEADER
# ************************************************************************
# 
#               HPCCG: Simple Conjugate Gradient Benchmark Code
#                 Copyright (2006) Sandia Corporation
# 
# Under terms of Contract DE-AC04-94AL85000, there is a non-exclusive
# license for use of this work by or on behalf of the U.S. Government.
# 
# This library is free software; you can redistribute it and/or modify
# it under the terms of the GNU Lesser General Public License as
# published by the Free Software Foundation; either version 2.1 of the
# License, or (at your option) any later version.
#  
# This library is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#  
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
# USA
# Questions? Contact Michael A. Heroux (maherou@sandia.gov) 
# 
# ************************************************************************
#@HEADER


# Simple hand-tuned makefile.  Modify as necessary for your environment.
# Questions? Contact Mike Heroux (maherou@sandia.gov).
#

#
# 0) Specify compiler and linker:

LINKER=mpic++

CHARMC ?= charmc

# 2) MPI headers:  
#    If you:
#    - Are building MPI mode (-DUSING_MPI is set above).
#    - Do not have the MPI headers installed in a default search directory and
#    - Are not using MPI compiler wrappers
#    Then specify the path to your MPI header file (include a -I)

#MPI_INC = -I/usr/MPICH/SDK.gcc/include


# 3) Specify C++ compiler optimization flags (if any)
#    Typically some reasonably high level of optimization should be used to 
#    enhance performance.

#IA32 with GCC: 
#CPP_OPT_FLAGS = -O3 -funroll-all-loops -malign-double

CPP_OPT_FLAGS = -g # -O3

#
# 4) MPI library:
#    If you:
#    - Are building MPI mode (-DUSING_MPI is set above).
#    - Do not have the MPI library installed a default search directory and
#    - Are not using MPI compiler wrappers for linking
#    Then specify the path to your MPI library (include -L and -l directives)

#MPI_LIB = -L/usr/MPICH/SDK.gcc/lib -lmpich

#
# 5) System libraries: (May need to add -lg2c before -lm)

SYS_LIB =-lm

#
# 6) Specify name if executable (optional):

TARGET = test_HPCCG

################### Derived Quantities (no modification required) ##############

CXXFLAGS= $(CPP_OPT_FLAGS)

LIB_PATHS= $(MPI_LIB) $(SYS_LIB)

TEST_CPP = generate_matrix.cpp read_HPC_row.cpp compute_residual.cpp mytimer.cpp \
          HPC_sparsemv.cpp HPCCG.cpp waxpby.cpp ddot.cpp \
          make_local_matrix.cpp exchange_externals.cpp

TEST_CHARM_CPP = charmHpccg.cpp

MPI_OBJ          = $(TEST_CPP:.cpp=-mpi.o) main-mpi.o
CHARM_OBJ        = $(TEST_CPP:.cpp=-charm.o) charmHpccg.o
SEQ_OBJ          = $(TEST_CPP:.cpp=-seq.o) main-seq.o

TARGETS = seq_HPCCG mpi_HPCCG charm_HPCCG

default: $(TARGETS)

%-seq.cpp: %.cpp
	ln -s $< $@
%-mpi.cpp: %.cpp
	ln -s $< $@
%-charm.cpp: %.cpp
	ln -s $< $@

$(CHARM_OBJ): CXX=$(CHARMC)
$(CHARM_OBJ): CXXFLAGS+=-DUSING_CHARM
charmHpccg.o:  charmHpccg.decl.h

$(MPI_OBJ): CXX=mpic++
$(MPI_OBJ): CXXFLAGS+=-DUSING_MPI $(MPI_INC)

charmHpccg.decl.h: hpccg.ci
	$(CHARMC) $<

charm_HPCCG: charmHpccg.o $(CHARM_OBJ)
	$(CHARMC) -module completion -o $@ $^

mpi_HPCCG: $(MPI_OBJ) main-mpi.o
	$(LINKER) $(CFLAGS) $^ $(LIB_PATHS) -o $@

seq_HPCCG: $(SEQ_OBJ) main-seq.o
	$(LINKER) $(CFLAGS) $^ $(LIB_PATHS) -o $@

clean:
	@rm -f *.o  *~ *-seq.cpp *-mpi.cpp *-charm.cpp $(TARGETS) $(TARGET:=.exe) *.decl.h *.def.h charmrun

-include ../../common.mk
OPTS	= -O3
# Override on the command line: make CHARMC=/path/to/charm/bin/charmc
CHARMC	= ../../../bin/charmc $(OPTS)
CC=mpicxx

# FFTW3: set FFTW_DIR to its install prefix, or it is looked for in the
# usual prefixes. Without it, all and test print a message and succeed.
FFTW_DIR ?=
FFTW_H := $(firstword $(wildcard $(if $(FFTW_DIR),$(FFTW_DIR)/include/fftw3.h) \
            /opt/local/include/fftw3.h /opt/homebrew/include/fftw3.h \
            /usr/local/include/fftw3.h /usr/include/fftw3.h))
FFTW_PREFIX = $(patsubst %/include/fftw3.h,%,$(FFTW_H))
INC = -I$(FFTW_PREFIX)/include
LIBS = -L$(FFTW_PREFIX)/lib -lfftw3 -lm

OBJS = fft1d.o

ifeq ($(FFTW_H),)
all test:
	@echo "fft-trans: skipped, FFTW3 not found"
else
# make (all) builds the Charm++ version; make everything also builds the
# MPI reference (fft_ref, needs fftw3_mpi), fft_bench and the projections build.
all: fft1d

everything: fft1d fft_ref fft1d.prj fft_bench

# A 512x512 transform over 4 chares, then the inverse transform; the
# program prints the residual but checks nothing.
test: fft1d
	$(call run, +p4 ./fft1d 4 512)
endif

fft_bench: fft_bench.o
	${CC} fft_bench.o -o fft_bench $(LIBS)

fft_bench.o: fft_bench.cpp
	${CC} -c fft_bench.cpp $(INC)

fft1d: $(OBJS)
	$(CHARMC) -language charm++ -o fft1d $(OBJS) $(LIBS)

projections: fft1d.prj
fft1d.prj: $(OBJS)
	$(CHARMC) -language charm++ -tracemode projections $(LIBS) -lz -o fft1d.prj $(OBJS)

summary: $(OBJS)
	$(CHARMC) -language charm++ -tracemode summary $(LIBS) -o fft1d.sum $(OBJS)

fft1d.decl.h: fft1d.ci
	$(CHARMC)  fft1d.ci

fft_ref: fft_ref.o
	${CC} fft_ref.o -o fft_ref $(LIBS:-lfftw3=-lfftw3_mpi -lfftw3)

fft_ref.o: fft_ref.cpp
	${CC} -c fft_ref.cpp $(INC)

cleanproj:
	rm -f *.log *.sts *.projrc

clean:
	rm -f *.decl.h *.def.h conv-host *.o fft1d fft1d.prj fft_bench charmrun fft_ref *~

fft1d.o: fft1d.C fft1d.decl.h
	$(CHARMC) $(INC) -c fft1d.C

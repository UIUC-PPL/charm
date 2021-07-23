CDIR := $(shell cd ../../../.. && pwd)
-include $(CDIR)/include/conv-mach-opt.mak
CHARMC=$(CDIR)/bin/charmc $(OPTS)

HEADERS=ampi.h ampif.h ampi_functions.h \
		ampi-interoperate.h ampiimpl.h ampi.decl.h ddt.h \
		$(addprefix romio-stub/,mpio.h mpiof.h mpio_functions.h mpio_globals.h)
HEADDEP=$(HEADERS) \
		../tcharm/tcharm.h ../tcharm/tcharm_impl.h
COMPAT=compat_ampi.o \
       compat_ampim.o compat_ampifm.o compat_ampicm.o \
       compat_ampicpp.o compat_ampins.o compat_ampirs.o
OBJS=ampi.o $(AMPIF_OBJ) ampiOneSided.o \
     ampiMisc.o ddt.o mpich-alltoall.o ampi_mpix.o ampi_noimpl.o

AMPI_LIB=libmoduleampi
AMPI_LIBDIR=$(CDIR)/lib
DEST=$(AMPI_LIBDIR)/$(AMPI_LIB).a
DESTF=$(AMPI_LIBDIR)/$(AMPI_LIB)f.a
COMPATLIB=$(AMPI_LIBDIR)/libampi-compat.a

AMPIF_OBJ=$(AMPI_LIBDIR)/ampif.o
AMPIFIMPL_OBJ=$(AMPI_LIBDIR)/ampifimpl.o
AMPIMOD_OBJ=$(AMPI_LIBDIR)/ampimod.o

FUNCPTR_SHIM_OBJ=$(AMPI_LIBDIR)/ampi_funcptr_shim.o
FUNCPTR_SHIM_MAIN_OBJ=$(AMPI_LIBDIR)/ampi_funcptr_shim_main.o
FUNCPTR_SHIM_FORTRAN_OBJ=$(AMPI_LIBDIR)/ampi_funcptr_shim_fortran.o
FUNCPTR_SHIM_FORTRAN_IMPL_OBJ=$(AMPI_LIBDIR)/ampi_funcptr_shim_fortran_impl.o
FUNCPTR_SHIM_FORTRAN_MOD_OBJ=$(AMPI_LIBDIR)/ampi_funcptr_shim_fortran_mod.o
FUNCPTR_SHIM_FORTRAN_ROMIO_OBJ=$(AMPI_LIBDIR)/ampi_funcptr_shim_fortran_romio.o
FUNCPTR_SHIM_FMPIMAIN_OBJ=$(AMPI_LIBDIR)/ampi_funcptr_shim_fmpimain.o
FUNCPTR_LOADER_OBJ=$(AMPI_LIBDIR)/ampi_funcptr_loader.o
FUNCPTR_LOADER_STUB_OBJ=$(AMPI_LIBDIR)/ampi_funcptr_loader_stub.o
GLOBALS_PIP_OBJ=$(AMPI_LIBDIR)/ampi_globals_pip.o
GLOBALS_FS_OBJ=$(AMPI_LIBDIR)/ampi_globals_fs.o
GLOBALS_PIE_OBJ=$(AMPI_LIBDIR)/ampi_globals_pie.o

AMPI_TARGET = \
  $(DEST) \
  $(FUNCPTR_SHIM_OBJ) \
  $(FUNCPTR_SHIM_MAIN_OBJ) \
  $(FUNCPTR_LOADER_OBJ) \
  $(FUNCPTR_LOADER_STUB_OBJ) \

AMPIF_TARGET = \
  $(DESTF) \
  $(FUNCPTR_SHIM_FORTRAN_OBJ) \
  $(FUNCPTR_SHIM_FORTRAN_IMPL_OBJ) \
  $(FUNCPTR_SHIM_FORTRAN_MOD_OBJ) \
  $(FUNCPTR_SHIM_FMPIMAIN_OBJ) \

ifeq (1,$(CMK_SUPPORTS_PIPGLOBALS))
  AMPI_TARGET += $(GLOBALS_PIP_OBJ) $(GLOBALS_PIE_OBJ)
endif
ifeq (1,$(CMK_SUPPORTS_FSGLOBALS))
  AMPI_TARGET += $(GLOBALS_FS_OBJ)
endif

ifeq ($(CMK_AMPI_WITH_ROMIO),1)
  ROMIO = romio/.libs/libromio.a

  ROMIO_OBJ_DIRS := \
    romio/adio/ad_ufs \
    romio/adio/common \
    romio/adio/ad_nfs \
    romio/adio/ad_testfs \
    romio/mpi-io \
    romio/mpi-io/glue/default \
    romio/mpi-io/glue \

  ROMIO_FORTRAN_OBJ_DIRS := \
    romio/mpi-io/fortran \

  ROMIO_OBJS = \
    $(foreach o,o obj, \
      $(foreach d,$(ROMIO_OBJ_DIRS), \
        $(wildcard $d/*.$o) \
      ) \
    )

  ROMIO_FORTRAN_OBJS = \
    $(foreach o,o obj, \
      $(foreach d,$(ROMIO_FORTRAN_OBJ_DIRS), \
        $(wildcard $d/*.$o) \
      ) \
    )
endif

ROMIO_FLAGS = -DAMPI_NO_UNIMPLEMENTED_WARNINGS -standalone -fPIC

ifneq ($(CMK_WINDOWS),1)
  obj_suf := .o
else
  obj_suf := .obj
endif

.SUFFIXES:
.SUFFIXES: .C .o

.C.o:
	$(CHARMC) -c $<

MAJOR_TARGETS := AMPI
ifeq (1,$(CMK_CAN_LINK_FORTRAN))
  MAJOR_TARGETS += AMPIF
  ROMIO_F77 := yes
else
  ROMIO_F77 := no
endif

all: $(MAJOR_TARGETS) $(COMPATLIB) $(ROMIO)

$(ROMIO): headers
	@echo "Building ROMIO"
	cd romio &&                                                                  \
		MPI_IMPL="ampi" FROM_MPICH="no" FROM_LAM="no" FROM_OMPI="no"         \
		MPI_LIB=""                                                           \
		CFLAGS="$(OPTS) $(ROMIO_FLAGS)"        \
		FFLAGS="$(OPTS) $(ROMIO_FLAGS)"        \
		F77="$(CDIR)/bin/ampif90"                                            \
		CC="$(CDIR)/bin/ampicc"                                              \
		./configure                                                          \
		--disable-aio                                                        \
		--enable-f77=$(ROMIO_F77)                                            \
		--enable-static=yes                                                  \
		--enable-shared=no                                                   \
		--disable-weak-symbols                                               \
		--srcdir=`pwd`                                                       \
		|| ( cat config.log ; exit 1 )

	cp romio/include/mpio.h romio/include/mpiof.h romio/include/mpio_functions.h $(CDIR)/include
	$(MAKE) -C romio AUTOCONF=: AUTOHEADER=: AUTOMAKE=: ACLOCAL=:
ifeq (1,$(CMK_CAN_LINK_FORTRAN))
	cp romio/mpi-io/fortran/romio_fortran$(obj_suf) $(FUNCPTR_SHIM_FORTRAN_ROMIO_OBJ)
endif
	@echo "ROMIO built successfully"

AMPI: $(AMPI_TARGET)
	@echo "AMPI built successfully"

AMPIF: $(AMPIF_TARGET)
	@echo "AMPIF built successfully"

$(DEST): $(OBJS) headers $(ROMIO)
	$(CHARMC) $(OBJS) $(ROMIO_OBJS) -o $@
	cp $(AMPI_LIB).dep $(AMPI_LIBDIR)/$(AMPI_LIB).dep

$(DESTF): $(OBJS) headers $(ROMIO)
	$(CHARMC) -c ampifimpl.f90 -o $(AMPIFIMPL_OBJ)
	$(CHARMC) -c ampimod.f90 -o $(AMPIMOD_OBJ)
	$(CHARMC) -cpmod ../../../../include mpi.M
	$(CHARMC) $(OBJS) $(ROMIO_OBJS) $(ROMIO_FORTRAN_OBJS) $(AMPIFIMPL_OBJ) $(AMPIMOD_OBJ) -o $@
	cp $(AMPI_LIB)f.dep $(AMPI_LIBDIR)/$(AMPI_LIB)f.dep

$(COMPATLIB): $(COMPAT)
	$(CHARMC) $(COMPAT) -o $@

headers: $(HEADERS)
	cp $(HEADERS) $(CDIR)/include/

	# Put AMPI's mpi.h in a separate directory to support the MPI machine layer and MPI interop
	mkdir -p $(CDIR)/include/ampi/
	cp ampi.h $(CDIR)/include/ampi/mpi.h
	cp ampif.h $(CDIR)/include/mpif.h

	cp ampiCC $(CDIR)/bin/ampicc
	cp ampiCC $(CDIR)/bin/ampiCC
	cp ampiCC $(CDIR)/bin/ampicxx
	cp ampiCC $(CDIR)/bin/ampic++
	cp ampif77 $(CDIR)/bin/ampif77
	cp ampif90 $(CDIR)/bin/ampif90
	cp ampif90 $(CDIR)/bin/ampifort
	cp ampirun $(CDIR)/bin/ampirun
	cp ampirun $(CDIR)/bin/ampiexec
	cp ampiCC $(CDIR)/bin/mpicc.ampi
	cp ampiCC $(CDIR)/bin/mpiCC.ampi
	cp ampiCC $(CDIR)/bin/mpicxx.ampi
	cp ampiCC $(CDIR)/bin/mpic++.ampi
	cp ampif77 $(CDIR)/bin/mpif77.ampi
	cp ampif90 $(CDIR)/bin/mpif90.ampi
	cp ampif90 $(CDIR)/bin/mpifort.ampi
	cp ampirun $(CDIR)/bin/mpirun.ampi
	cp ampirun $(CDIR)/bin/mpiexec.ampi
	mkdir -p $(CDIR)/bin/ampi/
	cp pathstub.sh $(CDIR)/bin/ampi/mpicc
	cp pathstub.sh $(CDIR)/bin/ampi/mpiCC
	cp pathstub.sh $(CDIR)/bin/ampi/mpicxx
	cp pathstub.sh $(CDIR)/bin/ampi/mpic++
	cp pathstub.sh $(CDIR)/bin/ampi/mpif77
	cp pathstub.sh $(CDIR)/bin/ampi/mpif90
	cp pathstub.sh $(CDIR)/bin/ampi/mpifort
	cp pathstub.sh $(CDIR)/bin/ampi/mpirun
	cp pathstub.sh $(CDIR)/bin/ampi/mpiexec
	cp nm_globals.sh $(CDIR)/bin/nm_globals.sh
	touch headers

compat_ampim.o: compat_ampim.C

compat_ampifm.o: compat_ampifm.C

compat_ampicm.o: compat_ampicm.C

compat_ampi.o: compat_ampi.c
	$(CHARMC) -c compat_ampi.c

compat_ampins.o: compat_ampins.C
	$(CHARMC) -c $<

compat_ampirs.o: compat_ampirs.C
	$(CHARMC) -c $<

ampi_mpix.o: ampi_mpix.C $(HEADDEP) headers

ampi_noimpl.o: ampi_noimpl.C $(HEADDEP) headers
	$(CHARMC) -c $< -DAMPI_NO_UNIMPLEMENTED_WARNINGS

compat_ampicpp.o: compat_ampicpp.C
	$(CHARMC) -c compat_ampicpp.C

ampi.o: ampi.C $(HEADDEP) headers
	$(CHARMC) -c ampi.C

$(AMPIF_OBJ): ampif.C $(HEADDEP) headers
	$(CHARMC) -c $< -o $@

ampiOneSided.o: ampiOneSided.C ampiimpl.h $(HEADDEP) headers
	$(CHARMC) -c ampiOneSided.C

ampiMisc.o: ampiMisc.C ampiimpl.h $(HEADDEP) headers
	$(CHARMC) -c ampiMisc.C

mpich-alltoall.o: mpich-alltoall.C $(HEADDEP) headers
	$(CHARMC) -c mpich-alltoall.C

ampi.decl.h ampi.def.h: ampi.ci
	$(CHARMC) ampi.ci

ddt.o: ddt.C ddt.h $(HEADDEP) headers

$(FUNCPTR_SHIM_OBJ): ampi_funcptr_shim.C $(HEADDEP) headers | $(ROMIO)
	$(CHARMC) -language ampi -DAMPI_USE_FUNCPTR -DAMPI_NO_UNIMPLEMENTED_WARNINGS -fPIE -shared -c $< -o $@

$(FUNCPTR_SHIM_MAIN_OBJ): ampi_funcptr_shim_main.C | $(ROMIO)
	$(CHARMC) -language ampi -fPIE -c $< -o $@

$(FUNCPTR_SHIM_FMPIMAIN_OBJ): ampi_funcptr_shim_fmpimain.C | $(ROMIO)
	$(CHARMC) -language ampi -fPIE -c $< -o $@

$(FUNCPTR_SHIM_FORTRAN_OBJ): ampif.C $(HEADDEP) headers | $(ROMIO)
	$(CHARMC) -language ampi -DAMPI_USE_FUNCPTR -fPIE -shared -c $< -o $@

$(FUNCPTR_SHIM_FORTRAN_IMPL_OBJ): ampifimpl.f90 $(HEADDEP) headers | $(ROMIO)
	$(CHARMC) -language ampi -fPIE -shared -c $< -o $@

$(FUNCPTR_SHIM_FORTRAN_MOD_OBJ): ampimod.f90 $(HEADDEP) headers | $(ROMIO)
	$(CHARMC) -language ampi -fPIE -shared -c $< -o $@

$(FUNCPTR_LOADER_OBJ): ampi_funcptr_loader.C $(HEADDEP) headers | $(ROMIO)
	$(CHARMC) -language ampi -DAMPI_NO_UNIMPLEMENTED_WARNINGS -c $< -o $@

$(FUNCPTR_LOADER_STUB_OBJ): ampi_funcptr_loader_stub.C $(HEADDEP) headers | $(ROMIO)
	$(CHARMC) -language ampi -c $< -o $@

$(GLOBALS_FS_OBJ): ampi_globals_fs.C $(HEADDEP) headers | $(ROMIO)
	$(CHARMC) -language ampi -c $< -o $@

$(GLOBALS_PIP_OBJ): ampi_globals_pip.C $(HEADDEP) headers | $(ROMIO)
	$(CHARMC) -language ampi -c $< -o $@

$(GLOBALS_PIE_OBJ): ampi_globals_pie.C $(HEADDEP) headers | $(ROMIO)
	$(CHARMC) -language ampi -c $< -o $@

clean:
	rm -fr *.o *~ *.decl.h *.def.h gmon.out $(DEST) $(DESTF) $(COMPATLIB) conv-host charmrun headers *.mod
	-$(MAKE) -C romio clean

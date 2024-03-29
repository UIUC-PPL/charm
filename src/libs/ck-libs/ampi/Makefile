CDIR := $(shell cd ../../../.. && pwd)
-include $(CDIR)/include/conv-mach-opt.mak
CHARMC=$(CDIR)/bin/charmc $(OPTS)

PRE_HEADERS=ampi.h ampi_functions.h $(addprefix romio-stub/,mpio_globals.h)
ifeq (1,$(CMK_CAN_LINK_FORTRAN))
  PRE_HEADERS += ampif.h
endif
HEADERS=$(PRE_HEADERS) ampi-interoperate.h ampiimpl.h ampi.decl.h ddt.h
ROMIO_HEADER_FILES := mpio.h mpio_functions.h
ifeq (1,$(CMK_CAN_LINK_FORTRAN))
  ROMIO_HEADER_FILES += mpiof.h
endif
ROMIO_HEADERS:=$(addprefix romio/include/,$(ROMIO_HEADER_FILES))
ROMIO_STUB_HEADERS:=$(addprefix romio-stub/,$(ROMIO_HEADER_FILES))
ifneq ($(CMK_AMPI_WITH_ROMIO),1)
  HEADERS+= $(ROMIO_STUB_HEADERS)
endif
HEADDEP=$(HEADERS) \
		../tcharm/tcharm.h ../tcharm/tcharm_impl.h
COMPAT=compat_ampim.o compat_ampifm.o \
       compat_ampicpp.o compat_ampins.o compat_ampirs.o
OBJS=ampi.o $(AMPIF_OBJ) ampiOneSided.o \
     ampiMisc.o ddt.o mpich-alltoall.o ampi_mpix.o ampi_noimpl.o

AMPI_LIB=libmoduleampi
AMPI_LIBDIR=$(CDIR)/lib
DEST=$(AMPI_LIBDIR)/$(AMPI_LIB).a
DESTF=$(AMPI_LIBDIR)/$(AMPI_LIB)f.a
COMPATLIB=$(AMPI_LIBDIR)/libampi-compat.a

AMPIF_OBJ=$(AMPI_LIBDIR)/ampif.o
AMPIMOD_OBJ=$(AMPI_LIBDIR)/ampimod.o

FUNCPTR_SHIM_OBJ=$(AMPI_LIBDIR)/ampi_funcptr_shim.o
FUNCPTR_SHIM_MAIN_OBJ=$(AMPI_LIBDIR)/ampi_funcptr_shim_main.o
FUNCPTR_SHIM_FORTRAN_OBJ=$(AMPI_LIBDIR)/ampi_funcptr_shim_fortran.o
FUNCPTR_SHIM_FORTRAN_MOD_OBJ=$(AMPI_LIBDIR)/ampi_funcptr_shim_fortran_mod.o
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

ifeq (1,$(CMK_SUPPORTS_PIEGLOBALS))
  AMPI_TARGET += $(GLOBALS_PIE_OBJ)
endif
ifeq (1,$(CMK_SUPPORTS_PIPGLOBALS))
  AMPI_TARGET += $(GLOBALS_PIP_OBJ)
endif
ifeq (1,$(CMK_SUPPORTS_FSGLOBALS))
  AMPI_TARGET += $(GLOBALS_FS_OBJ)
endif

ifeq ($(CMK_AMPI_WITH_ROMIO),1)
  ROMIO = romio/.libs/libromio.a
  ROMIO_MAKEFILE = romio/Makefile

  ROMIO_OBJ_DIRS := \
    romio/adio/ad_ufs \
    romio/adio/common \
    romio/adio/ad_nfs \
    romio/adio/ad_testfs \
    romio/mpi-io \
    romio/mpi-io/glue/default \
    romio/mpi-io/glue \

  ROMIO_OBJS = \
    $(foreach o,o obj, \
      $(foreach d,$(ROMIO_OBJ_DIRS), \
        $(wildcard $d/*.$o) \
      ) \
    )
endif

ROMIO_FLAGS = -DAMPI_NO_UNIMPLEMENTED_WARNINGS -seq -fPIC

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
  ROMIO_ENABLE_FORTRAN := yes
else
  ROMIO_ENABLE_FORTRAN := no
endif

all: $(MAJOR_TARGETS) $(COMPATLIB) $(ROMIO)

$(ROMIO_MAKEFILE): pre-headers
	@echo "Building ROMIO"
	cp $(ROMIO_STUB_HEADERS) $(CDIR)/include/
	cd romio && \
		./configure \
		--disable-aio \
		--enable-f77=$(ROMIO_ENABLE_FORTRAN) \
		--enable-f90=$(ROMIO_ENABLE_FORTRAN) \
		--enable-static=yes \
		--enable-shared=no \
		--disable-weak-symbols \
		--srcdir="$(pwd)" \
		MPI_IMPL=ampi \
		FROM_MPICH=no \
		FROM_LAM=no \
		FROM_OMPI=no \
		MPI_LIB= \
		CC="$(CDIR)/bin/ampicc" \
		F77="$(CDIR)/bin/ampif77" \
		FC="$(CDIR)/bin/ampif90" \
		CFLAGS="$(OPTS) $(ROMIO_FLAGS)" \
		FFLAGS="$(OPTS) $(ROMIO_FLAGS)" \
		FCFLAGS="$(OPTS) $(ROMIO_FLAGS)" \
		CPP= \
		CPPFLAGS= \
		LIBS= \
		LDFLAGS= \
		TEST_CC="$(CDIR)/bin/ampicc" \
		TEST_F77="$(CDIR)/bin/ampif77" \
		USER_CFLAGS="$(OPTS) $(ROMIO_FLAGS)" \
		USER_FFLAGS="$(OPTS) $(ROMIO_FLAGS)" \
		|| ( cat config.log ; exit 1 )
	cp $(ROMIO_HEADERS) $(CDIR)/include/

$(ROMIO): $(ROMIO_MAKEFILE)
	$(MAKE) -C romio AUTOCONF=: AUTOHEADER=: AUTOMAKE=: ACLOCAL=:
	@echo "ROMIO built successfully"

romio-target: $(ROMIO)
	touch $@

AMPI: $(AMPI_TARGET)
	@echo "AMPI built successfully"

AMPIF: $(AMPIF_TARGET)
	@echo "AMPIF built successfully"

$(DEST): $(OBJS) headers $(ROMIO)
	$(CHARMC) $(OBJS) $(ROMIO_OBJS) -o $@
	cp $(AMPI_LIB).dep $(AMPI_LIBDIR)/$(AMPI_LIB).dep

$(DESTF): $(OBJS) headers $(ROMIO)
	$(CHARMC) -c ampimod.f90 -o $(AMPIMOD_OBJ)
	$(CHARMC) -cpmod ../../../../include mpi.M
	$(CHARMC) $(OBJS) $(ROMIO_OBJS) $(AMPIMOD_OBJ) -o $@
	cp $(AMPI_LIB)f.dep $(AMPI_LIBDIR)/$(AMPI_LIB)f.dep

$(COMPATLIB): $(COMPAT)
	$(CHARMC) $(COMPAT) -o $@

pre-headers: $(PRE_HEADERS) ampiCC ampif77 ampif90 ampirun pathstub.sh nm_globals.sh
	# Put AMPI's mpi.h in a separate directory to support the MPI machine layer and MPI interop
	mkdir -p $(CDIR)/include/ampi/
	cp ampi.h $(CDIR)/include/ampi/mpi.h
ifeq (1,$(CMK_CAN_LINK_FORTRAN))
	cp ampif.h $(CDIR)/include/mpif.h
endif
	cp $(PRE_HEADERS) $(CDIR)/include/
	cp ampiCC $(CDIR)/bin/ampicc
	cp ampiCC $(CDIR)/bin/ampiCC
	cp ampiCC $(CDIR)/bin/ampicxx
	cp ampiCC $(CDIR)/bin/ampic++
ifeq (1,$(CMK_CAN_LINK_FORTRAN))
	cp ampif77 $(CDIR)/bin/ampif77
	cp ampif90 $(CDIR)/bin/ampif90
	cp ampif90 $(CDIR)/bin/ampifort
endif
	cp ampirun $(CDIR)/bin/ampirun
	cp ampirun $(CDIR)/bin/ampiexec
	cp ampiCC $(CDIR)/bin/mpicc.ampi
	cp ampiCC $(CDIR)/bin/mpiCC.ampi
	cp ampiCC $(CDIR)/bin/mpicxx.ampi
	cp ampiCC $(CDIR)/bin/mpic++.ampi
ifeq (1,$(CMK_CAN_LINK_FORTRAN))
	cp ampif77 $(CDIR)/bin/mpif77.ampi
	cp ampif90 $(CDIR)/bin/mpif90.ampi
	cp ampif90 $(CDIR)/bin/mpifort.ampi
endif
	cp ampirun $(CDIR)/bin/mpirun.ampi
	cp ampirun $(CDIR)/bin/mpiexec.ampi
	mkdir -p $(CDIR)/bin/ampi/
	cp pathstub.sh $(CDIR)/bin/ampi/mpicc
	cp pathstub.sh $(CDIR)/bin/ampi/mpiCC
	cp pathstub.sh $(CDIR)/bin/ampi/mpicxx
	cp pathstub.sh $(CDIR)/bin/ampi/mpic++
ifeq (1,$(CMK_CAN_LINK_FORTRAN))
	cp pathstub.sh $(CDIR)/bin/ampi/mpif77
	cp pathstub.sh $(CDIR)/bin/ampi/mpif90
	cp pathstub.sh $(CDIR)/bin/ampi/mpifort
endif
	cp pathstub.sh $(CDIR)/bin/ampi/mpirun
	cp pathstub.sh $(CDIR)/bin/ampi/mpiexec
	cp nm_globals.sh $(CDIR)/bin/nm_globals.sh
	touch $@

headers: pre-headers $(HEADERS)
	cp $(HEADERS) $(CDIR)/include/
	touch $@

compat_ampim.o: compat_ampim.C

compat_ampifm.o: compat_ampifm.C

compat_ampins.o: compat_ampins.C
	$(CHARMC) -c $<

compat_ampirs.o: compat_ampirs.C
	$(CHARMC) -c $<

ampi_mpix.o: ampi_mpix.C $(HEADDEP) headers | $(ROMIO_MAKEFILE)
	$(CHARMC) -language ampi -c $< -o $@

ampi_noimpl.o: ampi_noimpl.C $(HEADDEP) headers | $(ROMIO_MAKEFILE)
	$(CHARMC) -language ampi -c $< -o $@ -DAMPI_NO_UNIMPLEMENTED_WARNINGS

compat_ampicpp.o: compat_ampicpp.C
	$(CHARMC) -c compat_ampicpp.C

ampi.o: ampi.C $(HEADDEP) headers | $(ROMIO_MAKEFILE)
	$(CHARMC) -language ampi -c $< -o $@

$(AMPIF_OBJ): ampif.C $(HEADDEP) headers | $(ROMIO_MAKEFILE)
	$(CHARMC) -language ampi -c $< -o $@

ampiOneSided.o: ampiOneSided.C ampiimpl.h $(HEADDEP) headers | $(ROMIO_MAKEFILE)
	$(CHARMC) -language ampi -c $< -o $@

ampiMisc.o: ampiMisc.C ampiimpl.h $(HEADDEP) headers | $(ROMIO_MAKEFILE)
	$(CHARMC) -language ampi -c $< -o $@

mpich-alltoall.o: mpich-alltoall.C $(HEADDEP) headers | $(ROMIO_MAKEFILE)
	$(CHARMC) -language ampi -c $< -o $@

ampi.decl.h ampi.def.h: ampi.ci | $(ROMIO_MAKEFILE)
	$(CHARMC) ampi.ci

ddt.o: ddt.C ddt.h $(HEADDEP) headers | $(ROMIO_MAKEFILE)
	$(CHARMC) -language ampi -c $< -o $@

$(FUNCPTR_SHIM_OBJ): ampi_funcptr_shim.C $(HEADDEP) headers | $(ROMIO_MAKEFILE)
	$(CHARMC) -language ampi -DAMPI_USE_FUNCPTR -DAMPI_NO_UNIMPLEMENTED_WARNINGS -fPIE -shared -c $< -o $@

$(FUNCPTR_SHIM_MAIN_OBJ): ampi_funcptr_shim_main.C | $(ROMIO_MAKEFILE)
	$(CHARMC) -language ampi -fPIE -c $< -o $@

$(FUNCPTR_SHIM_FMPIMAIN_OBJ): ampi_funcptr_shim_fmpimain.C | $(ROMIO_MAKEFILE)
	$(CHARMC) -language ampi -fPIE -c $< -o $@

$(FUNCPTR_SHIM_FORTRAN_OBJ): ampif.C $(HEADDEP) headers | $(ROMIO_MAKEFILE)
	$(CHARMC) -language ampi -DAMPI_USE_FUNCPTR -fPIE -shared -c $< -o $@

$(FUNCPTR_SHIM_FORTRAN_MOD_OBJ): ampimod.f90 $(HEADDEP) headers | $(ROMIO_MAKEFILE)
	$(CHARMC) -language ampi -fPIE -shared -c $< -o $@

$(FUNCPTR_LOADER_OBJ): ampi_funcptr_loader.C $(HEADDEP) headers | $(ROMIO_MAKEFILE)
	$(CHARMC) -language ampi -DAMPI_NO_UNIMPLEMENTED_WARNINGS -c $< -o $@

$(FUNCPTR_LOADER_STUB_OBJ): ampi_funcptr_loader_stub.C $(HEADDEP) headers | $(ROMIO_MAKEFILE)
	$(CHARMC) -language ampi -c $< -o $@

$(GLOBALS_FS_OBJ): ampi_globals_fs.C $(HEADDEP) headers | $(ROMIO_MAKEFILE)
	$(CHARMC) -language ampi -c $< -o $@

$(GLOBALS_PIP_OBJ): ampi_globals_pip.C $(HEADDEP) headers | $(ROMIO_MAKEFILE)
	$(CHARMC) -language ampi -c $< -o $@

$(GLOBALS_PIE_OBJ): ampi_globals_pie.C $(HEADDEP) headers | $(ROMIO_MAKEFILE)
	$(CHARMC) -language ampi -c $< -o $@

clean:
	rm -fr *.o *~ *.decl.h *.def.h gmon.out $(DEST) $(DESTF) $(COMPATLIB) conv-host charmrun *.mod headers pre-headers romio-target
	-$(MAKE) -C romio clean

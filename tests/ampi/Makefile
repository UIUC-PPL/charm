-include ../../include/conv-mach-opt.mak

DIRS = \
  megampi \
  ibarrier \
  cas \
  onesided \
  winlock \
  coll_extent \
  stacksize \
  migration \
  datatype \
  collwake \
  collmatch \
  migrate_colls \
  scan \
  waitall_ata \
  privatization \
  jacobi3d \
  exit \
  chkpt \
  intercomm_merge \
  groupfree \
  bsend \
  wildcard \
  commdup \
  intercomm_coll \
  reduce_ops \

ifeq (1,$(CMK_CAN_LINK_FORTRAN))
  DIRS += fallreduce
endif

NONSCALEDIRS = \
  privatization \
  datatype \
  exit \
  chkpt \

ifeq ($(BUILD_MPICH_TESTS),true)
DIRS += mpich-test
endif

FTDIRS = \
  jacobi3d

# Randomization on: a restart from a checkpoint (chkpt) is a new process that
# resumes each rank's thread, and the thread's stack holds return addresses
# into libreconverse and the program. They are valid only if the new process
# loads both at the addresses the checkpointing process used, which requires
# address space layout randomization (ASLR) to be off. On Linux testrun puts
# setarch -R in front of every run, so chkpt runs there, except when
# TESTRUN_VA_RANDOMIZATION=1 asks testrun to leave randomization on. macOS
# has no way to turn it off for these runs, and Isomalloc refuses the
# restart, so chkpt is left out on macOS for every process count.
#
# A rank migrating between processes needs the same, and Isomalloc refuses
# the migration otherwise. macOS has no setarch; only a launcher that spawns
# the processes with _POSIX_SPAWN_DISABLE_ASLR (as classic charmrun did) can
# turn ASLR off there, and lcrun does not. So with randomization on and more
# than one process (TESTRUN_PROCS), leave out the tests whose ranks migrate.
# One process migrates fine.
ifeq ($(CMK_GDIR),reconverse)
ifeq ($(CMK_MACOSX),1)
RECONVERSE_VA_RANDOMIZED = 1
endif
ifeq ($(TESTRUN_VA_RANDOMIZATION),1)
RECONVERSE_VA_RANDOMIZED = 1
endif
ifeq ($(RECONVERSE_VA_RANDOMIZED),1)
RECONVERSE_NA = \
  chkpt \

ifneq ($(filter-out 0 1,$(TESTRUN_PROCS)),)
RECONVERSE_NA += \
  migration \
  megampi \
  onesided \
  privatization \
  scan \
  waitall_ata \
  collmatch \
  migrate_colls \

endif
endif
endif

TESTDIRS = $(filter-out $(FTDIRS) $(RECONVERSE_NA),$(DIRS))

TESTPDIRS = $(filter-out $(NONSCALEDIRS),$(TESTDIRS))

all: $(foreach i,$(DIRS),build-$i)

test: $(foreach i,$(TESTDIRS),test-$i)

testp: $(foreach i,$(TESTPDIRS),testp-$i)

syncfttest : $(foreach i,$(filter $(FTDIRS),$(DIRS)),test-$i)

mpisyncfttest :

clean: $(foreach i,$(DIRS),clean-$i)
	rm -f TAGS #*#
	rm -f core *~

$(foreach i,$(DIRS),build-$i):
	$(MAKE) -C $(subst build-,,$@) all OPTS='$(OPTS)'

$(foreach i,$(DIRS),test-$i):
	$(MAKE) -C $(subst test-,,$@) test OPTS='$(OPTS)' TESTOPTS='$(TESTOPTS)'

$(foreach i,$(TESTPDIRS),testp-$i):
	$(MAKE) -C $(subst testp-,,$@) testp OPTS='$(OPTS)' TESTOPTS='$(TESTOPTS)' P='$(P)'

$(foreach i,$(DIRS),clean-$i):
	$(MAKE) -C $(subst clean-,,$@) clean OPTS='$(OPTS)'


# additional dependencies

mpich-test/Makefile:
	cd mpich-test && ./configure.sh && cd ..

build-mpich-test: mpich-test/Makefile
test-mpich-test: mpich-test/Makefile

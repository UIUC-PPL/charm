-include ../../include/conv-mach-opt.mak

DIRS = \
  megampi \
  ibarrier \
  cas \
  onesided \
  stacksize \
  migration \
  datatype \
  collwake \
  collmatch \
  migrate_colls \
  scan \
  privatization \
  jacobi3d \
  exit \
  chkpt \
  intercomm_merge \
  groupfree \
  bsend \
  wildcard \
#  intercomm_coll \ # causes hangs in SMP mode

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

# Not yet run on reconverse: restarting from a checkpoint resumes each rank's
# thread in a new process, and its stack holds return addresses into
# libreconverse, which ASLR loads at a different address in every process.
ifeq ($(CMK_GDIR),reconverse)
RECONVERSE_NA = \
  chkpt \

# The same holds for a rank migrating between processes. Linux runs turn ASLR
# off (setarch -R). macOS has no setarch; only a launcher that spawns the
# processes with _POSIX_SPAWN_DISABLE_ASLR (as classic charmrun did) can turn
# it off there, and lcrun does not, so with more than one process
# (TESTRUN_PROCS) leave out the tests whose ranks migrate. One process
# migrates fine.
ifeq ($(CMK_MACOSX),1)
ifneq ($(filter-out 0 1,$(TESTRUN_PROCS)),)
RECONVERSE_NA += \
  migration \
  megampi \
  privatization \
  scan \

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

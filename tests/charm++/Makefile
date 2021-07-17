-include ../../include/conv-mach-opt.mak

DIRS = \
  megatest \
  alignment \
  simplearrayhello \
  provisioning \
  load_balancing \
  chkpt \
  delegation \
  queue \
  sdag \
  ckAllocSysMsgTest \
  method_templates \
  demand_creation \
  startupTest \
  topology \
  io \
  sparse \
  reductionTesting \
  partitions \
  charmxi_parsing \
  jacobi3d \
  jacobi3d-sdag \
  zerocopy \
  within_node_bcast \
  longIdle \
  bombard \
  varTRAM \
  atsync \

FTDIRS = \
  jacobi3d \
  jacobi3d-sdag \

TESTDIRS = $(filter-out $(FTDIRS),$(DIRS))

NONSCALEDIRS = \
  alignment \
  queue \
  sdag \
  sparse \
  charmxi_parsing \

TESTPDIRS = $(filter-out $(NONSCALEDIRS),$(TESTDIRS))

all: $(foreach i,$(DIRS),build-$i)

test: $(foreach i,$(TESTDIRS),test-$i)
ifeq ($(CMK_SMP),1)
ifneq ($(CMK_MULTICORE),1)
	make smptest
endif
endif

testp: $(foreach i,$(TESTPDIRS),testp-$i)

smptest: $(foreach i,$(TESTDIRS),smptest-$i)

syncfttest mpisyncfttest: $(foreach i,$(filter $(FTDIRS),$(DIRS)),test-$i)

clean: $(foreach i,$(DIRS),clean-$i)
	rm -f TAGS #*#
	rm -f core *~

$(foreach i,$(DIRS),build-$i):
	$(MAKE) -C $(subst build-,,$@) all OPTS='$(OPTS)'

$(foreach i,$(DIRS),test-$i):
	$(MAKE) -C $(subst test-,,$@) test OPTS='$(OPTS)' TESTOPTS='$(TESTOPTS)'

$(foreach i,$(TESTPDIRS),testp-$i):
	$(MAKE) -C $(subst testp-,,$@) testp OPTS='$(OPTS)' TESTOPTS='$(TESTOPTS)'

$(foreach i,$(DIRS),smptest-$i):
	$(MAKE) -C $(subst smptest-,,$@) smptest OPTS='$(OPTS)' TESTOPTS='$(TESTOPTS)'

$(foreach i,$(DIRS),clean-$i):
	$(MAKE) -C $(subst clean-,,$@) clean OPTS='$(OPTS)'

-include ../../include/conv-mach-opt.mak

DIRS = \
  megatest \
  alignment \
  simplearrayhello \
  migratable_threads \
  provisioning \
  load_balancing \
  chkpt \
  delegation \
  sdag \
  ckAllocSysMsgTest \
  method_templates \
  demand_creation \
  startupTest \
  topology \
  io \
  io_read \
  io_sessions \
  sparse \
  reductionTesting \
  partitions \
  charmxi_parsing \
  jacobi3d \
  jacobi3d-sdag \
  zerocopy \
  within_node_bcast \
  longIdle \
  periodic \
  bombard \
  varTRAM \
  dynamic_insertion \
  dynamic_insertion_deletion \
  amr_1d_random \
  amr_1d_simple \
  anytime_migration \
  unbounded_migration \
  unbounded_restart \
  anytime_bcastred \
  atsync_reduction \
  threaded_redn \
  objid_home \
  objid_insert \
  objid_sections \
  objid_bound \
  objid_tranche \
  objid_wide \
  objid_migrate6 \
  objid_loctable \
  objid_recreate \
  collide \
  qd \
  bcast_zcpup \

FTDIRS = \
  jacobi3d \
  jacobi3d-sdag \

# Not applicable on reconverse: longIdle needs CcdCallOnConditionOnPE, which
# reconverse does not provide.
ifeq ($(CMK_GDIR),reconverse)
RECONVERSE_NA = \
  longIdle \

endif

TESTDIRS = $(filter-out $(FTDIRS) $(RECONVERSE_NA),$(DIRS))

NONSCALEDIRS = \
  alignment \
  migratable_threads \
  sdag \
  sparse \
  charmxi_parsing \
  anytime_migration \

TESTPDIRS = $(filter-out $(NONSCALEDIRS),$(TESTDIRS))

all: $(foreach i,$(DIRS),build-$i)

test: $(foreach i,$(TESTDIRS),test-$i)
ifeq ($(CMK_SMP),1)
	make smptest
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

-include ../include/conv-mach-opt.mak

# Temporary hack that needs to be removed / replaced
OPTS += $(OPTSATBUILDTIME)

DIRS = \
  util \
  charm++ \
  ampi \
  campi \

FTDIRS = \
  charm++ \
  ampi \

TESTDIRS = $(DIRS)

all: $(foreach i,$(DIRS),build-$i)

test: $(foreach i,$(TESTDIRS),test-$i)

testp: $(foreach i,$(TESTDIRS),testp-$i)

syncfttest mpisyncfttest: $(foreach i,$(filter $(FTDIRS),$(TESTDIRS)),test-$i)

clean: $(foreach i,$(DIRS),clean-$i)
	rm -f TAGS #*#
	rm -f core *~

$(foreach i,$(DIRS),build-$i):
	$(MAKE) -C $(subst build-,,$@) all OPTS='$(OPTS)'

$(foreach i,$(DIRS),test-$i):
	$(MAKE) -C $(subst test-,,$@) test OPTS='$(OPTS)' TESTOPTS='$(TESTOPTS)'

$(foreach i,$(DIRS),testp-$i):
	$(MAKE) -C $(subst testp-,,$@) testp OPTS='$(OPTS)' TESTOPTS='$(TESTOPTS)' P='$(P)'

$(foreach i,$(DIRS),clean-$i):
	$(MAKE) -C $(subst clean-,,$@) clean OPTS='$(OPTS)'

test-AMPI: test-ampi
test-charm: test-charm++

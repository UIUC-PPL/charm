DIRS = \
  simple \

all: $(foreach i,$(DIRS),build-$i)

test: $(foreach i,$(DIRS),test-$i)

clean: $(foreach i,$(DIRS),clean-$i)

$(foreach i,$(DIRS),build-$i):
	$(MAKE) -C $(subst build-,,$@) OPTS='$(OPTS)'

$(foreach i,$(DIRS),test-$i):
	$(MAKE) -C $(subst test-,,$@) test OPTS='$(OPTS)' TESTOPTS='$(TESTOPTS)'

$(foreach i,$(DIRS),clean-$i):
	$(MAKE) -C $(subst clean-,,$@) clean OPTS='$(OPTS)'

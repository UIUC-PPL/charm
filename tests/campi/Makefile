-include ../../include/conv-mach-opt.mak

# campi programs are C++20 coroutines. libampi builds campi.C as a stub when
# the compiler lacks coroutine support (campi.C checks __cpp_impl_coroutine),
# so the same check decides here whether these tests are built and run.
CAMPI_COROUTINES := $(shell ../../bin/ampicxx -std=c++20 -E coroutine_probe.C 2>/dev/null | grep -c CAMPI_COROUTINES_OK; rm -f ampirun)

ifeq (1,$(CAMPI_COROUTINES))
DIRS = \
  pingpong \
  until \
  jacobi3d \

else
DIRS =
$(info tests/campi: the compiler has no C++20 coroutine support; skipping campi tests)
endif

TESTDIRS = $(DIRS)

all: $(foreach i,$(DIRS),build-$i)

test: $(foreach i,$(TESTDIRS),test-$i)

testp:

clean: $(foreach i,$(DIRS),clean-$i)
	rm -f TAGS #*#
	rm -f core *~ ampirun

$(foreach i,$(DIRS),build-$i):
	$(MAKE) -C $(subst build-,,$@) all OPTS='$(OPTS)'

$(foreach i,$(DIRS),test-$i):
	$(MAKE) -C $(subst test-,,$@) test OPTS='$(OPTS)' TESTOPTS='$(TESTOPTS)'

$(foreach i,$(DIRS),clean-$i):
	$(MAKE) -C $(subst clean-,,$@) clean OPTS='$(OPTS)'

.PHONY: all test testp clean

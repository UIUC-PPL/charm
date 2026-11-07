include config.mk
# Just point to the appropriate charm directory
CHARMHOME ?= $(HOME)/charm

# The relevant source files for this project
RAWSRC    = lu.C scheduler.C benchmark.C
INTF      = luUtils.ci lu.ci driver.ci

# Specify the exe name and the arguments to run it with
NP        = 4
TARGET    = charmlu
ARGS      = 64 16 500 8 2

# Specify the compilers, run script, flags etc.
OPT       = -O3
#CPPFLAGS += -DSCHED_PIVOT_REDN
#CPPFLAGS += -DCHARMLU_USEG_FROM_BELOW
CPPFLAGS += -DSEND_LIM=$(SEND_LIM) $(BLAS_INC)
CXXFLAGS += -language charm++ $(OPT)
LDFLAGS  += -module CkMulticast $(BLAS_LD)
LDLIBS   += $(BLAS_LIBS)

EXEC      = ./charmrun
EXECFLAGS = +p$(NP) ++local
TEST_SCRIPT=test_lu.pl
ifdef $(NODELIST)
  EXECFLAGS += ++nodelist $(NODELIST)
endif


########### This stuff should be able take care of itself ############

CHARMINC = $(CHARMHOME)/include
CHARMBIN = $(CHARMHOME)/bin
CHARMC   = $(CHARMBIN)/charmc

# The base directory of this project
base     ?= .
# The git binary
GIT       = $(shell which git)
# Compute the revision number (hash) of the build and feed it to the code
ifeq ($(GIT),)
  $(warning Cannot find the git binary. Will not compute the revision number)
else
  REVNUM  = $(shell $(GIT) --git-dir=$(base)/.git rev-parse HEAD)
endif
 
ifneq ($(REVNUM),)
  CPPFLAGS += -DLU_REVISION=$(REVNUM)
endif

.PHONY: all clean again test regtest

all: $(TARGET)

$(TARGET): CXX = $(CHARMC)
$(TARGET): $(RAWSRC:.C=.o)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

clean:
	$(RM) $(wildcard *.decl.h *.def.h *.d *.di *.o *.stamp) charmrun
	$(RM) $(wildcard *.log *.projrc *.sts) $(TARGET)

again: 
	$(MAKE) clean; $(MAKE)

test: all
	@echo "########################################################################################"
	$(EXEC) $(EXECFLAGS) $(TARGET) $(ARGS)

regtest: $(TARGET)
	perl $(TEST_SCRIPT) $(EXEC) $(EXECFLAGS) $(TARGET)

####### Pattern rules
# Rule to generate dependency information for C++ source files
%.d: %.C
	$(info Generating dependencies for $<)
	@g++ -MM -MG $(CPPFLAGS) $(INCDIRS:%=-I%) -I$(CHARMINC) $< | perl $(CHARMHOME)/src/scripts/dep.pl $(CHARMINC) > $@
#	@$(SHELL) -ec 'g++ -MM -MG $(CPPFLAGS) $(INCDIRS:%=-I%) $< \
#	| sed '\''s/\($*\)\.o[ :]*/\1.o $@ : /g'\'' > $@; \
#	[ -s $@ ] || rm -f $@'

# Rule to generate dependency info for charm++ interface (ci) definition files
%.di: %.ci
	$(info Generating dependencies for $<)
	@$(CHARMC) -M $< > $@

luUtils.ci.%.stamp: luUtils.ci
	@echo "Building $@ from $<"
	$(info-ci)
	$q$(CHARMC) $(CPPFLAGS) -E $< && touch $@

lu.ci.%.stamp: lu.ci
	@echo "Building $@ from $<"
	$(info-ci)
	$q$(CHARMC) $(CPPFLAGS) -E $< && touch $@

driver.ci.%.stamp: driver.ci
	@echo "Building $@ from $<"
	$(info-ci)
	$q$(CHARMC) $(CPPFLAGS) -E $< && touch $@

# Compilation rule for ci files
%.ci.stamp: %.ci
	$(info-ci)
	$q$(CHARMC) $(CPPFLAGS) -E $< && touch $@

# Include the generated files containing dependency info
ifneq ($(MAKECMDGOALS),clean)
ifneq ($(MAKECMDGOALS),realclean)
-include $(RAWSRC:.C=.d)
-include $(INTF:.ci=.di)
endif
endif


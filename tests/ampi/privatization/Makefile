-include ../../common.mk
-include ../../../include/conv-mach-opt.mak
AMPICC=../../../bin/ampicxx $(OPTS)
AMPIF90=../../../bin/ampif90 $(OPTS)


LANGUAGES := cxx
ifeq (1,$(CMK_CAN_LINK_FORTRAN))
  LANGUAGES += f90
endif


# for constructing individual build rules
VARIANTS := \
  control \
  swapglobals \
  tlsglobals \
  roseomptlsglobals \
  fsglobals \
  pipglobals \
  pieglobals \

PHONIES := \
  everything \

# for determining `all`, `test` rules
TARGETS :=

ifeq (1,$(CMK_SUPPORTS_SWAPGLOBALS))
  TARGETS += swapglobals
endif
ifeq (1,$(CMK_SUPPORTS_TLSGLOBALS))
  TARGETS += tlsglobals
endif
ifeq (1,$(CMK_SUPPORTS_FSGLOBALS))
  TARGETS += fsglobals
endif
ifeq (1,$(CMK_SUPPORTS_PIPGLOBALS))
  TARGETS += pipglobals
endif
ifeq (1,$(CMK_SUPPORTS_PIEGLOBALS))
  TARGETS += pieglobals
endif

$(foreach i,$(VARIANTS),$(eval LANGUAGES_$i := $(LANGUAGES)))
ifneq (1,$(CMK_HAS_OPENMP))
  LANGUAGES_tlsglobals := $(filter-out f90,$(LANGUAGES_tlsglobals))
endif


# Define what options to pass to charmc for each method.
$(foreach i,$(VARIANTS),$(foreach j,$(LANGUAGES),$(eval OPTS_$i_$j := -$i)))
OPTS_control_cxx :=
OPTS_control_f90 :=
OPTS_tlsglobals_f90 += -fopenmp

# Define the features that each privatization method supports.
FEATURES_control_cxx := globalvars threadlocalvars staticvars sharedlib dynamiclib migration
FEATURES_swapglobals_cxx := globalvars migration
FEATURES_tlsglobals_cxx := threadlocalvars staticvars sharedlib migration
FEATURES_roseomptlsglobals_cxx := threadlocalvars staticvars sharedlib migration
FEATURES_fsglobals_cxx := globalvars staticvars
FEATURES_pipglobals_cxx := globalvars staticvars sharedlib
FEATURES_pieglobals_cxx := globalvars staticvars sharedlib migration

ifeq (1,$(CMK_SUPPORTS_TLSGLOBALS))
  FEATURES_pieglobals_cxx += threadlocalvars
endif


# Universal settings
LINKOPTS := -balancer TreeLB


# Rule construction

EMPTY_MESSAGE := "No privatization methods supported for current AMPI target."

all: $(foreach i,$(TARGETS),$(foreach j,$(LANGUAGES_$i),$i-$j))
ifeq (,$(TARGETS))
	@echo $(EMPTY_MESSAGE)
endif

test: $(foreach i,$(TARGETS),$(foreach j,$(LANGUAGES_$i),test-$i-$j))
ifeq (,$(TARGETS))
	@echo $(EMPTY_MESSAGE)
endif

testp: $(foreach i,$(TARGETS),$(foreach j,$(LANGUAGES_$i),testp-$i-$j))
ifeq (,$(TARGETS))
	@echo $(EMPTY_MESSAGE)
endif

everything: $(foreach i,$(VARIANTS),$(foreach j,$(LANGUAGES),$i-$j))
test-everything: $(foreach i,$(VARIANTS),$(foreach j,$(LANGUAGES),test-$i-$j))

add_if_contains = $(subst $2,$3,$(findstring $2,$1))

define VARIANT_RULES

$1: $$(foreach j,$$(LANGUAGES_$1),$1-$$j)
test-$1: $$(foreach j,$$(LANGUAGES_$1),test-$1-$$j)

# link
$1-cxx: $1-framework.o $1-test-cxx.o $1-vars-static-cxx.o | $$(call add_if_contains,$$(FEATURES_$1_cxx),sharedlib,lib$1-vars-shared-cxx.so) $$(call add_if_contains,$$(FEATURES_$1_cxx),dynamiclib,lib$1-vars-dynamic-cxx.so)
	$$(AMPICC) -o $$@ $$(OPTS_$1_cxx) $$^ $$(LINKOPTS) -rpath-origin -L. $$(call add_if_contains,$$(FEATURES_$1_cxx),sharedlib,-l$1-vars-shared-cxx) $$(call add_if_contains,$$(FEATURES_$1_cxx),dynamiclib,-ldl)

$1-f90: $1-framework.o $1-test-f90.o
	$$(AMPIF90) -o $$@ $$(OPTS_$1_f90) $$^ $$(LINKOPTS)

lib$1-%-cxx.so: $1-%-cxx-so.o
	$$(AMPICC) -o $$@ $$(OPTS_$1_cxx) -shared -fvisibility=hidden -standalone $$^

$1-%.o: %.C
	$$(AMPICC) -o $$@ $$(OPTS_$1_cxx) -Dprivatization_method=$1 $$(foreach i,$$(FEATURES_$1_cxx),-Dtest_$$i) -c $$<

$1-%-cxx.o: %.C
	$$(AMPICC) -o $$@ $$(OPTS_$1_cxx) -Dprivatization_method=$1 $$(foreach i,$$(FEATURES_$1_cxx),-Dtest_$$i) -c $$<

$1-%-so.o: %.C
	$$(AMPICC) -o $$@ $$(OPTS_$1_cxx) -Dprivatization_method=$1 $$(foreach i,$$(FEATURES_$1_cxx),-Dtest_$$i) -shared -fvisibility=hidden -c $$<

$1-%-cxx-so.o: %.C
	$$(AMPICC) -o $$@ $$(OPTS_$1_cxx) -Dprivatization_method=$1 $$(foreach i,$$(FEATURES_$1_cxx),-Dtest_$$i) -shared -fvisibility=hidden -c $$<

$1-%.o: %.f90
	$$(AMPIF90) -o $$@ $$(OPTS_$1_f90) -c $$<

$1-%-f90.o: %.f90
	$$(AMPIF90) -o $$@ $$(OPTS_$1_f90) -c $$<

# test
test-$1-cxx: $1-cxx
	$$(call run, ./$1-cxx +p1 +vp2 +balancer RandCentLB)
	$$(call run, ./$1-cxx +p1 +vp4 +balancer RandCentLB)
	$$(call run, ./$1-cxx +p2 +vp2 +balancer RandCentLB)
	$$(call run, ./$1-cxx +p2 +vp4 +balancer RandCentLB)

test-$1-f90: $1-f90
	$$(call run, ./$1-f90 +p1 +vp2 +balancer RandCentLB)
	$$(call run, ./$1-f90 +p1 +vp4 +balancer RandCentLB)
	$$(call run, ./$1-f90 +p2 +vp2 +balancer RandCentLB)
	$$(call run, ./$1-f90 +p2 +vp4 +balancer RandCentLB)

testp-$1-cxx: $1-cxx
	$$(call run, ./$1-cxx +p$(P) +vp$(P) +balancer RandCentLB)
	$$(call run, ./$1-cxx +p$(P) +vp$$$$(( $(P) * 2 )) +balancer RandCentLB)
	$$(call run, ./$1-cxx +p$(P) +vp$$$$(( $(P) * 4 )) +balancer RandCentLB)

testp-$1-f90: $1-f90
	$$(call run, ./$1-f90 +p$(P) +vp$(P) +balancer RandCentLB)
	$$(call run, ./$1-f90 +p$(P) +vp$$$$(( $(P) * 2 )) +balancer RandCentLB)
	$$(call run, ./$1-f90 +p$(P) +vp$$$$(( $(P) * 4 )) +balancer RandCentLB)

endef

$(foreach i,$(VARIANTS),$(eval $(call VARIANT_RULES,$i)))

# overrides
tlsglobals-test-f90.o: test-tlsglobals.f90
	$(AMPIF90) -o $@ $(OPTS_tlsglobals_f90) -c $<

pieglobals-test-f90.o: test-pieglobals.f90
	$(AMPIF90) -o $@ $(OPTS_pieglobals_f90) -c $<

clean:
	rm -f *.o *.a *.so *.dylib *.mod $(foreach i,$(VARIANTS),$(foreach j,$(LANGUAGES),$(strip $i-$j $(wildcard $i-$j$(CMK_USER_SUFFIX)*)))) charmrun ampirun

.SUFFIXES:
.PHONY: \
  all \
  test \
  clean \
  $(LANGUAGES) \
  $(addprefix test-,$(LANGUAGES)) \
  $(foreach i,$(VARIANTS) $(PHONIES),$(foreach j,$(LANGUAGES),test-$i-$j) test-$i $i) \

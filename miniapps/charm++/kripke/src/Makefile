## Edit CHARMBASE to point to your Charm++ directory

################
## Generic Linux
CHARMBASE = $(HOME)/charm/netlrts-linux-x86_64
OPTS = -g -O0

##########################
## Vulcan: BlueGene/Q, XLC
#CHARMBASE = $(HOME)/charm/pami-bluegeneq-xlc
#OPTS = -qarch=auto -qsimd=auto -qhot=novector -qnostrict

#######################
## Cab: Infiniband, ICC
#CHARMBASE = $(HOME)/charm/verbs-linux-x86_64
#OPTS = -fno-omit-frame-pointer -unroll-aggressive -finline-functions -msse4.2

CHARMC = $(CHARMBASE)/bin/charmc $(OPTS)

C_CI := kripke.ci
C_SRCS := $(filter-out Input_Variables.cpp, $(wildcard *.cpp))
C_OBJS := $(C_SRCS:.cpp=.o)
DECL := $(C_CI:.ci=.decl.h)
DEF := $(C_CI:.ci=.def.h)
HEADERS := $(wildcard *.h)
DECL_DEF := $(DECL) $(DEF)

.PHONY: all projections loadbalance clean distclean

all: kripke


kripke: $(C_SRCS) $(DECL_DEF) $(HEADERS)
		$(CHARMC) -language charm++ $(C_SRCS) -o kripke

projections: $(C_SRCS) $(DECL_DEF) $(HEADERS)
		$(CHARMC) -tracemode projections -language charm++ $(C_SRCS) -o kripke

loadbalance: $(C_SRCS) $(DECL_DEF) $(HEADERS)
		$(CHARMC) -language charm++ -module CommonLBs $(C_SRCS) -o kripke

$(DECL_DEF): $(C_CI)
		$(CHARMC) $(C_CI)

clean:
		rm -f *.o *.decl.h *.def.h *.proj charmrun kripke

distclean: clean

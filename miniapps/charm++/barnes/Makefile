-include ../../common.mk
# CHARM_PATH defaults to the enclosing Charm++ build and STRUCTURES_PATH to
# a sibling clone of https://github.com/N-BodyShop/utility; override either
# on the command line, e.g. make STRUCTURES_PATH=/path/to/utility/structures
CHARM_PATH = ../../..
CHARM_LDB_PATH = $(CHARM_PATH)/src/ck-ldb
CHARM_UTIL_PATH = $(CHARM_PATH)/src/util
CHARM_LIB_PATH = $(CHARM_PATH)/lib
INCPATH = $(CHARM_PATH)/include
STRUCTURES_PATH = ../utility/structures

VPATH = $(STRUCTURES_PATH)

APP_FLAGS = -DSTATISTICS #-DVERBOSE_TRAVERSAL -DCHECK_NUM_INTERACTIONS -DPRINT_TREE -DDEBUG_TRAVERSALS #-DVERBOSE_NODE_REFINE
OPTS = -O3 -g $(APP_FLAGS)
CXXFLAGS += $(OPTS) -I$(INCPATH) -I$(STRUCTURES_PATH)
LDFLAGS += $(OPTS) -L. -language charm++ -module RotateLB -memory os #-tracemode projections

CHARMC = $(CHARM_PATH)/bin/charmc

CXX = $(CHARMC)
CC = $(CXX)
AR = ar q 
CXX_DEPEND = $(CXX) -M -MM -MG -Wall $(APP_FLAGS) 
CFLAGS = $(OPTS) $(DEFINE_FLAGS) -g 

OBJECTS = Main.o DataManager.o TreePiece.o util.o Reduction.o Worker.o Request.o State.o
SRC = Main.cpp DataManager.cpp TreePiece.cpp \
      util.cpp Reduction.cpp Worker.cpp Request.cpp \
      State.cpp particleGenerator.cpp gen_util.cpp plummer.cpp

TARGET = barnes 

# The structures headers (Vector3D.h, OrientedBox.h, ...) come from ChaNGa's
# utility repository. Without them, all and test print a message and succeed.
ifeq ($(wildcard $(STRUCTURES_PATH)/Vector3D.h),)
all test:
	@echo "barnes: skipped, ChaNGa structures library not found (STRUCTURES_PATH=$(STRUCTURES_PATH))"
else
all: $(TARGET) gen plummer

# 10,000 Plummer-sphere particles, 100 tree pieces, 50 iterations. No
# result check: the program prints the energy but checks nothing.
test: all
	./plummer 10000 particles.bin
	$(call run, +p4 ./barnes -in=particles.bin -p=100 -killat=50)

$(TARGET): $(OBJECTS) Makefile.dep 
	$(CHARMC) -o $(TARGET) $(LDFLAGS) $(OBJECTS)

gen: particleGenerator.cpp  
	g++ -o gen particleGenerator.cpp

plummer.o: plummer.cpp 
	g++ -I$(STRUCTURES_PATH) -I$(INCPATH) -c plummer.cpp 

gen_util.o: gen_util.cpp 
	g++ -I$(STRUCTURES_PATH) -c gen_util.cpp 

plummer: plummer.o gen_util.o 
	g++ -I$(STRUCTURES_PATH) -o plummer plummer.o gen_util.o

%.decl.h %.def.h : %.ci
	$(CHARMC) $(APP_FLAGS) -E $<

%.o: Makefile

depends:
	$(CXX_DEPEND) $(SRC) | while read i;do echo $$i| awk -F' ' '{for (i=1;i<NF;++i) print $$i" \\"}';echo;done|grep -v "$(CHARM_PATH)/bin" > Makefile.dep 

.PHONY: all test docs depends 

include Makefile.dep
endif

clean:
	rm -f core* $(OBJECTS) *~ $(TARGET) *.decl.h *.def.h charmrun conv-host gen plummer plummer.o gen_util.o particles.bin

CHARM_PATH = ../charm
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
all: $(TARGET) gen plummer

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

clean:
	rm -f core* $(OBJECTS) *~ $(TARGET) *.decl.h *.def.h charmrun conv-host 

depends:
	$(CXX_DEPEND) $(SRC) | while read i;do echo $$i| awk -F' ' '{for (i=1;i<NF;++i) print $$i" \\"}';echo;done|grep -v "$(CHARM_PATH)/bin" > Makefile.dep 

.PHONY: all docs depends 

include Makefile.dep

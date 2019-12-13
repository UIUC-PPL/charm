CHARM_DIR = ../../../..
CHARMC = $(CHARM_DIR)/bin/charmc $(OPTS)

NVCC = nvcc
NVCC_FLAGS = -c -std=c++11
CHARM_INC = -I$(CHARM_DIR)/include
LD_LIBS = #-lnvToolsExt

TARGET = vecadd
all: $(TARGET)

OBJS = $(TARGET).o $(TARGET)CUDA.o

$(TARGET): $(OBJS)
	$(CHARMC) -language charm++ -o $@ $(OBJS) $(LD_LIBS)

$(TARGET).decl.h: $(TARGET).ci
	$(CHARMC) $<

$(TARGET).o: $(TARGET).C $(TARGET).decl.h
	$(CHARMC) -c $<

$(TARGET)CUDA.o: $(TARGET).cu
	$(NVCC) -o $@ $(NVCC_FLAGS) $(CHARM_INC) $<

clean:
	rm -f *.decl.h *.def.h conv-host *.o $(TARGET) charmrun

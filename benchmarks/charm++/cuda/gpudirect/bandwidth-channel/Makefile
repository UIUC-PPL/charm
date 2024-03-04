-include ../../../../common.mk

OPTS = -O3

CHARM_DIR = ../../../../..
CHARMC = $(CHARM_DIR)/bin/charmc $(OPTS)
CHARM_INC = -I$(CHARM_DIR)/include
LD_LIBS =

TARGET = bandwidth
all: $(TARGET)

OBJS = $(TARGET).o

$(TARGET): $(OBJS)
	$(CHARMC) -language charm++ -o $@ $(OBJS) $(LD_LIBS)

$(TARGET).decl.h: $(TARGET).ci
	$(CHARMC) $<

$(TARGET).def.h: $(TARGET).ci

$(TARGET).o: $(TARGET).C $(TARGET).decl.h $(TARGET).def.h
	$(CHARMC) -c $<

test: all
	$(call run, +p2 ./bandwidth +gpumap block +gpushm)

clean:
	rm -f *.decl.h *.def.h conv-host *.o $(TARGET) charmrun

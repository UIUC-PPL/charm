-include ../../common.mk
-include ../../../include/conv-mach-opt.mak
CHARMC=../../../bin/charmc $(OPTS)

# Two partitions need at least two processes. On reconverse +p<n> through
# testrun starts one process with n PEs, so ask for two.
ifeq ($(CMK_GDIR),reconverse)
RUN_PROCS = TESTRUN_PROCS=2
endif

OBJS = hello.o

all: hello

hello: $(OBJS)
	$(CHARMC) -language charm++ -o hello $(OBJS)

hello.decl.h: hello.ci
	$(CHARMC)  hello.ci

clean:
	rm -f *.decl.h *.def.h conv-host *.o hello charmrun *.log *.sum *.sts hello.exe hello.pdb hello.ilk

hello.o: hello.C hello.decl.h
	$(CHARMC) -c hello.C

test: all
ifeq ($(CMK_NO_PARTITIONS),1)
	@echo "Skipping test since build does not support partitions"
else
	$(RUN_PROCS) $(call run, ./hello +p4 10 2 +partitions 2)
endif

testp: all
ifeq ($(CMK_NO_PARTITIONS),1)
	@echo "Skipping test since build does not support partitions"
else ifneq (,$(filter %0 %2 %4 %6 %8,$(P))) # if P is even
	@echo "P is even, running partitions test with $(P)/2 partitions"
	$(call run, ./hello +p$(P) $$(( $(P) * 4 )) 2 +partitions $$(( $(P) / 2 )))
else
	@echo "P is odd, running partitions test with 1 partition"
	$(call run, ./hello +p$(P) $$(( $(P) * 4 )) $(P) +partitions 1)
endif

smptest: all
ifeq ($(CMK_NO_PARTITIONS),1)
	@echo "Skipping test since build does not support partitions"
else
	$(RUN_PROCS) $(call run, ./hello +p4 10 2 +partitions 2 ++ppn 2)
endif

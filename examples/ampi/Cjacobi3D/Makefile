-include ../../common.mk
-include ../../../include/conv-mach-opt.mak
CHARMBASE=../../../
CHARMC=../../../bin/ampicxx $(OPTS)
TOKENS=6

-include $(CHARMBASE)/include/conv-mach-opt.mak

all: ampi

AMPI_TARGETS := \
  jacobi \
  jacobi.pup \
  jacobi-get \

ifeq (1,$(CMK_SUPPORTS_TLSGLOBALS))
  AMPI_TARGETS += jacobi.tls
endif

ifeq (1,$(CMK_SUPPORTS_MEMORY_ISOMALLOC))
  AMPI_TARGETS += jacobi.iso
endif

ampi: $(AMPI_TARGETS)

mpi: jacobi.C
	mpiCC -o jacobi jacobi.C  $(OPTS)

jacobi-cpp: jacobi-cpp.C
	$(CHARMC) -c jacobi-cpp.C 
	$(CHARMC) -o $@ jacobi-cpp.o -lm

jacobi.o: jacobi.C
	$(CHARMC) -c jacobi.C 

jacobi: jacobi.o
	$(CHARMC) -o $@ $^ -lm

jacobi.pup: jacobi.o
	$(CHARMC) -o $@ $^ -memory default -lm

jacobi.iso: jacobi.C
	$(CHARMC) -c -DNO_PUP jacobi.C -o jacobi.iso.o
	$(CHARMC) -o $@ jacobi.iso.o -memory isomalloc

jacobi.tls: jacobi.C
	$(CHARMC) -c -tlsglobals jacobi.C -o jacobi.tls.o
	$(CHARMC) -o jacobi.tls jacobi.tls.o -tlsglobals

jacobi.rose: jacobi.C
	$(CHARMC) -roseomptlsglobals -o jacobi.rose.o -c $<
	$(CHARMC) -roseomptlsglobals -o $@ jacobi.rose.o

# jacobi-get creates an MPI_Win over the chunk, and a window's memory has to
# keep its address when the rank migrates or restarts. The Isomalloc heap
# moves the chunk at the same address; chunk_pup, which frees the chunk and
# allocates a new one, would not. So the pup routine is left out (-DNO_PUP).
jacobi-get: jacobi-get.C
	$(CHARMC) -c -DNO_PUP jacobi-get.C
	$(CHARMC) -o $@ jacobi-get.o -lm

jacobi-get-tokens:
	cd $(CHARMBASE)/tmp; touch machine.c; touch ckfutures.C; make AMPI OPTS="-DIGET_FLOWCONTROL=1 -DIGET_TOKENNUM=$(TOKENS)"; cd -; 
	make jacobi-get;

# A restart from a checkpoint resumes each rank's thread in a new process,
# which must load the program and its libraries at the addresses the thread's
# stack holds, so address space layout randomization must be off. On Linux
# testrun runs every program under setarch -R, unless TESTRUN_VA_RANDOMIZATION=1
# asks it not to; macOS has no way to turn it off, and Isomalloc refuses the
# restart. The same condition as in tests/ampi/Makefile.
ifeq ($(CMK_GDIR),reconverse)
ifeq ($(CMK_MACOSX),1)
RECONVERSE_VA_RANDOMIZED = 1
endif
ifeq ($(TESTRUN_VA_RANDOMIZATION),1)
RECONVERSE_VA_RANDOMIZED = 1
endif
endif

# The final line of a restarted run must be the final line of the
# uninterrupted run that wrote the checkpoint.
define check_restart
	grep "^jacobi: " $(1) && test "$$(grep '^jacobi: ' $(1))" = "$$(grep '^jacobi: ' $(2))"
endef

test: ampi
	$(call run, +p3 ./jacobi.pup 2 2 2 40 +vp8 +balancer RotateLB +LBDebug 1 )
	$(call run, +p3 ./jacobi 2 2 2 40 +vp8 +balancer RotateLB +LBDebug 1 )
	$(call run, +p3 ./jacobi-get 2 2 2 40 +vp8 +balancer RotateLB +LBDebug 1 )
ifeq (1,$(CMK_SUPPORTS_TLSGLOBALS))
	$(call run, +p3 ./jacobi.tls 2 2 2 40 +vp8 +balancer RotateLB +LBDebug 1)
endif
ifeq (1,$(CMK_SUPPORTS_MEMORY_ISOMALLOC))
	$(call run, +p3 ./jacobi.iso 2 2 2 40 +vp8 +balancer RotateLB +LBDebug 1)
endif
	# Checkpoint after iteration 20 of 40 to log/ (the load balancer keeps
	# moving ranks at iterations 5, 15, 25, 35), then restart from it on the
	# same, fewer and more PEs; with TESTRUN_PROCS=2 the runs use two
	# processes, so the PE counts are even (testrun skips a PE count the
	# process count does not divide). The Isomalloc build (jacobi) restores
	# the stack and heap, the -memory default build (jacobi.pup) restores the
	# chunk with chunk_pup.
	rm -rf log log.pup
	$(call run, +p4 ./jacobi 2 2 2 40 -chkpt 20 log +vp8 +balancer RotateLB ) 2>&1 | tee chkpt.log
	grep "^jacobi: 40 iterations" chkpt.log
	$(call run, +p4 ./jacobi.pup 2 2 2 40 -chkpt 20 log.pup +vp8 +balancer RotateLB ) 2>&1 | tee chkpt.pup.log
	grep "^jacobi: 40 iterations" chkpt.pup.log
ifneq ($(RECONVERSE_VA_RANDOMIZED),1)
	-sync
	$(call run, +p4 ./jacobi 2 2 2 40 -chkpt 20 log +vp8 +balancer RotateLB +restart log ) 2>&1 | tee restart4.log
	$(call check_restart,restart4.log,chkpt.log)
	$(call run, +p2 ./jacobi 2 2 2 40 -chkpt 20 log +vp8 +balancer RotateLB +restart log ) 2>&1 | tee restart2.log
	$(call check_restart,restart2.log,chkpt.log)
	$(call run, +p6 ./jacobi 2 2 2 40 -chkpt 20 log +vp8 +balancer RotateLB +restart log ) 2>&1 | tee restart6.log
	$(call check_restart,restart6.log,chkpt.log)
	$(call run, +p6 ./jacobi.pup 2 2 2 40 -chkpt 20 log.pup +vp8 +balancer RotateLB +restart log.pup ) 2>&1 | tee restart6.pup.log
	$(call check_restart,restart6.pup.log,chkpt.pup.log)
endif

testp: ampi
	$(call run, +p$(P) ./jacobi.pup $(P) 1 1 40 +vp$(P) +balancer RotateLB +LBDebug 1 )
	$(call run, +p$(P) ./jacobi.pup $(P) 2 1 40 +vp$$(( $(P) * 2 )) +balancer RotateLB +LBDebug 1 )
	$(call run, +p$(P) ./jacobi.pup $(P) 2 2 40 +vp$$(( $(P) * 4 )) +balancer RotateLB +LBDebug 1 )

	$(call run, +p$(P) ./jacobi $(P) 1 1 40 +vp$(P) +balancer RotateLB +LBDebug 1 )
	$(call run, +p$(P) ./jacobi $(P) 2 1 40 +vp$$(( $(P) * 2 )) +balancer RotateLB +LBDebug 1 )
	$(call run, +p$(P) ./jacobi $(P) 2 2 40 +vp$$(( $(P) * 4 )) +balancer RotateLB +LBDebug 1 )

ifeq (1,$(CMK_SUPPORTS_TLSGLOBALS))
	$(call run, +p$(P) ./jacobi.tls $(P) 1 1 40 +vp$(P) +balancer RotateLB +LBDebug 1 )
	$(call run, +p$(P) ./jacobi.tls $(P) 2 1 40 +vp$$(( $(P) * 2 )) +balancer RotateLB +LBDebug 1 )
	$(call run, +p$(P) ./jacobi.tls $(P) 2 2 40 +vp$$(( $(P) * 4 )) +balancer RotateLB +LBDebug 1 )
endif

ifeq (1,$(CMK_SUPPORTS_MEMORY_ISOMALLOC))
	$(call run, +p$(P) ./jacobi.iso $(P) 1 1 40 +vp$(P) +balancer RotateLB +LBDebug 1)
	$(call run, +p$(P) ./jacobi.iso $(P) 2 1 40 +vp$$(( $(P) * 2 )) +balancer RotateLB +LBDebug 1 )
	$(call run, +p$(P) ./jacobi.iso $(P) 2 2 40 +vp$$(( $(P) * 4 )) +balancer RotateLB +LBDebug 1 )
endif



clean:
	rm -f *.o jacobi jacobi.pup *~ moduleinit.C charmrun conv-host jacobi-cpp jacobi.iso jacobi-get jacobi.tls ampirun
	rm -f chkpt.log chkpt.pup.log restart*.log
	rm -rf 40 80 120 log log.pup

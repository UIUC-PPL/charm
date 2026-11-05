-include ../../common.mk
-include ../../../include/conv-mach-opt.mak
CHARMC=../../../bin/ampicc

all: hello

ampi: hello

hello: hello.c
	$(CHARMC) -o hello hello.c $(OPTS)

# Checkpoint to the directory log/, then restart from it on the same, fewer
# and more PEs. With TESTRUN_PROCS > 1 the runs use that many processes, and
# the last checkpoint (written by that many processes) is also restarted in
# one process. A multi-process restart with a different process count is
# what exercises the Isomalloc region recorded in the checkpoint
# (log/Isomalloc.dat): every process must reserve the same address range
# that the checkpointed threads' stacks and heaps used.
test: hello
	rm -rf log
	$(call run, +p4 ./hello +vp8 )
	-sync
	$(call run, +p4 ./hello +vp8 +restart log )
	$(call run, +p2 ./hello +vp8 +restart log )
	$(call run, +p8 ./hello +vp8 +restart log )
	-sync
	# The restarts above most likely compute the same region on their own, so
	# they do not show that log/Isomalloc.dat was read. Lower the recorded start
	# by 32 MB (the checkpointed contexts are still inside the range), restart,
	# and require the line that says the recorded range was taken.
	start=$$(awk '/^start/ { print $$2 }' log/Isomalloc.dat) && \
	  new=$$(printf '0x%x' $$(( start - 32 * 1024 * 1024 ))) && \
	  sed -i.orig "s/^start .*/start $$new/" log/Isomalloc.dat && cat log/Isomalloc.dat
	$(call run, +p4 ./hello +vp8 +restart log ) 2>&1 | tee restart-adopted.log
	grep -q "Isomalloc> Restart: using the checkpoint's address range" restart-adopted.log
	grep -q "All tests passed" restart-adopted.log
	-sync
	rm -rf log
	$(call run, +p2 ./hello +vp8 )
	-sync
	$(call run, +p4 ./hello +vp8 +restart log )
ifneq ($(filter-out 0 1,$(TESTRUN_PROCS)),)
	TESTRUN_PROCS=1 $(call run, +p4 ./hello +vp8 +restart log )
endif
	# A checkpoint written after a load balancing step: RotateLB at step 1
	# moves every rank to another PE, so rank 0 is not on PE 0 when the
	# checkpoint is taken at step 2. The restart runs with and without the
	# balancer; the restart under RotateLB migrates the restored ranks again
	# at the second load balancing step (step 4).
	-sync
	rm -rf log
	$(call run, +p4 ./hello +vp8 +balancer RotateLB )
	-sync
	$(call run, +p2 ./hello +vp8 +balancer RotateLB +restart log )
	$(call run, +p4 ./hello +vp8 +restart log )

clean:
	rm -f *.o *.mod moduleinit.C hello *~ conv-host charmrun ampirun restart-adopted.log
	rm -rf log

-include ../../common.mk
CHARMC=../../../bin/ampicxx $(OPTS)

all: jacobi

jacobi: jacobi.o
	$(CHARMC) -o jacobi jacobi.o -lm

jacobi.o: jacobi.C
	$(CHARMC) -std=c++20 -c jacobi.C

clean:
	rm -f *.o jacobi charmrun conv-host ampirun
	rm -f lb.log fake.log chkpt.log restart*.log
	rm -rf log

# The final line of a restarted run must be the final line of the
# uninterrupted run that wrote the checkpoint.
define check_restart
	grep "^jacobi: " $(1) && test "$$(grep '^jacobi: ' $(1))" = "$$(grep '^jacobi: ' $(2))"
endef

# Even PE counts throughout, so that no run is skipped with TESTRUN_PROCS=2.
# A campi rank migrates and restarts from its state object, not from its
# stack and heap, so the restart runs need address space layout
# randomization neither off nor on: they run on macOS too.
test: jacobi
	$(call run, ./jacobi 2 2 2 20 +p1 +vp8)
	$(call run, ./jacobi 2 2 2 20 +p2 +vp8)
	$(call run, +p4 ./jacobi 2 2 2 40 +vp8 +balancer RotateLB ) 2>&1 | tee lb.log
	grep "^jacobi: 40 iterations" lb.log
	$(call run, +p2 ./jacobi 2 2 2 40 +vp8 +campi_fakemigrate ) 2>&1 | tee fake.log
	grep "^jacobi: 40 iterations" fake.log
	# Checkpoint after iteration 20 of 40 to log/ (the balancer moves ranks at
	# iterations 5, 15, 25, 35), then restart from it on the same, fewer and
	# more PEs.
	rm -rf log
	$(call run, +p4 ./jacobi 2 2 2 40 -chkpt 20 log +vp8 +balancer RotateLB ) 2>&1 | tee chkpt.log
	grep "^jacobi: 40 iterations" chkpt.log
	-sync
	$(call run, +p4 ./jacobi 2 2 2 40 -chkpt 20 log +vp8 +balancer RotateLB +restart log ) 2>&1 | tee restart4.log
	$(call check_restart,restart4.log,chkpt.log)
	$(call run, +p2 ./jacobi 2 2 2 40 -chkpt 20 log +vp8 +balancer RotateLB +restart log ) 2>&1 | tee restart2.log
	$(call check_restart,restart2.log,chkpt.log)
	$(call run, +p6 ./jacobi 2 2 2 40 -chkpt 20 log +vp8 +balancer RotateLB +restart log ) 2>&1 | tee restart6.log
	$(call check_restart,restart6.log,chkpt.log)

-include ../../common.mk
CHARMC=../../../bin/ampicxx $(OPTS) $(MOPTS)

# Other tests that could be here:
# - Fortran EXIT()
# - C exit(), return
# - Other main() prototypes

all: exit_explicit exit_return exit_after_finalize exit_abort

exit_explicit: exit_explicit.C
	$(CHARMC) -o $@ $<

exit_return: exit_return.C
	$(CHARMC) -o $@ $<

exit_after_finalize: exit_after_finalize.C
	$(CHARMC) -o $@ $<

exit_abort: exit_abort.C
	$(CHARMC) -o $@ $<

# Check that exit_explicit and exit_return exit with exit code 42, and that
# the value main returns after MPI_Finalize becomes the exit code: 3 when
# every rank returns 3, nonzero when rank 0 returns 0 and the others 3, and
# 0 when every rank returns 0. exit_abort checks that MPI_Abort's error code
# becomes the exit status, whether the aborting rank is in the first process
# (rank 0) or, under TESTRUN_PROCS=2, the second (rank 3), and that code 0
# gives status 0.
test: all
	$(call run, ./exit_explicit 2>&1); [ $$? -eq 42 ] && echo "exit_explicit test passed." || (echo "exit_explicit test failed (exit code was $$?, but expected 42)."; false)
	$(call run, ./exit_return 2>&1); [ $$? -eq 42 ] && echo "exit_return test passed." || (echo "exit_return test failed (exit code was $$?, but expected 42)."; false)
	$(call run, ./exit_after_finalize +p2 +vp4 3 2>&1); rc=$$?; [ $$rc -eq 3 ] && echo "exit_after_finalize 3 test passed." || (echo "exit_after_finalize 3 test failed (exit code was $$rc, but expected 3)."; false)
	$(call run, ./exit_after_finalize +p2 +vp4 3 mixed 2>&1); rc=$$?; [ $$rc -ne 0 ] && echo "exit_after_finalize 3 mixed test passed." || (echo "exit_after_finalize 3 mixed test failed (exit code was 0, but expected nonzero)."; false)
	$(call run, ./exit_after_finalize +p2 +vp4 0 2>&1); rc=$$?; [ $$rc -eq 0 ] && echo "exit_after_finalize 0 test passed." || (echo "exit_after_finalize 0 test failed (exit code was $$rc, but expected 0)."; false)
	$(call run, ./exit_abort +p2 +vp4 0 7 2>&1); rc=$$?; [ $$rc -eq 7 ] && echo "exit_abort 0 7 test passed." || (echo "exit_abort 0 7 test failed (exit code was $$rc, but expected 7)."; false)
	$(call run, ./exit_abort +p2 +vp4 3 7 2>&1); rc=$$?; [ $$rc -eq 7 ] && echo "exit_abort 3 7 test passed." || (echo "exit_abort 3 7 test failed (exit code was $$rc, but expected 7)."; false)
	$(call run, ./exit_abort +p2 +vp4 2 0 2>&1); rc=$$?; [ $$rc -eq 0 ] && echo "exit_abort 2 0 test passed." || (echo "exit_abort 2 0 test failed (exit code was $$rc, but expected 0)."; false)

clean:
	rm -f *.o exit_explicit exit_return exit_after_finalize exit_abort charmrun ampirun

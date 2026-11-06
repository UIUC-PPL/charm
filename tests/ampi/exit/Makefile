-include ../../common.mk
CHARMC=../../../bin/ampicxx $(OPTS) $(MOPTS)

# Other tests that could be here:
# - Fortran EXIT()
# - C exit(), return
# - Other main() prototypes

all: exit_explicit exit_return exit_after_finalize

exit_explicit: exit_explicit.C
	$(CHARMC) -o $@ $<

exit_return: exit_return.C
	$(CHARMC) -o $@ $<

exit_after_finalize: exit_after_finalize.C
	$(CHARMC) -o $@ $<

# Check that exit_explicit and exit_return exit with exit code 42, and that
# the value main returns after MPI_Finalize becomes the exit code: 3 when
# every rank returns 3, nonzero when rank 0 returns 0 and the others 3, and
# 0 when every rank returns 0.
test: all
	$(call run, ./exit_explicit 2>&1); [ $$? -eq 42 ] && echo "exit_explicit test passed." || (echo "exit_explicit test failed (exit code was $$?, but expected 42)."; false)
	$(call run, ./exit_return 2>&1); [ $$? -eq 42 ] && echo "exit_return test passed." || (echo "exit_return test failed (exit code was $$?, but expected 42)."; false)
	$(call run, ./exit_after_finalize +p2 +vp4 3 2>&1); rc=$$?; [ $$rc -eq 3 ] && echo "exit_after_finalize 3 test passed." || (echo "exit_after_finalize 3 test failed (exit code was $$rc, but expected 3)."; false)
	$(call run, ./exit_after_finalize +p2 +vp4 3 mixed 2>&1); rc=$$?; [ $$rc -ne 0 ] && echo "exit_after_finalize 3 mixed test passed." || (echo "exit_after_finalize 3 mixed test failed (exit code was 0, but expected nonzero)."; false)
	$(call run, ./exit_after_finalize +p2 +vp4 0 2>&1); rc=$$?; [ $$rc -eq 0 ] && echo "exit_after_finalize 0 test passed." || (echo "exit_after_finalize 0 test failed (exit code was $$rc, but expected 0)."; false)

clean:
	rm -f *.o exit_explicit exit_return exit_after_finalize charmrun ampirun

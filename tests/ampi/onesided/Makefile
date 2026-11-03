-include ../../common.mk
CHARMC=../../../bin/ampicxx $(OPTS)

all: onesided

onesided: test.o
	$(CHARMC) -o onesided test.o

test.o: test.C
	$(CHARMC) -c test.C

# The abort runs: rank 0 accesses one element past the end of a disp_unit 4
# window with each RMA call; AMPI must abort naming the window bounds.
ABORT_MSG = "outside the MPI_Win"
test: onesided
	$(call run, ./onesided +p1 +vp1 )
	$(call run, ./onesided +p1 +vp2 )
	$(call run, ./onesided +p1 +vp4 )
	$(call run, ./onesided +p2 +vp2 )
	$(call run, ./onesided +p2 +vp4 )
	$(call run, ./onesided +p1 +vp2 abort put ) 2>&1 | grep -q $(ABORT_MSG)
	$(call run, ./onesided +p1 +vp2 abort get ) 2>&1 | grep -q $(ABORT_MSG)
	$(call run, ./onesided +p1 +vp2 abort acc ) 2>&1 | grep -q $(ABORT_MSG)
	$(call run, ./onesided +p1 +vp2 abort fop ) 2>&1 | grep -q $(ABORT_MSG)
	$(call run, ./onesided +p1 +vp2 abort getacc ) 2>&1 | grep -q $(ABORT_MSG)
	$(call run, ./onesided +p1 +vp2 abort cas ) 2>&1 | grep -q $(ABORT_MSG)
	$(call run, ./onesided +p2 +vp2 abort put ) 2>&1 | grep -q $(ABORT_MSG)

testp: onesided
	$(call run, ./onesided +p$(P) +vp$(P) )
	$(call run, ./onesided +p$(P) +vp$$(( $(P) * 2 )) )

clean:
	rm -f *.o onesided ampirun charmrun conv-host *~

-include ../../common.mk
CHARMC=../../../bin/ampicxx $(OPTS)

all: winlock

winlock: test.o
	$(CHARMC) -o winlock test.o

test.o: test.C
	$(CHARMC) -c test.C

# The badunlock run: rank 0 unlocks a window it has not locked; AMPI must
# fail with MPI_ERR_RMA_SYNC.
BADUNLOCK_MSG = "MPI_ERR_RMA_SYNC"
test: winlock
	$(call run, ./winlock +p1 +vp1 )
	$(call run, ./winlock +p1 +vp4 )
	$(call run, ./winlock +p2 +vp4 )
	$(call run, ./winlock +p4 +vp8 )
	$(call run, ./winlock +p1 +vp2 badunlock ) 2>&1 | grep -q $(BADUNLOCK_MSG)

testp: winlock
	$(call run, ./winlock +p$(P) +vp$(P) )
	$(call run, ./winlock +p$(P) +vp$$(( $(P) * 2 )) )

clean:
	rm -f *.o winlock ampirun charmrun conv-host *~

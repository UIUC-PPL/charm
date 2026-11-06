-include ../../common.mk
CHARMC=../../../bin/ampicxx $(OPTS)

all: reduce_ops

reduce_ops: reduce_ops.o
	$(CHARMC) -o reduce_ops reduce_ops.o

reduce_ops.o: reduce_ops.C
	$(CHARMC) -c reduce_ops.C

test: reduce_ops
	$(call run, ./reduce_ops +p1 +vp1)
	$(call run, ./reduce_ops +p1 +vp4)
	$(call run, ./reduce_ops +p2 +vp7)
	$(call run, ./reduce_ops +p4 +vp8)

testp: reduce_ops
	$(call run, ./reduce_ops +p$(P) +vp$(P) )
	$(call run, ./reduce_ops +p$(P) +vp$$(( $(P) * 2 )) )

clean:
	rm -f *.o *.mod reduce_ops *~ conv-host charmrun ampirun reduce_ops.exe reduce_ops.pdb reduce_ops.ilk

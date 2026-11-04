-include ../../common.mk
CHARMC=../../../bin/ampicxx $(OPTS)

all: intercomm_coll

intercomm_coll: intercomm_coll.o
	$(CHARMC) -o intercomm_coll intercomm_coll.o

intercomm_coll.o: intercomm_coll.C
	$(CHARMC) -c intercomm_coll.C

test: intercomm_coll
	$(call run, ./intercomm_coll +p1 +vp4)
	$(call run, ./intercomm_coll +p2 +vp4)
#
testp: intercomm_coll
	$(call run, ./intercomm_coll +p$(P) +vp$(P) )
	$(call run, ./intercomm_coll +p$(P) +vp$$(( $(P) * 2 )) )
	$(call run, ./intercomm_coll +p$(P) +vp$$(( $(P) * 4 )) )

clean:
	rm -f *.o *.mod intercomm_coll *~ conv-host charmrun intercomm_coll.o charmrun.exe intercomm_coll.exe intercomm_coll.pdb intercomm_coll.ilk ampirun

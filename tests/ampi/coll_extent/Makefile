-include ../../common.mk
CHARMC=../../../bin/ampicxx $(OPTS)

all: coll_extent

coll_extent: test.o
	$(CHARMC) -o coll_extent test.o

test.o: test.C
	$(CHARMC) -c test.C

test: coll_extent
	$(call run, ./coll_extent +p1 +vp2 )
	$(call run, ./coll_extent +p1 +vp5 )
	$(call run, ./coll_extent +p2 +vp4 )
	$(call run, ./coll_extent +p2 +vp7 )

testp: coll_extent
	$(call run, ./coll_extent +p$(P) +vp$$(( $(P) * 2 )) )

clean:
	rm -f *.o coll_extent ampirun charmrun conv-host *~

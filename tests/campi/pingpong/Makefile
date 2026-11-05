-include ../../common.mk
CHARMC=../../../bin/ampicxx $(OPTS)

all: pingpong

pingpong: pingpong.o
	$(CHARMC) -o pingpong pingpong.o

pingpong.o: pingpong.C
	$(CHARMC) -std=c++20 -c pingpong.C

clean:
	rm -f *.o pingpong charmrun conv-host ampirun

test: pingpong
	$(call run, ./pingpong +p1 +vp2)
	$(call run, ./pingpong +p1 +vp4)
	$(call run, ./pingpong +p2 +vp4)
	# mapfile puts rank 0 on PE 1, so MPI_Init creates the arrays off PE 0
	$(call run, ./pingpong +p2 +vp4 +mapping MAPFILE)

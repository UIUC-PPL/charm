-include ../../common.mk
CHARMC=../../../bin/ampicxx $(OPTS)

all: jacobi

jacobi: jacobi.o
	$(CHARMC) -o jacobi jacobi.o -lm

jacobi.o: jacobi.C
	$(CHARMC) -std=c++20 -c jacobi.C

clean:
	rm -f *.o jacobi charmrun conv-host ampirun

test: jacobi
	$(call run, ./jacobi 2 2 2 20 +p1 +vp8)
	$(call run, ./jacobi 2 2 2 20 +p2 +vp8)

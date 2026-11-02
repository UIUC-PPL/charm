-include ../../common.mk
CHARMC=../../../bin/ampicxx $(OPTS)

all: until

until: until.o
	$(CHARMC) -o until until.o

until.o: until.C
	$(CHARMC) -std=c++20 -c until.C

clean:
	rm -f *.o until charmrun conv-host ampirun

test: until
	$(call run, ./until +p1 +vp4)
	$(call run, ./until +p2 +vp4)
	$(call run, ./until +p2 +vp5)

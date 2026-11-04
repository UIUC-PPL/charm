-include ../../../common.mk
CHARMC=../../../../bin/charmc $(OPTS)

all: hello

hello:   hello.o
	$(CHARMC) hello.o -o hello -module CkMulticast -language charm++ 

hello.o : hello.C hello.def.h hello.decl.h 
	$(CHARMC) -c hello.C

hello.decl.h hello.def.h : hello.ci.stamp

hello.ci.stamp: hello.ci
	$(CHARMC) $<
	touch $@

test: all
	$(call run, +p3 ./hello 10 )
	$(call run, +p3 ./hello 10 +randomized_msgq +randomized_seed 1 )
	$(call run, +p4 ./hello 10 +randomized_msgq +randomized_seed 1 )
	$(call run, +p4 ./hello 10 +randomized_msgq +randomized_seed 2 )
	$(call run, +p4 ./hello 10 +randomized_msgq +randomized_seed 3 )

testp: all
	$(call run, +p$(P) ./hello $$(( $(P) * 8 )) )

smptest: all
	$(call run, +p2 ./hello 10 ++ppn 2 )
	$(call run, +p4 ./hello 10 ++ppn 2 )

clean:
	rm -f conv-host *.o charmrun charmrun.exe
	rm -f *.def.h *.decl.h *.ci.stamp
	rm -f hello hello.*.log hello.sts hello.exe hello.pdb hello.ilk
	rm -f gmon.out #*#
	rm -f core *~
	rm -f TAGS *.headers hello


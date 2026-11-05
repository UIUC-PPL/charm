-include ../../common.mk
CHARMC=../../../bin/charmc $(OPTS)

all: multirun multirun_time

#Charm++ libraries to be used for Interoperation
LIBS=libmodulehi.a libmodulehello.a libmodulekNeighbor.a

multirun: multirun.cpp $(LIBS)
	$(CHARMC) -c++ mpicxx -c multirun.cpp -o multirun.o -I../../../include
	$(CHARMC) -ld++ mpicxx -mpi -o multirun multirun.o -L./ -module hello -module kNeighbor -module CommonLBs

multirun_time: multirun_time.cpp $(LIBS)
	$(CHARMC) -c++ mpicxx -c multirun_time.cpp -o multirun_time.o
	$(CHARMC) -ld++ mpicxx -mpi -nomain-module -o multirun_time multirun_time.o -L./ -module hello -module hi -module kNeighbor -module CommonLBs

$(LIBS):  
	make -C libs

clean:
	make -C libs clean
	rm -f multirun multirun_time *.o *.a charmrun

test: all
	mpirun -np 16 ./multirun_time


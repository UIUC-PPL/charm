/*  Example code to demonstrate Interoperability between MPI and Charm
    Author - Nikhil Jain
    Contact - nikhil@illinois.edu
*/

//standard header files
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
//header files for libraries in Charm I wish to use with MPI
#include "libs/hello/hello.h"
#include "libs/kNeighbor/kNeighbor.h"
//header file from Charm needed for Interoperation
#include "mpi-interoperate.h"

int main(int argc, char **argv){
  int peid, numpes;
  MPI_Comm newComm;

  //basic MPI initilization
  MPI_Init(&argc, &argv);
  MPI_Comm_rank(MPI_COMM_WORLD, &peid);
  MPI_Comm_size(MPI_COMM_WORLD, &numpes);

  if(numpes % 4 != 0){
    if(peid==0){
      printf("This test program must be run with number of procs = 4x\n");
    }
    MPI_Finalize();
    return 1;
  }

  // Two colors. CharmLibInit requires a communicator congruent to
  // MPI_COMM_WORLD, so on reconverse this aborts with an explanation.
  //splitting WORLD into 2 sets
  MPI_Comm_split(MPI_COMM_WORLD, peid%2, peid, &newComm);

  //initialize Charm for each set
  CharmLibInit(newComm, argc, argv);
  MPI_Barrier(MPI_COMM_WORLD);

  //do some MPI work
  for(int i=0; i<5; i++) {
    if(peid % 2 == 0) {    
      MPI_Send(&peid, 1, MPI_INT, peid+1, 808, MPI_COMM_WORLD);
    } else {
      int recvid = 0;
      MPI_Status sts;
      MPI_Recv(&recvid, 1, MPI_INT, peid-1, 808, MPI_COMM_WORLD, &sts);
    }
  }

  //on the first set of processors, invoke Hello
  if(peid%2) {
    HelloStart(5);
    MPI_Barrier(newComm);
  } else {
  //on the other set of processors, invoke kNeighbor and clean up Charm
    kNeighbor(10,10,128,2);
    MPI_Barrier(newComm);
    CharmLibExit();
  }
    
  //on the first set, do more MPI work, invoke Hello and clean up Charm
  if(peid%2) {
    for(int i=0; i<5; i++) {
      if(peid % 4 == 1) {    
        MPI_Send(&peid, 1, MPI_INT, peid+2, 808, MPI_COMM_WORLD);
      }  else {
        int recvid = 0;
        MPI_Status sts;
        MPI_Recv(&recvid, 1, MPI_INT, peid-2, 808, MPI_COMM_WORLD, &sts);
      }
    }
    HelloStart(16);
    MPI_Barrier(newComm);
    CharmLibExit();
  }

  //final synchronization
  MPI_Barrier(MPI_COMM_WORLD);

  MPI_Finalize();
  return 0;  
}

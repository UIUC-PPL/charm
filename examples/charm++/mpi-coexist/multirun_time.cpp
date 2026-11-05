/*  Example code to demonstrate time sharing interoperability between MPI and Charm
    Author - Nikhil Jain
    Contact - nikhil@illinois.edu
 */

//standard header files
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
//header files for libraries in Charm I wish to use with MPI
#include "libs/hi/hi.h"
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

  if(numpes % 2 != 0){
    if(peid==0){
      printf("This test program must be run with number of procs = 2x\n");
    }
    MPI_Finalize();
    return 1;
  }

  // One color: newComm is congruent to MPI_COMM_WORLD.
  MPI_Comm_split(MPI_COMM_WORLD, 1, peid, &newComm);

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

  //Hello
  HelloStart(5);
  MPI_Barrier(newComm);

  for(int i=0; i<5; i++) {
    if(peid % 2 == 1) {    
      MPI_Send(&peid, 1, MPI_INT, peid-1, 808, MPI_COMM_WORLD);
    }  else {
      int recvid = 0;
      MPI_Status sts;
      MPI_Recv(&recvid, 1, MPI_INT, peid+1, 808, MPI_COMM_WORLD, &sts);
    }
  }

  HiStart(16);
  MPI_Barrier(newComm);

  if(!peid)
    printf("Invoke charmlib exit\n");

  CharmLibExit();

  // MPI keeps working after CharmLibExit returns.
  int one = 1, sum = 0;
  MPI_Allreduce(&one, &sum, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
  MPI_Barrier(MPI_COMM_WORLD);
  if (sum != numpes) {
    printf("FAIL: rank %d: allreduce after CharmLibExit gave %d, expected %d\n",
           peid, sum, numpes);
    MPI_Abort(MPI_COMM_WORLD, 1);
  }
  if (!peid)
    printf("PASS: MPI works after CharmLibExit (%d ranks)\n", numpes);

  MPI_Finalize();
  if (!peid) printf("MPI_Finalize returned\n");
  return 0;  
}

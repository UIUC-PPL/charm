// Regression test for issue #4117: each AMPI communicator is a chare array,
// and each array takes a collection number that is never reused. With the
// standard id layout (12 collection bits) a run could create at most 4,095
// arrays in total, live or freed, and then aborted with "Charm++ ran out of
// collection numbers". This test creates and frees 5,000 communicators with
// MPI_Comm_dup and 5,000 with MPI_Comm_split, which crosses that old limit.
#include <stdio.h>
#include <stdlib.h>
#include "mpi.h"

#define ITERS 5000

int main(int argc, char **argv)
{
  int rank, size;
  MPI_Init(&argc, &argv);
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &size);

  for (int i = 1; i <= ITERS; i++) {
    MPI_Comm c;
    MPI_Comm_dup(MPI_COMM_WORLD, &c);
    int csize;
    MPI_Comm_size(c, &csize);
    if (csize != size) {
      printf("[%d] MPI_Comm_dup iteration %d: size %d, expected %d\n", rank, i, csize, size);
      MPI_Abort(MPI_COMM_WORLD, 1);
    }
    MPI_Comm_free(&c);
    if (rank == 0 && i % 1000 == 0) {
      printf("MPI_Comm_dup/free: %d iterations done\n", i);
      fflush(stdout);
    }
  }

  for (int i = 1; i <= ITERS; i++) {
    MPI_Comm c;
    MPI_Comm_split(MPI_COMM_WORLD, rank % 2, rank, &c);
    int crank, csize;
    MPI_Comm_rank(c, &crank);
    MPI_Comm_size(c, &csize);
    int expected = (size + 1 - rank % 2) / 2;
    if (csize != expected || crank != rank / 2) {
      printf("[%d] MPI_Comm_split iteration %d: rank %d size %d, expected rank %d size %d\n",
             rank, i, crank, csize, rank / 2, expected);
      MPI_Abort(MPI_COMM_WORLD, 1);
    }
    MPI_Comm_free(&c);
    if (rank == 0 && i % 1000 == 0) {
      printf("MPI_Comm_split/free: %d iterations done\n", i);
      fflush(stdout);
    }
  }

  MPI_Barrier(MPI_COMM_WORLD);
  if (rank == 0) printf("All tests passed\n");
  MPI_Finalize();
  return 0;
}

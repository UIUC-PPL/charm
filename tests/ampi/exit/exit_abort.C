#include <stdlib.h>
#include <stdio.h>
#include <mpi.h>

// MPI_Abort must end the program with its error code as the exit status.
// Usage: exit_abort <rank> <code>
// Rank <rank> calls MPI_Abort(MPI_COMM_WORLD, <code>) after a barrier; every
// other rank waits in MPI_Recv for a message that is never sent, which is the
// ordinary situation in which a program aborts. No rank may continue.
int main(int argc, char *argv[])
{
  MPI_Init(&argc, &argv);
  int rank, size;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &size);
  int abortRank = (argc > 1) ? atoi(argv[1]) : 0;
  int code = (argc > 2) ? atoi(argv[2]) : 1;
  if (abortRank < 0 || abortRank >= size) {
    if (rank == 0) printf("exit_abort: rank %d out of range (%d ranks)\n", abortRank, size);
    MPI_Finalize();
    return 1;
  }

  MPI_Barrier(MPI_COMM_WORLD);

  if (rank == abortRank) {
    MPI_Abort(MPI_COMM_WORLD, code);
    printf("exit_abort: MPI_Abort returned\n");
    return 1;
  }

  int buf;
  MPI_Recv(&buf, 1, MPI_INT, abortRank, 12345, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
  printf("exit_abort: rank %d continued after MPI_Abort\n", rank);
  return 1;
}

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <mpi.h>

// main returns a value after MPI_Finalize; the program's exit status must be
// that value. Usage: exit_after_finalize <code> [mixed]
// With "mixed", rank 0 returns 0 and every other rank returns <code>, so the
// exit status must still be nonzero when <code> is nonzero.
int main(int argc, char *argv[])
{
  MPI_Init(&argc, &argv);
  int rank;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  int code = (argc > 1) ? atoi(argv[1]) : 0;
  bool mixed = (argc > 2 && strcmp(argv[2], "mixed") == 0);

  MPI_Finalize();

  // MPI_Finalized is one of the calls allowed after MPI_Finalize.
  int finalized = 0;
  MPI_Finalized(&finalized);
  if (!finalized) {
    printf("[%d] MPI_Finalized reported 0 after MPI_Finalize\n", rank);
    return 99;
  }

  if (rank == 0)
    printf("After MPI_Finalize, main returns %d%s.\n", code,
           mixed ? " on ranks other than 0" : "");
  if (mixed && rank == 0)
    return 0;
  return code;
}

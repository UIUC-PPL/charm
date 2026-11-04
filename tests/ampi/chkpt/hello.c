/*
 * AMPI checkpoint and restart test.
 *
 * Each rank runs NSTEPS steps of a ring exchange: it sends two values that
 * depend on its rank and the step number to its right neighbour, receives
 * the left neighbour's values, and checks them against what the left
 * neighbour must have sent in that step. A running checksum of the received
 * values and a count of checked values are kept in local variables, and
 * every received value is also stored in an array from malloc, which lives
 * in the rank's Isomalloc heap (ampicc links -memory isomalloc). At step
 * 2 every rank calls AMPI_Migrate with "ampi_checkpoint" set to
 * "to_file=log", which writes a checkpoint to the directory log/.
 *
 * Run as a checkpoint run (./hello +vp8), the program executes all steps.
 * Run with "+restart log", each rank's thread resumes from the checkpoint
 * inside AMPI_Migrate with its stack (and so the local variables, including
 * the checksum and count) and its heap (the array) restored, and executes
 * steps 3 to 5 only. The
 * Makefile restarts the same checkpoint on the same, fewer and more PEs and
 * processes than the checkpoint run used.
 *
 * At the end each rank compares its checksum with the expected value and
 * the array's contents with the values its left neighbour sent, and
 * rank 0 checks that the total count of checked values equals NSTEPS*size
 * and prints "All tests passed". Any mismatch prints an ERROR line and
 * calls MPI_Abort, so the program exits nonzero.
 */
#include <stdio.h>
#include <stdlib.h>
#include <mpi.h>

#define CHKPT_TO_FILE 1 // define to 0 for in-memory checkpointing

#define NSTEPS 6
#define CHKPT_STEP 2

static double value0(int rank, int step) { return rank + 0.01 * step; }
static double value1(int rank, int step) { return 1000.0 * rank + step; }

static void fail(int myrank) {
  fflush(stdout);
  MPI_Abort(MPI_COMM_WORLD, 1);
  exit(1);
}

int main(int argc, char **argv) {
  int myrank, size, leftnbr, rightnbr;
  int step = 0;
  int s;
  double a[2], b[2];
  double checksum = 0.0, expected_checksum = 0.0;
  double *received;  /* heap: 2 values per step, filled as they arrive */
  int nchecked = 0, total_checked = 0;
  MPI_Status sts;

  MPI_Init(&argc, &argv);
  MPI_Comm_rank(MPI_COMM_WORLD, &myrank);
  MPI_Comm_size(MPI_COMM_WORLD, &size);

  MPI_Info chkpt_info;
  MPI_Info_create(&chkpt_info);
#if CHKPT_TO_FILE
  MPI_Info_set(chkpt_info, "ampi_checkpoint", "to_file=log");
#else
  MPI_Info_set(chkpt_info, "ampi_checkpoint", "in_memory");
#endif

  leftnbr = (myrank + size - 1) % size;
  rightnbr = (myrank + 1) % size;

  received = (double *)malloc(2 * NSTEPS * sizeof(double));
  for (s = 0; s < 2 * NSTEPS; s++)
    received[s] = -1.0;

  for (step = 0; step < NSTEPS; step++) {
    a[0] = value0(myrank, step);
    a[1] = value1(myrank, step);
    b[0] = b[1] = -1.0;
    MPI_Send(a, 2, MPI_DOUBLE, rightnbr, 0, MPI_COMM_WORLD);
    MPI_Recv(b, 2, MPI_DOUBLE, leftnbr, 0, MPI_COMM_WORLD, &sts);
    if (b[0] != value0(leftnbr, step) || b[1] != value1(leftnbr, step)) {
      printf("[%d] ERROR: step %d: got {%f,%f} expected {%f,%f}\n", myrank,
             step, b[0], b[1], value0(leftnbr, step), value1(leftnbr, step));
      fail(myrank);
    }
    checksum += b[0] + b[1];
    received[2 * step] = b[0];
    received[2 * step + 1] = b[1];
    nchecked++;
    if (myrank == 0)
      printf("[%d]step %d,a={%f,%f},b={%f,%f}\n", myrank, step, a[0], a[1],
             b[0], b[1]);
    if (step == CHKPT_STEP) {
      AMPI_Migrate(chkpt_info);
    }
  }

  /* The same additions in the same order as above, so the comparison is exact. */
  for (s = 0; s < NSTEPS; s++)
    expected_checksum += value0(leftnbr, s) + value1(leftnbr, s);
  if (checksum != expected_checksum || nchecked != NSTEPS) {
    printf("[%d] ERROR: step %d: got checksum %f over %d steps expected %f over %d steps\n",
           myrank, step, checksum, nchecked, expected_checksum, NSTEPS);
    fail(myrank);
  }

  /* The heap array: the entries from before the checkpoint come back only
     if the rank's heap was restored with its stack. */
  for (s = 0; s < NSTEPS; s++) {
    if (received[2 * s] != value0(leftnbr, s) || received[2 * s + 1] != value1(leftnbr, s)) {
      printf("[%d] ERROR: heap array entry for step %d holds {%f,%f} expected {%f,%f}\n",
             myrank, s, received[2 * s], received[2 * s + 1], value0(leftnbr, s), value1(leftnbr, s));
      fail(myrank);
    }
  }
  free(received);

  MPI_Reduce(&nchecked, &total_checked, 1, MPI_INT, MPI_SUM, 0, MPI_COMM_WORLD);
  if (myrank == 0) {
    if (total_checked != NSTEPS * size) {
      printf("[%d] ERROR: step %d: got %d checked values expected %d\n", myrank,
             step, total_checked, NSTEPS * size);
      fail(myrank);
    }
    printf("All tests passed\n");
    fflush(stdout);
  }

  MPI_Info_free(&chkpt_info);
  MPI_Finalize();
  return 0;
}

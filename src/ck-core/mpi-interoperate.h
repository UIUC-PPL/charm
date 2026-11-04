#ifndef _MPI_INTEROPERATE_
#define _MPI_INTEROPERATE_

#include "converse.h"
#include "ck.h"
#include "trace.h"

// CharmLibInit takes an MPI communicator, so it is declared only in a
// translation unit that includes mpi.h before this header (or defines
// CMK_INTEROP_USE_MPI). Merely finding mpi.h on the include path is not
// enough: Open MPI's mpi.h pulls its C++ bindings into every C++ file, which
// then needs libmpi_cxx at link time. libck itself needs no MPI.
#if CMK_CONVERSE_MPI || defined(MPI_VERSION) || defined(CMK_INTEROP_USE_MPI)
#define CMK_INTEROP_HAVE_MPI_H 1
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

void CharmLibInitWorld(int argc, char **argv);
int CharmInteropPickRendezvous(char *host, int hostlen);
void CharmLibInitWorldTcp(int rank, int size, int local_rank, int local_size,
                          const char *host, int port, int argc, char **argv);

void CharmBeginInit(int argc, char** argv);
void CharmFinishInit();
void CharmInit(int argc, char** argv);

void CharmLibExit();

void LibCkExit(void);

void StartCharmScheduler();
void StopCharmScheduler();

#ifdef __cplusplus
}
#endif

#if CMK_INTEROP_HAVE_MPI_H
// Starts Charm++ inside an MPI program. Charm++ runs on every rank of the job:
// userComm must be MPI_COMM_WORLD or congruent to it, and Charm++ process i is
// MPI_COMM_WORLD rank i. After CharmLibExit returns the program may keep
// using MPI and must call MPI_Finalize itself.
static inline void CharmLibInit(MPI_Comm userComm, int argc, char **argv) {
  int cmp = MPI_UNEQUAL, rank = -1, size = 0;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &size);
  MPI_Comm_compare(userComm, MPI_COMM_WORLD, &cmp);
  if (cmp != MPI_IDENT && cmp != MPI_CONGRUENT) {
    fprintf(stderr,
            "CharmLibInit (rank %d): the communicator must be MPI_COMM_WORLD "
            "or congruent to it. Charm++ on reconverse runs on every rank of "
            "the job; running it on a subset of ranks (a split communicator) "
            "is not supported.\n", rank);
    fflush(stderr);
    MPI_Abort(MPI_COMM_WORLD, 1);
  }
  // A launcher that does not match the MPI library makes every process a
  // 1-rank MPI job; the launcher's own task count still says how many it
  // started. Checked before Charm++ starts, so the warning comes first.
  if (size == 1 && rank == 0) {
    const char *vars[] = {"OMPI_COMM_WORLD_SIZE", "PMI_SIZE",
                          "SLURM_STEP_NUM_TASKS"};
    for (const char *var : vars) {
      const char *val = getenv(var);
      if (val == NULL) continue;
      if (atoi(val) > 1) {
        fprintf(stderr,
                "Warning: CharmLibInit: MPI_COMM_WORLD has 1 rank, but %s=%s: "
                "the launcher may have started %s processes, each its own MPI "
                "job. Launch with the launcher your MPI requires (for Cray "
                "MPICH under Slurm, srun --mpi=cray_shasta).\n", var, val, val);
        fflush(stderr);
      }
      break;
    }
  }
  // Bootstrap LCI over LCT's tcp backend, rendezvous at rank 0, so the
  // launcher's PMI is left to MPI.
  char host[256] = {0};
  int port = -1;
  if (rank == 0) port = CharmInteropPickRendezvous(host, sizeof(host));
  MPI_Bcast(&port, 1, MPI_INT, 0, MPI_COMM_WORLD);
  MPI_Bcast(host, sizeof(host), MPI_CHAR, 0, MPI_COMM_WORLD);
  if (port < 0) {
    if (rank == 0)
      fprintf(stderr, "CharmLibInit: no TCP port for the LCI bootstrap\n");
    MPI_Abort(MPI_COMM_WORLD, 1);
  }
  MPI_Comm local;
  int local_rank = 0, local_size = 1;
  MPI_Comm_split_type(MPI_COMM_WORLD, MPI_COMM_TYPE_SHARED, rank,
                      MPI_INFO_NULL, &local);
  MPI_Comm_rank(local, &local_rank);
  MPI_Comm_size(local, &local_size);
  MPI_Comm_free(&local);
  CharmLibInitWorldTcp(rank, size, local_rank, local_size, host, port, argc,
                       argv);
  if (CmiMyNode() != rank || CmiNumNodes() != size) {
    fprintf(stderr,
            "CharmLibInit: Charm++ process %d of %d is MPI_COMM_WORLD rank %d "
            "of %d; the launcher gave Charm++ and MPI different process "
            "numbering, which MPI interop requires to match.\n",
            CmiMyNode(), CmiNumNodes(), rank, size);
    fflush(stderr);
    MPI_Abort(MPI_COMM_WORLD, 1);
  }
}
#endif

#undef CkExit
#define CkExit LibCkExit

#endif //_MPI_INTEROPERATE_

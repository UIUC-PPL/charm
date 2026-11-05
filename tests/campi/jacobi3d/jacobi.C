/*
 * campi test: 3D Jacobi relaxation, the campi counterpart of
 * examples/ampi/Cjacobi3D/jacobi.C, with load balancing and checkpoint/restart.
 *
 *   ./jacobi X Y Z [nIter] [-chkpt ITER DIR] +vpN [+pe P] [+balancer LB]
 *
 * X*Y*Z ranks, each owning a DIMX x DIMY x DIMZ block with a one-cell halo,
 * exchange halos with their six neighbours and relax for nIter iterations
 * (default 20). Rank 0 prints the maximum change per iteration and a final
 * line "jacobi: N iterations, final maxerr ..." that is the same whether the
 * run was interrupted or not, and the same as thread AMPI's.
 *
 * Every rank's data across iterations is in its State object, which campi
 * packs (State::pup) when the rank migrates or is checkpointed. Two migrate
 * sites: load balancing at iter % 10 == 5 (with "+balancer <LB>" the runtime
 * moves ranks there), and, with "-chkpt ITER DIR", a checkpoint to the
 * directory DIR after iteration ITER. A later run with "+restart DIR", on any
 * number of PEs or processes and with the same +vp, restarts every rank at
 * the checkpoint site and runs iterations ITER+1 to nIter. The program's
 * arguments are not read again on a restart (the code before the site is
 * skipped). Unlike thread AMPI, the restart needs neither address space
 * layout randomization off nor Isomalloc:
 *
 *   ./jacobi 2 2 2 40 -chkpt 20 log +vp8 +pe 4
 *   ./jacobi 2 2 2 40 -chkpt 20 log +vp8 +pe 2 +restart log
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include "mpi.h"
#include "campi.h"

#define DIMX 100
#define DIMY 100
#define DIMZ 100

class chunk {
  public:
    double t[DIMX+2][DIMY+2][DIMZ+2];
    int xidx, yidx, zidx;
    int xm, xp, ym, yp, zm, zp;
    double sbxm[DIMY*DIMZ];
    double sbxp[DIMY*DIMZ];
    double sbym[DIMX*DIMZ];
    double sbyp[DIMX*DIMZ];
    double sbzm[DIMX*DIMY];
    double sbzp[DIMX*DIMY];
    double rbxm[DIMY*DIMZ];
    double rbxp[DIMY*DIMZ];
    double rbym[DIMX*DIMZ];
    double rbyp[DIMX*DIMZ];
    double rbzm[DIMX*DIMY];
    double rbzp[DIMX*DIMY];

    void pup(PUP::er &p) {
      PUParray(p, &t[0][0][0], (DIMX+2)*(DIMY+2)*(DIMZ+2));
      p | xidx; p | yidx; p | zidx;
      p | xp; p | xm; p | yp; p | ym; p | zp; p | zm;
      PUParray(p, sbxm, DIMY*DIMZ); PUParray(p, sbxp, DIMY*DIMZ);
      PUParray(p, rbxm, DIMY*DIMZ); PUParray(p, rbxp, DIMY*DIMZ);
      PUParray(p, sbym, DIMX*DIMZ); PUParray(p, sbyp, DIMX*DIMZ);
      PUParray(p, rbym, DIMX*DIMZ); PUParray(p, rbyp, DIMX*DIMZ);
      PUParray(p, sbzm, DIMX*DIMY); PUParray(p, sbzp, DIMX*DIMY);
      PUParray(p, rbzm, DIMX*DIMY); PUParray(p, rbzp, DIMX*DIMY);
    }
};

// Everything a rank carries from one iteration to the next.
struct State {
  int rank = 0, size = 0;
  int NX = 0, NY = 0, NZ = 0;
  int niter = 0, iter = 0;
  double maxerr = 0.0, starttime = 0.0;
  int chkpt_iter = 0;
  std::string chkpt_dir;           // empty: no checkpoint
  MPI_Info chkpt_info = MPI_INFO_NULL;
  chunk *cp = nullptr;

  State() = default;
  State(const State &) = delete;
  State &operator=(const State &) = delete;
  ~State() { delete cp; }

  void pup(PUP::er &p) {
    p | rank; p | size;
    p | NX; p | NY; p | NZ;
    p | niter; p | iter;
    p | maxerr; p | starttime;
    p | chkpt_iter; p | chkpt_dir; p | chkpt_info;
    bool hasChunk = (cp != nullptr);
    p | hasChunk;
    if (hasChunk) {
      if (p.isUnpacking()) cp = new chunk;
      cp->pup(p);
    }
  }
};

#define abs(x) ((x)<0.0 ? -(x) : (x))

static int index1d(const State &s, int ix, int iy, int iz)
{
  return s.NY*s.NZ*ix + s.NZ*iy + iz;
}

static void index3d(const State &s, int index, int& ix, int& iy, int& iz)
{
  ix = index/(s.NY*s.NZ);
  iy = (index%(s.NY*s.NZ))/s.NZ;
  iz = index%s.NZ;
}

static void copyout(double *d, double t[DIMX+2][DIMY+2][DIMZ+2],
                    int sx, int ex, int sy, int ey, int sz, int ez)
{
  int i, j, k;
  int l = 0;
  for(i=sx; i<=ex; i++)
    for(j=sy; j<=ey; j++)
      for(k=sz; k<=ez; k++, l++)
        d[l] = t[i][j][k];
}

static void copyin(double *d, double t[DIMX+2][DIMY+2][DIMZ+2],
                    int sx, int ex, int sy, int ey, int sz, int ez)
{
  int i, j, k;
  int l = 0;
  for(i=sx; i<=ex; i++)
    for(j=sy; j<=ey; j++)
      for(k=sz; k<=ez; k++, l++)
        t[i][j][k] = d[l];
}

CAMPI_MAIN_STATE(State, ac, av, s)
{
  // Scratch values within one iteration; none is live across a migrate site.
  int i, j, k, m;
  double error, tval, itertime;
  chunk *cp;
  MPI_Request req[12];

  CAMPI_RESUMABLE();

  MPI_Init(&ac, &av);
  MPI_Comm_rank(MPI_COMM_WORLD, &s.rank);
  MPI_Comm_size(MPI_COMM_WORLD, &s.size);

  if (ac < 4) {
    if (s.rank == 0)
      printf("Usage: jacobi X Y Z [nIter] [-chkpt ITER DIR]\n");
    MPI_Finalize();
    co_return 1;
  }
  s.NX = atoi(av[1]);
  s.NY = atoi(av[2]);
  s.NZ = atoi(av[3]);
  if (s.NX*s.NY*s.NZ != s.size) {
    if (s.rank == 0)
      printf("%d x %d x %d != %d\n", s.NX, s.NY, s.NZ, s.size);
    MPI_Finalize();
    co_return 2;
  }
  s.niter = 20;
  for (i = 4; i < ac; i++) {
    if (strcmp(av[i], "-chkpt") == 0 && i + 2 < ac) {
      s.chkpt_iter = atoi(av[i+1]);
      s.chkpt_dir = av[i+2];
      i += 2;
    } else if (av[i][0] != '+' && av[i][0] != '-') {
      s.niter = atoi(av[i]);
    }
  }

  MPI_Bcast(&s.niter, 1, MPI_INT, 0, MPI_COMM_WORLD);

  if (!s.chkpt_dir.empty()) {
    char value[MPI_MAX_INFO_VAL];
    snprintf(value, sizeof(value), "to_file=%s", s.chkpt_dir.c_str());
    MPI_Info_create(&s.chkpt_info);
    MPI_Info_set(s.chkpt_info, "ampi_checkpoint", value);
  }

  s.cp = cp = new chunk;
  index3d(s, s.rank, cp->xidx, cp->yidx, cp->zidx);
  cp->xp = index1d(s, (cp->xidx+1)%s.NX, cp->yidx, cp->zidx);
  cp->xm = index1d(s, (cp->xidx+s.NX-1)%s.NX, cp->yidx, cp->zidx);
  cp->yp = index1d(s, cp->xidx, (cp->yidx+1)%s.NY, cp->zidx);
  cp->ym = index1d(s, cp->xidx, (cp->yidx+s.NY-1)%s.NY, cp->zidx);
  cp->zp = index1d(s, cp->xidx, cp->yidx, (cp->zidx+1)%s.NZ);
  cp->zm = index1d(s, cp->xidx, cp->yidx, (cp->zidx+s.NZ-1)%s.NZ);
  for(i=1; i<=DIMZ; i++)
    for(j=1; j<=DIMY; j++)
      for(k=1; k<=DIMX; k++)
        cp->t[k][j][i] = DIMY*DIMX*(i-1) + DIMX*(j-2) + (k-1);

  MPI_Barrier(MPI_COMM_WORLD);
  s.starttime = MPI_Wtime();

  for(s.iter=1; s.iter<=s.niter; s.iter++) {
    cp = s.cp;
    s.maxerr = 0.0;
    copyout(cp->sbxm, cp->t, 1, 1, 1, DIMY, 1, DIMZ);
    copyout(cp->sbxp, cp->t, DIMX, DIMX, 1, DIMY, 1, DIMZ);
    copyout(cp->sbym, cp->t, 1, DIMX, 1, 1, 1, DIMZ);
    copyout(cp->sbyp, cp->t, 1, DIMX, DIMY, DIMY, 1, DIMZ);
    copyout(cp->sbzm, cp->t, 1, DIMX, 1, DIMY, 1, 1);
    copyout(cp->sbzp, cp->t, 1, DIMX, 1, DIMY, DIMZ, DIMZ);

    MPI_Irecv(cp->rbxp, DIMY*DIMZ, MPI_DOUBLE, cp->xp, 0, MPI_COMM_WORLD, &req[0]);
    MPI_Irecv(cp->rbxm, DIMY*DIMZ, MPI_DOUBLE, cp->xm, 1, MPI_COMM_WORLD, &req[1]);
    MPI_Irecv(cp->rbyp, DIMX*DIMZ, MPI_DOUBLE, cp->yp, 2, MPI_COMM_WORLD, &req[2]);
    MPI_Irecv(cp->rbym, DIMX*DIMZ, MPI_DOUBLE, cp->ym, 3, MPI_COMM_WORLD, &req[3]);
    MPI_Irecv(cp->rbzm, DIMX*DIMY, MPI_DOUBLE, cp->zm, 5, MPI_COMM_WORLD, &req[4]);
    MPI_Irecv(cp->rbzp, DIMX*DIMY, MPI_DOUBLE, cp->zp, 4, MPI_COMM_WORLD, &req[5]);

    MPI_Isend(cp->sbxm, DIMY*DIMZ, MPI_DOUBLE, cp->xm, 0, MPI_COMM_WORLD, &req[6]);
    MPI_Isend(cp->sbxp, DIMY*DIMZ, MPI_DOUBLE, cp->xp, 1, MPI_COMM_WORLD, &req[7]);
    MPI_Isend(cp->sbym, DIMX*DIMZ, MPI_DOUBLE, cp->ym, 2, MPI_COMM_WORLD, &req[8]);
    MPI_Isend(cp->sbyp, DIMX*DIMZ, MPI_DOUBLE, cp->yp, 3, MPI_COMM_WORLD, &req[9]);
    MPI_Isend(cp->sbzm, DIMX*DIMY, MPI_DOUBLE, cp->zm, 4, MPI_COMM_WORLD, &req[10]);
    MPI_Isend(cp->sbzp, DIMX*DIMY, MPI_DOUBLE, cp->zp, 5, MPI_COMM_WORLD, &req[11]);

    MPI_Waitall(12, req, MPI_STATUSES_IGNORE);

    copyin(cp->sbxm, cp->t, 0, 0, 1, DIMY, 1, DIMZ);
    copyin(cp->sbxp, cp->t, DIMX+1, DIMX+1, 1, DIMY, 1, DIMZ);
    copyin(cp->sbym, cp->t, 1, DIMX, 0, 0, 1, DIMZ);
    copyin(cp->sbyp, cp->t, 1, DIMX, DIMY+1, DIMY+1, 1, DIMZ);
    copyin(cp->sbzm, cp->t, 1, DIMX, 1, DIMY, 0, 0);
    copyin(cp->sbzp, cp->t, 1, DIMX, 1, DIMY, DIMZ+1, DIMZ+1);

    if(s.iter > 25 && s.iter < 85 && s.rank == 35)
      m = 9;
    else
      m = 1;
    for(; m>0; m--)
      for(i=1; i<=DIMZ; i++)
        for(j=1; j<=DIMY; j++)
          for(k=1; k<=DIMX; k++) {
            tval = (cp->t[k][j][i]   + cp->t[k][j][i+1] +
                    cp->t[k][j][i-1] + cp->t[k][j+1][i] +
                    cp->t[k][j-1][i] + cp->t[k+1][j][i] +
                    cp->t[k-1][j][i]) / 7.0;
            error = abs(tval-cp->t[k][j][i]);
            cp->t[k][j][i] = tval;
            if (error > s.maxerr) s.maxerr = error;
          }
    MPI_Allreduce(MPI_IN_PLACE, &s.maxerr, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);

    itertime = MPI_Wtime() - s.starttime;
    MPI_Allreduce(MPI_IN_PLACE, &itertime, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
    if (s.rank == 0)
      printf("iter %d time: %lf maxerr: %lf\n", s.iter, itertime / s.size, s.maxerr);
    s.starttime = MPI_Wtime();
    if (!s.chkpt_dir.empty() && s.iter == s.chkpt_iter) {
      // Writes the checkpoint; a run started with "+restart <dir>" restarts
      // every rank here, with s.iter == s.chkpt_iter, and goes on.
      if (s.rank == 0)
        printf("iter %d: checkpoint to %s\n", s.iter, s.chkpt_dir.c_str());
      CAMPI_MIGRATE(s.chkpt_info);
      s.starttime = MPI_Wtime(); // the clock of this process
      if (s.rank == 0) {
        char where[MPI_MAX_PROCESSOR_NAME];
        int len;
        MPI_Get_processor_name(where, &len);
        printf("iter %d: continuing after the checkpoint; rank 0 is %s\n", s.iter, where);
      }
    } else if (s.iter%10 == 5) {
      CAMPI_MIGRATE(AMPI_INFO_LB_SYNC);
      s.starttime = MPI_Wtime(); // the clock of this process
    }
  }
  if (s.rank == 0)
    printf("jacobi: %d iterations, final maxerr %.17g\n", s.niter, s.maxerr);
  if (s.chkpt_info != MPI_INFO_NULL)
    MPI_Info_free(&s.chkpt_info);
  MPI_Finalize();
  CAMPI_END();
  co_return 0;
}

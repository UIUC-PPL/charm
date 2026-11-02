// campi test: a copy of examples/ampi/Cjacobi3D/jacobi.C converted with campi.h
// (CAMPI_MAIN and co_return are the only changes).
#include <stdio.h>
#include <stdlib.h>
#include "mpi.h"
#include "campi.h"

#define DIMX 100
#define DIMY 100
#define DIMZ 100

int NX, NY, NZ;

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
};

#ifdef AMPI
void chunk_pup(pup_er p, void *d)
{
  chunk **cpp = (chunk **) d;
  if(pup_isUnpacking(p))
    *cpp = new chunk;
  chunk *cp = *cpp;
  pup_doubles(p, &cp->t[0][0][0], (DIMX+2)*(DIMY+2)*(DIMZ+2));
  pup_int(p, &cp->xidx);
  pup_int(p, &cp->yidx);
  pup_int(p, &cp->zidx);
  pup_int(p, &cp->xp);
  pup_int(p, &cp->xm);
  pup_int(p, &cp->yp);
  pup_int(p, &cp->ym);
  pup_int(p, &cp->zp);
  pup_int(p, &cp->zm);
  pup_doubles(p, cp->sbxm, (DIMY*DIMZ));
  pup_doubles(p, cp->sbxp, (DIMY*DIMZ));
  pup_doubles(p, cp->rbxm, (DIMY*DIMZ));
  pup_doubles(p, cp->rbxp, (DIMY*DIMZ));
  pup_doubles(p, cp->sbym, (DIMX*DIMZ));
  pup_doubles(p, cp->sbyp, (DIMX*DIMZ));
  pup_doubles(p, cp->rbym, (DIMX*DIMZ));
  pup_doubles(p, cp->rbyp, (DIMX*DIMZ));
  pup_doubles(p, cp->sbzm, (DIMX*DIMY));
  pup_doubles(p, cp->sbzp, (DIMX*DIMY));
  pup_doubles(p, cp->rbzm, (DIMX*DIMY));
  pup_doubles(p, cp->rbzp, (DIMX*DIMY));
  if(pup_isDeleting(p))
    delete cp;
}
#endif

#define abs(x) ((x)<0.0 ? -(x) : (x))

int index1d(int ix, int iy, int iz)
{
  return NY*NZ*ix + NZ*iy + iz;
}

void index3d(int index, int& ix, int& iy, int& iz)
{
  ix = index/(NY*NZ);
  iy = (index%(NY*NZ))/NZ;
  iz = index%NZ;
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

CAMPI_MAIN(ac, av)
{
  int i, j, k, m;
  int iter, niter, cp_idx;
  double maxerr, error, tval, starttime, itertime;
  chunk *cp;
  int rank, size;
  MPI_Request req[12];

  MPI_Init(&ac, &av);
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &size);

  if (ac < 4) {
    if (rank == 0)
      printf("Usage: jacobi X Y Z [nIter].\n");
    MPI_Finalize();
    co_return 1;
  }
  NX = atoi(av[1]);
  NY = atoi(av[2]);
  NZ = atoi(av[3]);
  if (NX*NY*NZ != size) {
    if (rank == 0)
      printf("%d x %d x %d != %d\n", NX,NY,NZ, size);
    MPI_Finalize();
    co_return 2;
  }
  if (ac == 5)
    niter = atoi(av[4]);
  else
    niter = 20;

  MPI_Bcast(&niter, 1, MPI_INT, 0, MPI_COMM_WORLD);

  cp = new chunk;
#if defined(AMPI) && ! defined(NO_PUP)
  AMPI_Register_pup((MPI_PupFn)chunk_pup, (void*)&cp, &cp_idx);
#endif

  index3d(rank, cp->xidx, cp->yidx, cp->zidx);
  cp->xp = index1d((cp->xidx+1)%NX,cp->yidx,cp->zidx);
  cp->xm = index1d((cp->xidx+NX-1)%NX,cp->yidx,cp->zidx);
  cp->yp = index1d(cp->xidx,(cp->yidx+1)%NY,cp->zidx);
  cp->ym = index1d(cp->xidx,(cp->yidx+NY-1)%NY,cp->zidx);
  cp->zp = index1d(cp->xidx,cp->yidx,(cp->zidx+1)%NZ);
  cp->zm = index1d(cp->xidx,cp->yidx,(cp->zidx+NZ-1)%NZ);
  for(i=1; i<=DIMZ; i++)
    for(j=1; j<=DIMY; j++)
      for(k=1; k<=DIMX; k++)
        cp->t[k][j][i] = DIMY*DIMX*(i-1) + DIMX*(j-2) + (k-1);

  MPI_Barrier(MPI_COMM_WORLD);
  starttime = MPI_Wtime();

  for(iter=1; iter<=niter; iter++) {
    maxerr = 0.0;
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

    if(iter > 25 && iter < 85 && rank == 35)
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
            if (error > maxerr) maxerr = error;
          }
    MPI_Allreduce(MPI_IN_PLACE, &maxerr, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);

    itertime = MPI_Wtime() - starttime;
    MPI_Allreduce(MPI_IN_PLACE, &itertime, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
    if (rank == 0)
      printf("iter %d time: %lf maxerr: %lf\n", iter, itertime / size, maxerr);
    starttime = MPI_Wtime();
#ifdef AMPI
    if(iter%10 == 5) {
      AMPI_Migrate(AMPI_INFO_LB_SYNC);
    }
#endif
  }
  MPI_Finalize();
  co_return 0;
}

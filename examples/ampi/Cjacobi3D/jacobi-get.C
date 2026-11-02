#include <stdio.h>
#include <stdlib.h>
#include "mpi.h"
#include <math.h>

#define DIMX 100 
#define DIMY 100
#define DIMZ 100

int NX, NY, NZ;

class chunk {
  public:
    double t[DIMX][DIMY][DIMZ];
    double *sbxm, *sbxp, *sbym, *sbyp, *sbzm, *sbzp;
    int xidx, yidx, zidx;
    int xm, xp, ym, yp, zm, zp;
};

#ifdef AMPI
void chunk_pup(pup_er p, void *d)
{
  chunk **cpp = (chunk **) d;
  if(pup_isUnpacking(p))
    *cpp = new chunk;
  chunk *cp = *cpp;
  pup_doubles(p, &cp->t[0][0][0], (DIMX)*(DIMY)*(DIMZ));
  pup_int(p, &cp->xidx);
  pup_int(p, &cp->yidx);
  pup_int(p, &cp->zidx);
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

static void copyout(double *d, double t[DIMX][DIMY][DIMZ],
                    int sx, int ex, int sy, int ey, int sz, int ez)
{
  int i, j, k;
  int l = 0;
  for(i=sx-1; i<ex; i++)
    for(j=sy-1; j<ey; j++)
      for(k=sz-1; k<ez; k++, l++)
        d[l] = t[i][j][k];
}

static void copyin(double *d, double t[DIMX][DIMY][DIMZ],
                    int sx, int ex, int sy, int ey, int sz, int ez)
{
  int i, j, k;
  int l = 0;
  for(i=sx-1; i<ex; i++)
    for(j=sy-1; j<ey; j++)
      for(k=sz-1; k<ez; k++, l++)
        t[i][j][k] = d[l];
}

int main(int ac, char** av)
{
  int i,j,k,m,cidx;
  int iter, niter, cp_idx;
  MPI_Status status;
  double error, tval, maxerr, tmpmaxerr, starttime, endtime, itertime;
  chunk *cp;
  int thisIndex, ierr, nblocks;
  MPI_Request reqxm, reqxp, reqym, reqyp, reqzm, reqzp;;
  MPI_Status stsxm, stsxp, stsym, stsyp, stszm, stszp;
  MPI_Win win;

  MPI_Init(&ac, &av);
  MPI_Comm_rank(MPI_COMM_WORLD, &thisIndex);
  MPI_Comm_size(MPI_COMM_WORLD, &nblocks);

  if (ac < 4) {
    if (thisIndex == 0)
      printf("Usage: jacobi X Y Z [nIter].\n");
    MPI_Finalize();
    return 1;
  }
  NX = atoi(av[1]);
  NY = atoi(av[2]);
  NZ = atoi(av[3]);
  if (NX*NY*NZ != nblocks) {
    if (thisIndex == 0) 
      printf("%d x %d x %d != %d\n", NX,NY,NZ, nblocks);
    MPI_Finalize();
    return 2;
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

  index3d(thisIndex, cp->xidx, cp->yidx, cp->zidx);
  cp->xp = index1d((cp->xidx+1)%NX,cp->yidx,cp->zidx);
  cp->xm = index1d((cp->xidx+NX-1)%NX,cp->yidx,cp->zidx);
  cp->yp = index1d(cp->xidx,(cp->yidx+1)%NY,cp->zidx);
  cp->ym = index1d(cp->xidx,(cp->yidx+NY-1)%NY,cp->zidx);
  cp->zp = index1d(cp->xidx,cp->yidx,(cp->zidx+1)%NZ);
  cp->zm = index1d(cp->xidx,cp->yidx,(cp->zidx+NZ-1)%NZ);
  for(i=0; i<DIMZ; i++)
    for(j=0; j<DIMY; j++)
      for(k=0; k<DIMX; k++)
        cp->t[k][j][i] = DIMY*DIMX*(i) + DIMX*(j) + (k);
/*
  for(i=0;i<DIMZ;i++){
      printf("Plane %d\n",i);
      for(j=0; j<DIMY; j++){
        for(k=0; k<DIMX; k++)
          printf("%5.2f\t",cp->t[k][j][i]);
        printf("\n");
      }

  }
*/
  MPI_Barrier(MPI_COMM_WORLD);
  starttime = MPI_Wtime();
  MPI_Datatype linevec, planevec;
  MPI_Type_vector(1,DIMX,DIMY*DIMX,MPI_DOUBLE,&linevec);
  MPI_Type_commit(&linevec);
  MPI_Type_vector(1,1,DIMX,MPI_DOUBLE,&planevec);
  MPI_Type_commit(&planevec);

  // The window size is in bytes; disp_unit scales the displacements only.
  MPI_Win_create(cp->t, DIMX*DIMY*DIMZ*sizeof(double), sizeof(double), 0, MPI_COMM_WORLD, &win);

  maxerr = 0.0;
  for(iter=1; iter<=niter; iter++) {
    maxerr = 0.0;
/*
    copyout(cp->sbxm, cp->t, 1, 1, 1, DIMY, 1, DIMZ);
    copyout(cp->sbxp, cp->t, DIMX, DIMX, 1, DIMY, 1, DIMZ);
    copyout(cp->sbym, cp->t, 1, DIMX, 1, 1, 1, DIMZ);
    copyout(cp->sbyp, cp->t, 1, DIMX, DIMY, DIMY, 1, DIMZ);
    copyout(cp->sbzm, cp->t, 1, DIMX, 1, DIMY, 1, 1);
    copyout(cp->sbzp, cp->t, 1, DIMX, 1, DIMY, DIMZ, DIMZ);
*/
    AMPI_Iget(0, DIMY*DIMZ, MPI_DOUBLE, cp->xp, 0, DIMY*DIMZ, MPI_DOUBLE, win, &reqxp);
    AMPI_Iget(0, DIMY*DIMZ, MPI_DOUBLE, cp->xm, (DIMX-1)*DIMY*DIMZ, DIMY*DIMZ, MPI_DOUBLE, win, &reqxm);
    AMPI_Iget(0, DIMX*DIMZ, MPI_DOUBLE, cp->yp, 0, DIMZ, linevec, win, &reqyp);
    AMPI_Iget(0, DIMX*DIMZ, MPI_DOUBLE, cp->yp, (DIMY-1), DIMZ, linevec, win, &reqym);
    AMPI_Iget(0, DIMX*DIMY, MPI_DOUBLE, cp->zp, 0, DIMX*DIMY, planevec, win, &reqzp);
    AMPI_Iget(0, DIMX*DIMY, MPI_DOUBLE, cp->zp, 0, DIMX*DIMY, planevec, win, &reqzm);

    AMPI_Iget_wait(&reqxp, &stsxp, win);
    AMPI_Iget_wait(&reqxm, &stsxm, win);
    AMPI_Iget_wait(&reqyp, &stsyp, win);
    AMPI_Iget_wait(&reqym, &stsym, win);
    AMPI_Iget_wait(&reqzp, &stszp, win);
    AMPI_Iget_wait(&reqzm, &stszm, win);

    AMPI_Iget_data((void*)&cp->sbxp, stsxp);
    AMPI_Iget_data((void*)&cp->sbxm, stsxm);
    AMPI_Iget_data((void*)&cp->sbyp, stsyp);
    AMPI_Iget_data((void*)&cp->sbym, stsym);
    AMPI_Iget_data((void*)&cp->sbzp, stszp);
    AMPI_Iget_data((void*)&cp->sbzm, stszm);


    if(iter > 25 &&  iter < 85 && thisIndex == 35)
      m = 9;
    else
      m = 1;

    for(; m>0; m--){
      for(i=1; i<DIMZ-1; i++)
        for(j=1; j<DIMY-1; j++)
          for(k=1; k<DIMX-1; k++) {
            tval = (cp->t[k][j][i] + cp->t[k][j][i+1] +
                 cp->t[k][j][i-1] + cp->t[k][j+1][i]+ 
                 cp->t[k][j-1][i] + cp->t[k+1][j][i] + cp->t[k-1][j][i])/7.0;
            error = fabs(tval-cp->t[k][j][i]);
            cp->t[k][j][i] = tval;
            if (error > maxerr) maxerr = error;
          }
// Add boundary
//   : k=0 case 
      k=0;
      for(i=1; i<DIMZ-1; i++)
	  for(j=1; j<DIMY-1; j++) {
	      tval = (cp->t[k][j][i] + cp->t[k][j][i+1] +
		    cp->t[k][j][i-1] + cp->t[k][j+1][i]+ 
		    cp->t[k][j-1][i] + cp->t[k+1][j][i] + (cp->sbzp)[j*DIMZ+i])/6.0;
            error = fabs(tval-cp->t[k][j][i]);
            cp->t[k][j][i] = tval;
            if (error > maxerr) maxerr = error;		      
	  }
//   : k=DIMZ-1 case
      k=DIMZ-1;
      for(i=1; i<DIMZ-1; i++)
	  for(j=1; j<DIMY-1; j++) {
	      tval = (cp->t[k][j][i] + cp->t[k][j][i+1] +
		    cp->t[k][j][i-1] + cp->t[k][j+1][i]+ 
		    cp->t[k][j-1][i] + cp->t[k-1][j][i] +
(cp->sbzm)[j*DIMZ+i])/7.0;
            error = fabs(tval-cp->t[k][j][i]);
            cp->t[k][j][i] = tval;
            if (error > maxerr) maxerr = error;		      
	  }
//   : j=0 case
      j=0;
      for(i=1; i<DIMZ-1; i++)
	  for(k=1; k<DIMX-1; k++) {
            tval = (cp->t[k][j][i] + cp->t[k][j][i+1] +
                 cp->t[k][j][i-1] + cp->t[k][j+1][i]+ 
                 (cp->sbyp)[k*DIMZ+i] + cp->t[k+1][j][i] + cp->t[k-1][j][i])/7.0;
            error = fabs(tval-cp->t[k][j][i]);
            cp->t[k][j][i] = tval;
            if (error > maxerr) maxerr = error;		      
	  }
//   : j=DIMY-1 case
      j=DIMY-1;
      for(i=1; i<DIMZ-1; i++)
	  for(k=1; k<DIMX-1; k++) {
            tval = (cp->t[k][j][i] + cp->t[k][j][i+1] +
                 cp->t[k][j][i-1] + (cp->sbym)[k*DIMZ+i]+ 
                 cp->t[k][j-1][i] + cp->t[k+1][j][i] + cp->t[k-1][j][i])/7.0;
            error = fabs(tval-cp->t[k][j][i]);
            cp->t[k][j][i] = tval;
            if (error > maxerr) maxerr = error;		      
	  }
//   : i=0 case
      i=0;
      for(j=1; j<DIMY-1; j++)
	  for(k=1; k<DIMX-1; k++) {
            tval = (cp->t[k][j][i] + cp->t[k][j][i+1] +
                 (cp->sbzp)[k*DIMY+j] + cp->t[k][j+1][i]+ 
                 cp->t[k][j-1][i] + cp->t[k+1][j][i] + cp->t[k-1][j][i])/7.0;
            error = fabs(tval-cp->t[k][j][i]);
            cp->t[k][j][i] = tval;
            if (error > maxerr) maxerr = error;		      
	  }
//   : i=DIMZ-1 case
      i=DIMZ-1;
      for(j=1; j<DIMY-1; j++)
	  for(k=1; k<DIMX-1; k++) {
            tval = (cp->t[k][j][i] +  cp->sbzm[k*DIMY+j] +
                 cp->t[k][j][i-1] + cp->t[k][j+1][i]+ 
                 cp->t[k][j-1][i] + cp->t[k+1][j][i] + cp->t[k-1][j][i])/7.0;
            error = fabs(tval-cp->t[k][j][i]);
            cp->t[k][j][i] = tval;
            if (error > maxerr) maxerr = error;		      
	  }
// corner lines case
//  :i=0, j=0 or DIMY-1 
      i=0; j=0;  
      for(k=1; k<DIMX-1; k++) {
	  tval = (cp->t[k][j][i] +  cp->t[k][j][i+1] +
		  cp->sbzp[k*DIMY+j] + cp->t[k][j+1][i]+ 
		  cp->sbyp[k*DIMZ+i] + cp->t[k+1][j][i] + cp->t[k-1][j][i])/7.0;
	  error = fabs(tval-cp->t[k][j][i]);
	  cp->t[k][j][i] = tval;
	  if (error > maxerr) maxerr = error;		      
      }
      i=0; j=DIMY-1;  
      for(k=1; k<DIMX-1; k++) {
	  tval = (cp->t[k][j][i] +  cp->t[k][j][i+1] +
		  cp->sbzp[k*DIMY+j] + cp->sbym[k*DIMZ+i] + 
		  cp->t[k][j-1][i] + cp->t[k+1][j][i] + cp->t[k-1][j][i])/7.0;
	  error = fabs(tval-cp->t[k][j][i]);
	  cp->t[k][j][i] = tval;
	  if (error > maxerr) maxerr = error;		      
      }
//  :i=DIMZ-1, j=0 or DIMY-1 
      i=DIMZ-1; j=0;  
      for(k=1; k<DIMX-1; k++) {
	  tval = (cp->t[k][j][i] +  cp->t[k][j][i-1] +
		  cp->sbzm[k*DIMY+j] + cp->t[k][j+1][i]+ 
		  cp->sbyp[k*DIMZ+i] + cp->t[k+1][j][i] + cp->t[k-1][j][i])/7.0;
	  error = fabs(tval-cp->t[k][j][i]);
	  cp->t[k][j][i] = tval;
	  if (error > maxerr) maxerr = error;		      
      }
      i=DIMZ-1; j=DIMY-1;  
      for(k=1; k<DIMX-1; k++) {
	  tval = (cp->t[k][j][i] +  cp->t[k][j][i-1] +
		  cp->sbzm[k*DIMY+j] + cp->sbym[k*DIMZ+i] + 
		  cp->t[k][j-1][i] + cp->t[k+1][j][i] + cp->t[k-1][j][i])/7.0;
	  error = fabs(tval-cp->t[k][j][i]);
	  cp->t[k][j][i] = tval;
	  if (error > maxerr) maxerr = error;		      
      }
//  :i=0, k=0 or DIMX-1
      i=0; k=0;
      for(j=1; j<DIMY-1; j++) {
            tval = (cp->t[k][j][i] + cp->sbzp[k*DIMY+j] +
                 cp->t[k][j][i+1] + cp->t[k][j+1][i]+ 
                 cp->t[k][j-1][i] + cp->sbxp[k*DIMX+j] +
cp->t[k+1][j][i])/7.0;
            error = fabs(tval-cp->t[k][j][i]);
            cp->t[k][j][i] = tval;
            if (error > maxerr) maxerr = error;		      
      }
      i=0; k=DIMX-1;
      for(j=1; j<DIMY-1; j++) {
            tval = (cp->t[k][j][i] + cp->sbzp[k*DIMY+j] +
                 cp->t[k][j][i+1] + cp->t[k][j+1][i]+ 
                 cp->t[k][j-1][i] + cp->sbxm[k*DIMY+j] + cp->t[k-1][j][i])/7.0;
            error = fabs(tval-cp->t[k][j][i]);
            cp->t[k][j][i] = tval;
            if (error > maxerr) maxerr = error;		      
      }
//  :i=DIMZ-1, k=0 or DIMX-1
      i=DIMZ-1; k=0;
      for(j=1; j<DIMY-1; j++) {
            tval = (cp->t[k][j][i] + cp->sbzm[k*DIMY+j] +
                 cp->t[k][j][i-1] + cp->t[k][j+1][i]+ 
                 cp->t[k][j-1][i] + cp->sbxp[k*DIMY+j] + cp->t[k+1][j][i])/7.0;
            error = fabs(tval-cp->t[k][j][i]);
            cp->t[k][j][i] = tval;
            if (error > maxerr) maxerr = error;		      
      }
      i=DIMZ-1; k=DIMX-1;
      for(j=1; j<DIMY-1; j++) {
            tval = (cp->t[k][j][i] + cp->sbzm[k*DIMY+j] +
                 cp->t[k][j][i-1] + cp->t[k][j+1][i]+ 
                 cp->t[k][j-1][i] + cp->sbxm[k*DIMY+j] + cp->t[k-1][j][i])/7.0;
            error = fabs(tval-cp->t[k][j][i]);
            cp->t[k][j][i] = tval;
            if (error > maxerr) maxerr = error;		      
      }

//  :j=0 k=0 or DIMX-1, 
      j=0; k=0;	    
      for(i=1; i<DIMZ-1; i++){
	      tval = (cp->t[k][j][i] + cp->t[k][j][i+1] +
                 cp->t[k][j][i-1] + cp->t[k][j+1][i]+ 
                 cp->sbyp[k*DIMZ+i] + cp->t[k+1][j][i] +
cp->sbxp[j*DIMZ+i])/7.0;
            error = fabs(tval-cp->t[k][j][i]);
            cp->t[k][j][i] = tval;
            if (error > maxerr) maxerr = error;		      
	  }
      j=0; k=DIMY-1;	    
      for(i=1; i<DIMZ-1; i++){
	      tval = (cp->t[k][j][i] + cp->t[k][j][i+1] +
                 cp->t[k][j][i-1] + cp->t[k][j+1][i]+ 
                 cp->sbyp[k*DIMZ+i] + cp->t[k-1][j][i] +
cp->sbxm[j*DIMZ+i])/7.0;
            error = fabs(tval-cp->t[k][j][i]);
            cp->t[k][j][i] = tval;
            if (error > maxerr) maxerr = error;		      
	  }

//  :j=DIMY-1, k=0 or DIMX-1 
      j=DIMY-1; k=0;	    
      for(i=1; i<DIMZ-1; i++){
	      tval = (cp->t[k][j][i] + cp->t[k][j][i+1] +
                 cp->t[k][j][i-1] + cp->t[k][j-1][i]+ 
                 cp->sbym[k*DIMZ+i] + cp->t[k+1][j][i] +
cp->sbxp[j*DIMZ+i])/7.0;
            error = fabs(tval-cp->t[k][j][i]);
            cp->t[k][j][i] = tval;
            if (error > maxerr) maxerr = error;		      
	  }
      j=DIMY-1; k=DIMY-1;	    
      for(i=1; i<DIMZ-1; i++){
	      tval = (cp->t[k][j][i] + cp->t[k][j][i+1] +
                 cp->t[k][j][i-1] + cp->t[k][j-1][i]+ 
                 cp->sbym[k*DIMZ+i] + cp->t[k-1][j][i] +
cp->sbxm[j*DIMZ+i])/7.0;
            error = fabs(tval-cp->t[k][j][i]);
            cp->t[k][j][i] = tval;
            if (error > maxerr) maxerr = error;		      
	  }

// corner points case


    }

    AMPI_Iget_free(&reqxp, &stsxp, win);
    AMPI_Iget_free(&reqxm, &stsxm, win);
    AMPI_Iget_free(&reqyp, &stsyp, win);
    AMPI_Iget_free(&reqym, &stsym, win);
    AMPI_Iget_free(&reqzp, &stszp, win);
    AMPI_Iget_free(&reqzm, &stszm, win);

    MPI_Allreduce(&maxerr, &tmpmaxerr, 1, MPI_DOUBLE, MPI_MAX, 
                   MPI_COMM_WORLD);
    maxerr = tmpmaxerr;
    endtime = MPI_Wtime();
    itertime = endtime - starttime;
    double  it;
    MPI_Allreduce(&itertime, &it, 1, MPI_DOUBLE, MPI_SUM,
                   MPI_COMM_WORLD);
    itertime = it/nblocks;
    if (thisIndex == 0)
      printf("iter %d time: %lf maxerr: %lf\n", iter, itertime, maxerr);
    starttime = MPI_Wtime();
#ifdef AMPI
    if(iter%20 == 10) {
      AMPI_Migrate(AMPI_INFO_LB_SYNC);
    }
#endif
  }
  MPI_Win_free(&win);
  MPI_Type_free(&linevec);
  MPI_Type_free(&planevec);
  MPI_Finalize();
  return 0;
}

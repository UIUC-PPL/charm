#include <vector>
#include <algorithm>
#include <tr1/random>
#include "acml.h"
#include <papi.h>
#include <mpi.h>

// Event set for Jaguar
int EVENTS[] = {
  PAPI_FPU_IDL // FPU idle cycles
  , PAPI_RES_STL // resource stalls
  , 0x40004071 // L3_EVICTIONS
  , 0x40002079 // L3_CACHE_MISSES
};
const static int NUM_EVENTS = sizeof(EVENTS)/sizeof(EVENTS[0]);

int blockSize;
int numBlocks;

int getIndex(int i, int j) {
  return i * blockSize + j;
}

struct Events {
  long long l[NUM_EVENTS];
};

//#define GATHER

bool isLive(int liveCores, int rank) {
  return (rank == 0) || (rank % 6) < (liveCores / 2);
}

int main(int argc, char **argv) {
  MPI_Init(&argc, &argv);
  int rank, size;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &size);

  int liveCores;
  int count, iter = 0;
  std::vector<double> peTimes(size);
  std::vector<Events> counts(size);
  int numIter;

  if (argc < 5) {
    printf("usage: blockSize numBlocks numIter liveCores\n");
    exit(1);
  }

  blockSize = atoi(argv[1]);
  numBlocks = atoi(argv[2]);
  numIter = atoi(argv[3]);
  liveCores = atoi(argv[4]);

  if (liveCores > size) {
    printf("Asked for too many working cores\n");
    exit(2);
  }

  if (rank == 0) {
      printf("Live Cores, Iteration, Core, Live, Time, FPU Idle, Resource Stall, L3 Evictions, L3 Misses\n");
      //printf("numBlocks = %d, blockSize = %d, numIter = %d, liveCores = %d\n",
      //       numBlocks, blockSize, numIter, liveCores);
  }

  double **blocks;
  double *U;

  std::tr1::mt19937 eng;
  std::tr1::uniform_real<double> unif(0,1);

  blocks = new double*[numBlocks];
  for (int i = 0; i < numBlocks; i++) {
    blocks[i] = new double[blockSize * blockSize];
    for (int j = 0; j < blockSize*blockSize; ++j)
      blocks[i][j] = unif(eng);
  }
  U = new double[blockSize];
  for (int j = 0; j < blockSize; ++j)
    U[j] = unif(eng);

  long long values[NUM_EVENTS] = { 0 };
  double totalTestTime = 0.0;

  double summedTime = 0.0;
  long long summedCounts[NUM_EVENTS] = { 0 };

  for (int activeCol = 0; activeCol < numIter; ++activeCol) {
    MPI_Barrier(MPI_COMM_WORLD);

    if (PAPI_start_counters(EVENTS, NUM_EVENTS) != PAPI_OK)
      exit(3);
    if (isLive(liveCores, rank)) {
    double startTestTime = MPI_Wtime();
    for (int i = 0; i < numBlocks; i++) {
      double *block = blocks[i];

      int startingRow = 0;
      int offset = 2;

#if 0
      const int cacheLineWidth = 16;
      const int innerUnroll = 1;
      for(int k = activeCol + offset; k < blockSize; k += cacheLineWidth) {
	int row = startingRow * blockSize;
	int limit = std::min(blockSize, k + cacheLineWidth);
	for(int j = startingRow; j < blockSize; j++) {
	  int kk = k;
	  double L = block[row + activeCol];
	  double *bb = block + row + kk;
	  for (; kk < limit && kk % innerUnroll != 0; ++kk)
	    *bb++ -=  L * U[kk - activeCol];
	  for (; kk < limit - innerUnroll + 1; kk += innerUnroll) {
	    *bb++ -=  L * U[kk - activeCol];
	    /*block[row + kk + 1] -=  L * U[kk + 1 - activeCol];
	      block[row + kk + 2] -=  L * U[kk + 2 - activeCol];
	      block[row + kk + 3] -=  L * U[kk + 3 - activeCol];*/
	  }
	  for (; kk < limit; ++kk)
	    *bb++ -=  L * U[kk - activeCol];
	  row += blockSize;
	}
      }
#else
      dger(blockSize-(activeCol+offset), blockSize-startingRow,
	   -1.0,
	   U+offset, 1,
	   &block[getIndex(startingRow,activeCol)], blockSize,
	   &block[getIndex(startingRow,activeCol+offset)], blockSize);
#endif
    totalTestTime = MPI_Wtime() - startTestTime;

    }
    } else { // do dgemms to simulate actual interference
#ifdef DO_DGEMM
      for (int i = 0; i < numBlocks; i += 3)
	dgemm('n', 'n', blockSize, blockSize, blockSize, 1.0, blocks[i%numBlocks], blockSize, blocks[(i+1)%numBlocks], blockSize, 1.0, blocks[(i+2)%numBlocks], blockSize);
#endif
    }
    MPI_Barrier(MPI_COMM_WORLD);
    if (PAPI_stop_counters(values, NUM_EVENTS) != PAPI_OK)
      exit(4);

#ifdef GATHER
    MPI_Gather(&totalTestTime, 1, MPI_DOUBLE, &peTimes[0], 1, MPI_DOUBLE, 0, MPI_COMM_WORLD);
    MPI_Gather(values, NUM_EVENTS, MPI_LONG_LONG_INT, &counts[0], NUM_EVENTS, MPI_LONG_LONG_INT, 0, MPI_COMM_WORLD);
#else
    MPI_Reduce(&totalTestTime, &peTimes[0], 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(values, &counts[0], 2, MPI_LONG_LONG_INT, MPI_SUM, 0, MPI_COMM_WORLD);
    int vi[2];
    vi[0] = values[2];
    vi[1] = values[3];
    MPI_Reduce(rank == 0 ? MPI_IN_PLACE : vi, vi, 2, MPI_LONG_LONG_INT, MPI_MAX, 0, MPI_COMM_WORLD);
    counts[0].l[2] = vi[0];
    counts[0].l[3] = vi[1];
#endif

    if (rank == 0) {
#ifdef GATHER
      for (int i = 0; i < size; ++i) {
	printf("%d, %d, %d, %d, %f", liveCores, activeCol, i, isLive(liveCores, i) ? 1 : 0, peTimes[i]);
	for (int j = 0; j < NUM_EVENTS; ++j)
	  printf(", %lld", counts[i].l[j]);
	printf("\n");
      }
#else
	printf("%d: time = %f, counts = ", activeCol, peTimes[0]/liveCores);
	summedTime += peTimes[0];
	for (int j = 0; j < NUM_EVENTS; ++j) {
	  printf("%lld, ", counts[0].l[j]/liveCores);
	  summedCounts[j] += counts[0].l[j];
	}
	printf("\n");
#endif
    }
  }

#ifndef GATHER
  if (rank == 0) {
    printf("sums: %f, ", summedTime/liveCores);
    for (int j = 0; j < 2; ++j)
      printf("%lld, ", summedCounts[j]/liveCores);
    for (int j = 2; j < 4; ++j)
      printf("%lld, ", summedCounts[j]);
    printf("\n");
  }
#endif

  MPI_Finalize();
  return 0;
}


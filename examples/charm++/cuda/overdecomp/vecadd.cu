#include "hapi.h"
#include <stdio.h>

#define BLOCK_SIZE 256

__device__ unsigned mySmId() {
  unsigned sm_id;

  asm volatile("mov.u32 %0, %%smid;" : "=r"(sm_id));

  return sm_id;
}

__global__ void vecAdd(double* C, double* A, double* B, int n) {
  // Get our global thread ID
  int id = blockIdx.x * blockDim.x + threadIdx.x;

  // Make sure we do not go out of bounds
  if (id < n) {
    C[id] = A[id] + B[id];
  }

  // Print which SM this thread block is running on
  //if (threadIdx.x == 0) printf("Block %d, SM %d\n", blockIdx.x, mySmId());
}

void cudaVecAdd(int n, double* h_A, double* h_B, double* h_C, double* d_A,
                double* d_B, double* d_C, cudaStream_t stream) {
  dim3 block_dim(BLOCK_SIZE);
  dim3 grid_dim((n + block_dim.x - 1) / block_dim.x);

  vecAdd<<<grid_dim, block_dim, 0, stream>>>(d_C, d_A, d_B, n);
  hapiCheck(cudaPeekAtLastError());
}

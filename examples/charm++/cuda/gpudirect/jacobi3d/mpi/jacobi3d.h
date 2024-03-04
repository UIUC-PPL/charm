#ifndef __CUDA_GPUDIRECT_JACOBI3D_H_
#define __CUDA_GPUDIRECT_JACOBI3D_H_

#include <cuda_runtime.h>

#ifdef TEST_CORRECTNESS
typedef int DataType;
#else
typedef double DataType;
#endif

enum Direction { LEFT = 0, RIGHT, TOP, BOTTOM, FRONT, BACK, DIR_COUNT };

#define NDIMS 3
#define IDX(i,j,k) ((block_width+2)*(block_height+2)*(k)+(block_width+2)*(j)+(i))

#define cudaCheck(code) cudaErrorDie(code, #code, __FILE__, __LINE__)
void cudaErrorDie(cudaError_t ret, const char* code, const char* file, int line);

#endif // __CUDA_GPUDIRECT_JACOBI3D_H_

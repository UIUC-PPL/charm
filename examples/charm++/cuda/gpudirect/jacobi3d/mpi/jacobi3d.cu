#include "jacobi3d.h"
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include <algorithm>

#define TILE_SIZE_3D 8
#define TILE_SIZE_2D 16
#define DIVIDEBY7 0.142857

void cudaErrorDie(cudaError_t ret, const char* code, const char* file, int line) {
  if (ret != cudaSuccess) {
    fprintf(stderr, "Fatal CUDA Error [%d] %s at %s:%d\n", ret,
        cudaGetErrorString(ret), file, line);
    exit(-1);
  }
}

__global__ void initKernel(DataType* temperature, int block_width,
    int block_height, int block_depth) {
  int i = blockDim.x*blockIdx.x+threadIdx.x;
  int j = blockDim.y*blockIdx.y+threadIdx.y;
  int k = blockDim.z*blockIdx.z+threadIdx.z;
  if (i < block_width+2 && j < block_height+2 && k < block_depth+2) {
    temperature[IDX(i,j,k)] = 0;
  }
}

__global__ void ghostInitKernel(DataType* ghost, int ghost_count) {
  int i = blockDim.x*blockIdx.x+threadIdx.x;
  if (i < ghost_count) {
    ghost[i] = 0;
  }
}

__global__ void leftBoundaryKernel(DataType* temperature, int block_width,
    int block_height, int block_depth) {
  int j = blockDim.x*blockIdx.x+threadIdx.x;
  int k = blockDim.y*blockIdx.y+threadIdx.y;
  if (j < block_height && k < block_depth) {
    temperature[IDX(0,1+j,1+k)] = 1;
  }
}

__global__ void rightBoundaryKernel(DataType* temperature, int block_width,
    int block_height, int block_depth) {
  int j = blockDim.x*blockIdx.x+threadIdx.x;
  int k = blockDim.y*blockIdx.y+threadIdx.y;
  if (j < block_height && k < block_depth) {
    temperature[IDX(block_width+1,1+j,1+k)] = 1;
  }
}

__global__ void topBoundaryKernel(DataType* temperature, int block_width,
    int block_height, int block_depth) {
  int i = blockDim.x*blockIdx.x+threadIdx.x;
  int k = blockDim.y*blockIdx.y+threadIdx.y;
  if (i < block_width && k < block_depth) {
    temperature[IDX(1+i,0,1+k)] = 1;
  }
}

__global__ void bottomBoundaryKernel(DataType* temperature, int block_width,
    int block_height, int block_depth) {
  int i = blockDim.x*blockIdx.x+threadIdx.x;
  int k = blockDim.y*blockIdx.y+threadIdx.y;
  if (i < block_width && k < block_depth) {
    temperature[IDX(1+i,block_height+1,1+k)] = 1;
  }
}

__global__ void frontBoundaryKernel(DataType* temperature, int block_width,
    int block_height, int block_depth) {
  int i = blockDim.x*blockIdx.x+threadIdx.x;
  int j = blockDim.y*blockIdx.y+threadIdx.y;
  if (i < block_width && j < block_height) {
    temperature[IDX(1+i,1+j,0)] = 1;
  }
}

__global__ void backBoundaryKernel(DataType* temperature, int block_width,
    int block_height, int block_depth) {
  int i = blockDim.x*blockIdx.x+threadIdx.x;
  int j = blockDim.y*blockIdx.y+threadIdx.y;
  if (i < block_width && j < block_height) {
    temperature[IDX(1+i,1+j,block_depth+1)] = 1;
  }
}

__global__ void jacobiKernel(DataType* temperature, DataType* new_temperature,
    int block_width, int block_height, int block_depth) {
  int i = (blockDim.x*blockIdx.x+threadIdx.x)+1;
  int j = (blockDim.y*blockIdx.y+threadIdx.y)+1;
  int k = (blockDim.z*blockIdx.z+threadIdx.z)+1;

  if (i <= block_width && j <= block_height && k <= block_depth) {
#ifdef TEST_CORRECTNESS
    new_temperature[IDX(i,j,k)] = (temperature[IDX(i,j,k)] +
      temperature[IDX(i-1,j,k)] + temperature[IDX(i+1,j,k)] +
      temperature[IDX(i,j-1,k)] + temperature[IDX(i,j+1,k)] +
      temperature[IDX(i,j,k-1)] + temperature[IDX(i,j,k+1)]) % 10000;
#else
    new_temperature[IDX(i,j,k)] = (temperature[IDX(i,j,k)] +
      temperature[IDX(i-1,j,k)] + temperature[IDX(i+1,j,k)] +
      temperature[IDX(i,j-1,k)] + temperature[IDX(i,j+1,k)] +
      temperature[IDX(i,j,k-1)] + temperature[IDX(i,j,k+1)]) * DIVIDEBY7;
#endif
  }
}

__global__ void jacobiInteriorKernel(DataType* temperature, DataType* new_temperature,
    int block_width, int block_height, int block_depth) {
  int i = (blockDim.x*blockIdx.x+threadIdx.x)+2;
  int j = (blockDim.y*blockIdx.y+threadIdx.y)+2;
  int k = (blockDim.z*blockIdx.z+threadIdx.z)+2;

  if (i <= block_width-1 && j <= block_height-1 && k <= block_depth-1) {
#ifdef TEST_CORRECTNESS
    new_temperature[IDX(i,j,k)] = (temperature[IDX(i,j,k)] +
      temperature[IDX(i-1,j,k)] + temperature[IDX(i+1,j,k)] +
      temperature[IDX(i,j-1,k)] + temperature[IDX(i,j+1,k)] +
      temperature[IDX(i,j,k-1)] + temperature[IDX(i,j,k+1)]) % 10000;
#else
    new_temperature[IDX(i,j,k)] = (temperature[IDX(i,j,k)] +
      temperature[IDX(i-1,j,k)] + temperature[IDX(i+1,j,k)] +
      temperature[IDX(i,j-1,k)] + temperature[IDX(i,j+1,k)] +
      temperature[IDX(i,j,k-1)] + temperature[IDX(i,j,k+1)]) * DIVIDEBY7;
#endif
  }
}

__global__ void jacobiBoundaryKernel(DataType* temperature, DataType* new_temperature,
    int block_width, int block_height, int block_depth) {
  int t = blockDim.x*blockIdx.x+threadIdx.x;

  int left_cut = block_height * block_depth;
  int right_cut = block_height * block_depth;
  int top_cut = block_width * block_depth;
  int bottom_cut = block_width * block_depth;
  int front_cut = block_width * block_height;
  int back_cut = block_width * block_height;

  int i, j, k;
  if (t < left_cut) {
    i = 1;
    j = t % block_height + 1;
    k = t / block_height + 1;
#ifdef TEST_CORRECTNESS
    new_temperature[IDX(i,j,k)] = (temperature[IDX(i,j,k)] +
        temperature[IDX(i-1,j,k)] + temperature[IDX(i+1,j,k)] +
        temperature[IDX(i,j-1,k)] + temperature[IDX(i,j+1,k)] +
        temperature[IDX(i,j,k-1)] + temperature[IDX(i,j,k+1)]) % 10000;
#else
    new_temperature[IDX(i,j,k)] = (temperature[IDX(i,j,k)] +
        temperature[IDX(i-1,j,k)] + temperature[IDX(i+1,j,k)] +
        temperature[IDX(i,j-1,k)] + temperature[IDX(i,j+1,k)] +
        temperature[IDX(i,j,k-1)] + temperature[IDX(i,j,k+1)]) * DIVIDEBY7;
#endif
  }
  if (t < right_cut) {
    i = block_width;
    j = t % block_height + 1;
    k = t / block_height + 1;
#ifdef TEST_CORRECTNESS
    new_temperature[IDX(i,j,k)] = (temperature[IDX(i,j,k)] +
        temperature[IDX(i-1,j,k)] + temperature[IDX(i+1,j,k)] +
        temperature[IDX(i,j-1,k)] + temperature[IDX(i,j+1,k)] +
        temperature[IDX(i,j,k-1)] + temperature[IDX(i,j,k+1)]) % 10000;
#else
    new_temperature[IDX(i,j,k)] = (temperature[IDX(i,j,k)] +
        temperature[IDX(i-1,j,k)] + temperature[IDX(i+1,j,k)] +
        temperature[IDX(i,j-1,k)] + temperature[IDX(i,j+1,k)] +
        temperature[IDX(i,j,k-1)] + temperature[IDX(i,j,k+1)]) * DIVIDEBY7;
#endif
  }
  if (t < top_cut) {
    i = t % block_width + 1;
    j = 1;
    k = t / block_width + 1;
#ifdef TEST_CORRECTNESS
    new_temperature[IDX(i,j,k)] = (temperature[IDX(i,j,k)] +
        temperature[IDX(i-1,j,k)] + temperature[IDX(i+1,j,k)] +
        temperature[IDX(i,j-1,k)] + temperature[IDX(i,j+1,k)] +
        temperature[IDX(i,j,k-1)] + temperature[IDX(i,j,k+1)]) % 10000;
#else
    new_temperature[IDX(i,j,k)] = (temperature[IDX(i,j,k)] +
        temperature[IDX(i-1,j,k)] + temperature[IDX(i+1,j,k)] +
        temperature[IDX(i,j-1,k)] + temperature[IDX(i,j+1,k)] +
        temperature[IDX(i,j,k-1)] + temperature[IDX(i,j,k+1)]) * DIVIDEBY7;
#endif
  }
  if (t < bottom_cut) {
    i = t % block_width + 1;
    j = block_height;
    k = t / block_width + 1;
#ifdef TEST_CORRECTNESS
    new_temperature[IDX(i,j,k)] = (temperature[IDX(i,j,k)] +
        temperature[IDX(i-1,j,k)] + temperature[IDX(i+1,j,k)] +
        temperature[IDX(i,j-1,k)] + temperature[IDX(i,j+1,k)] +
        temperature[IDX(i,j,k-1)] + temperature[IDX(i,j,k+1)]) % 10000;
#else
    new_temperature[IDX(i,j,k)] = (temperature[IDX(i,j,k)] +
        temperature[IDX(i-1,j,k)] + temperature[IDX(i+1,j,k)] +
        temperature[IDX(i,j-1,k)] + temperature[IDX(i,j+1,k)] +
        temperature[IDX(i,j,k-1)] + temperature[IDX(i,j,k+1)]) * DIVIDEBY7;
#endif
  }
  if (t < front_cut) {
    i = t % block_width + 1;
    j = t / block_width + 1;
    k = 1;
#ifdef TEST_CORRECTNESS
    new_temperature[IDX(i,j,k)] = (temperature[IDX(i,j,k)] +
        temperature[IDX(i-1,j,k)] + temperature[IDX(i+1,j,k)] +
        temperature[IDX(i,j-1,k)] + temperature[IDX(i,j+1,k)] +
        temperature[IDX(i,j,k-1)] + temperature[IDX(i,j,k+1)]) % 10000;
#else
    new_temperature[IDX(i,j,k)] = (temperature[IDX(i,j,k)] +
        temperature[IDX(i-1,j,k)] + temperature[IDX(i+1,j,k)] +
        temperature[IDX(i,j-1,k)] + temperature[IDX(i,j+1,k)] +
        temperature[IDX(i,j,k-1)] + temperature[IDX(i,j,k+1)]) * DIVIDEBY7;
#endif
  }
  if (t < back_cut) {
    i = t % block_width + 1;
    j = t / block_width + 1;
    k = block_depth;
#ifdef TEST_CORRECTNESS
    new_temperature[IDX(i,j,k)] = (temperature[IDX(i,j,k)] +
        temperature[IDX(i-1,j,k)] + temperature[IDX(i+1,j,k)] +
        temperature[IDX(i,j-1,k)] + temperature[IDX(i,j+1,k)] +
        temperature[IDX(i,j,k-1)] + temperature[IDX(i,j,k+1)]) % 10000;
#else
    new_temperature[IDX(i,j,k)] = (temperature[IDX(i,j,k)] +
        temperature[IDX(i-1,j,k)] + temperature[IDX(i+1,j,k)] +
        temperature[IDX(i,j-1,k)] + temperature[IDX(i,j+1,k)] +
        temperature[IDX(i,j,k-1)] + temperature[IDX(i,j,k+1)]) * DIVIDEBY7;
#endif
  }
}

__global__ void jacobiFusedPackingKernel(DataType* temperature, DataType* new_temperature,
    DataType* left_ghost, DataType* right_ghost, DataType* top_ghost, DataType* bottom_ghost,
    DataType* front_ghost, DataType* back_ghost, bool left_bound, bool right_bound,
    bool top_bound, bool bottom_bound, bool front_bound, bool back_bound,
    int block_width, int block_height, int block_depth) {
  int i = (blockDim.x*blockIdx.x+threadIdx.x)+1;
  int j = (blockDim.y*blockIdx.y+threadIdx.y)+1;
  int k = (blockDim.z*blockIdx.z+threadIdx.z)+1;

  if (i <= block_width && j <= block_height && k <= block_depth) {
    // Interior Jacobi update
#ifdef TEST_CORRECTNESS
    new_temperature[IDX(i,j,k)] = (temperature[IDX(i,j,k)] +
      temperature[IDX(i-1,j,k)] + temperature[IDX(i+1,j,k)] +
      temperature[IDX(i,j-1,k)] + temperature[IDX(i,j+1,k)] +
      temperature[IDX(i,j,k-1)] + temperature[IDX(i,j,k+1)]) % 10000;
#else
    new_temperature[IDX(i,j,k)] = (temperature[IDX(i,j,k)] +
      temperature[IDX(i-1,j,k)] + temperature[IDX(i+1,j,k)] +
      temperature[IDX(i,j-1,k)] + temperature[IDX(i,j+1,k)] +
      temperature[IDX(i,j,k-1)] + temperature[IDX(i,j,k+1)]) * DIVIDEBY7;
#endif

    // Pack ghosts
    if (!left_bound && i == 1) {
      left_ghost[block_height*(k-1)+(j-1)] = new_temperature[IDX(i,j,k)];
    }
    if (!right_bound && i == block_width) {
      right_ghost[block_height*(k-1)+(j-1)] = new_temperature[IDX(i,j,k)];
    }
    if (!top_bound && j == 1) {
      top_ghost[block_width*(k-1)+(i-1)] = new_temperature[IDX(i,j,k)];
    }
    if (!bottom_bound && j == block_height) {
      bottom_ghost[block_width*(k-1)+(i-1)] = new_temperature[IDX(i,j,k)];
    }
    if (!front_bound && k == 1) {
      front_ghost[block_width*(j-1)+(i-1)] = new_temperature[IDX(i,j,k)];
    }
    if (!back_bound && k == block_depth) {
      back_ghost[block_width*(j-1)+(i-1)] = new_temperature[IDX(i,j,k)];
    }
  }
}

__global__ void jacobiFusedAllKernel(DataType* temperature, DataType* new_temperature,
    DataType* send_left_ghost, DataType* send_right_ghost, DataType* send_top_ghost,
    DataType* send_bottom_ghost, DataType* send_front_ghost, DataType* send_back_ghost,
    DataType* recv_left_ghost, DataType* recv_right_ghost, DataType* recv_top_ghost,
    DataType* recv_bottom_ghost, DataType* recv_front_ghost, DataType* recv_back_ghost,
    bool left_bound, bool right_bound, bool top_bound, bool bottom_bound,
    bool front_bound, bool back_bound, int block_width, int block_height, int block_depth) {
  int i = (blockDim.x*blockIdx.x+threadIdx.x)+1;
  int j = (blockDim.y*blockIdx.y+threadIdx.y)+1;
  int k = (blockDim.z*blockIdx.z+threadIdx.z)+1;

  if (i <= block_width && j <= block_height && k <= block_depth) {
    // Unpack ghosts
    if (!left_bound && i == 1) {
      temperature[IDX(i-1,j,k)] = recv_left_ghost[block_height*(k-1)+(j-1)];
    }
    if (!right_bound && i == block_width) {
      temperature[IDX(i+1,j,k)] = recv_right_ghost[block_height*(k-1)+(j-1)];
    }
    if (!top_bound && j == 1) {
      temperature[IDX(i,j-1,k)] = recv_top_ghost[block_width*(k-1)+(i-1)];
    }
    if (!bottom_bound && j == block_height) {
      temperature[IDX(i,j+1,k)] = recv_bottom_ghost[block_width*(k-1)+(i-1)];
    }
    if (!front_bound && k == 1) {
      temperature[IDX(i,j,k-1)] = recv_front_ghost[block_width*(j-1)+(i-1)];
    }
    if (!back_bound && k == block_depth) {
      temperature[IDX(i,j,k+1)] = recv_back_ghost[block_width*(j-1)+(i-1)];
    }

    // Interior Jacobi update
#ifdef TEST_CORRECTNESS
    new_temperature[IDX(i,j,k)] = (temperature[IDX(i,j,k)] +
      temperature[IDX(i-1,j,k)] + temperature[IDX(i+1,j,k)] +
      temperature[IDX(i,j-1,k)] + temperature[IDX(i,j+1,k)] +
      temperature[IDX(i,j,k-1)] + temperature[IDX(i,j,k+1)]) % 10000;
#else
    new_temperature[IDX(i,j,k)] = (temperature[IDX(i,j,k)] +
      temperature[IDX(i-1,j,k)] + temperature[IDX(i+1,j,k)] +
      temperature[IDX(i,j-1,k)] + temperature[IDX(i,j+1,k)] +
      temperature[IDX(i,j,k-1)] + temperature[IDX(i,j,k+1)]) * DIVIDEBY7;
#endif

    // Pack ghosts
    if (!left_bound && i == 1) {
      send_left_ghost[block_height*(k-1)+(j-1)] = new_temperature[IDX(i,j,k)];
    }
    if (!right_bound && i == block_width) {
      send_right_ghost[block_height*(k-1)+(j-1)] = new_temperature[IDX(i,j,k)];
    }
    if (!top_bound && j == 1) {
      send_top_ghost[block_width*(k-1)+(i-1)] = new_temperature[IDX(i,j,k)];
    }
    if (!bottom_bound && j == block_height) {
      send_bottom_ghost[block_width*(k-1)+(i-1)] = new_temperature[IDX(i,j,k)];
    }
    if (!front_bound && k == 1) {
      send_front_ghost[block_width*(j-1)+(i-1)] = new_temperature[IDX(i,j,k)];
    }
    if (!back_bound && k == block_depth) {
      send_back_ghost[block_width*(j-1)+(i-1)] = new_temperature[IDX(i,j,k)];
    }
  }
}

__global__ void leftPackingKernel(DataType* temperature, DataType* ghost,
    int block_width, int block_height, int block_depth) {
  int j = blockDim.x*blockIdx.x+threadIdx.x;
  int k = blockDim.y*blockIdx.y+threadIdx.y;
  if (j < block_height && k < block_depth) {
    ghost[block_height*k+j] = temperature[IDX(1,1+j,1+k)];
  }
}

__global__ void rightPackingKernel(DataType* temperature, DataType* ghost,
    int block_width, int block_height, int block_depth) {
  int j = blockDim.x*blockIdx.x+threadIdx.x;
  int k = blockDim.y*blockIdx.y+threadIdx.y;
  if (j < block_height && k < block_depth) {
    ghost[block_height*k+j] = temperature[IDX(block_width,1+j,1+k)];
  }
}

__global__ void topPackingKernel(DataType* temperature, DataType* ghost,
    int block_width, int block_height, int block_depth) {
  int i = blockDim.x*blockIdx.x+threadIdx.x;
  int k = blockDim.y*blockIdx.y+threadIdx.y;
  if (i < block_width && k < block_depth) {
    ghost[block_width*k+i] = temperature[IDX(1+i,1,1+k)];
  }
}

__global__ void bottomPackingKernel(DataType* temperature, DataType* ghost,
    int block_width, int block_height, int block_depth) {
  int i = blockDim.x*blockIdx.x+threadIdx.x;
  int k = blockDim.y*blockIdx.y+threadIdx.y;
  if (i < block_width && k < block_depth) {
    ghost[block_width*k+i] = temperature[IDX(1+i,block_height,1+k)];
  }
}

__global__ void frontPackingKernel(DataType* temperature, DataType* ghost,
    int block_width, int block_height, int block_depth) {
  int i = blockDim.x*blockIdx.x+threadIdx.x;
  int j = blockDim.y*blockIdx.y+threadIdx.y;
  if (i < block_width && j < block_height) {
    ghost[block_width*j+i] = temperature[IDX(1+i,1+j,1)];
  }
}

__global__ void backPackingKernel(DataType* temperature, DataType* ghost,
    int block_width, int block_height, int block_depth) {
  int i = blockDim.x*blockIdx.x+threadIdx.x;
  int j = blockDim.y*blockIdx.y+threadIdx.y;
  if (i < block_width && j < block_height) {
    ghost[block_width*j+i] = temperature[IDX(1+i,1+j,block_depth)];
  }
}

__global__ void fusedPackingKernelv1(DataType* temperature, DataType* left_ghost,
    DataType* right_ghost, DataType* top_ghost, DataType* bottom_ghost,
    DataType* front_ghost, DataType* back_ghost, bool left_bound, bool right_bound,
    bool top_bound, bool bottom_bound, bool front_bound, bool back_bound,
    int block_width, int block_height, int block_depth) {
  int t = blockDim.x*blockIdx.x+threadIdx.x;

  int left_cut = block_height * block_depth;
  int right_cut = left_cut + block_height * block_depth;
  int top_cut = right_cut + block_width * block_depth;
  int bottom_cut = top_cut + block_width * block_depth;
  int front_cut = bottom_cut + block_width * block_height;
  int back_cut = front_cut + block_width * block_height;

  int my_ghost = -1;
  if (t >= 0 && t < left_cut) {
    my_ghost = LEFT;
  } else if (t >= left_cut && t < right_cut) {
    my_ghost = RIGHT;
    t -= left_cut;
  } else if (t >= right_cut && t < top_cut) {
    my_ghost = TOP;
    t -= right_cut;
  } else if (t >= top_cut && t < bottom_cut) {
    my_ghost = BOTTOM;
    t -= top_cut;
  } else if (t >= bottom_cut && t < front_cut) {
    my_ghost = FRONT;
    t -= bottom_cut;
  } else if (t >= front_cut && t < back_cut) {
    my_ghost = BACK;
    t -= front_cut;
  } else {
    return;
  }

  if (my_ghost == LEFT && !left_bound) {
    int j = t % block_height;
    int k = t / block_height;
    left_ghost[t] = temperature[IDX(1,1+j,1+k)];
  }
  if (my_ghost == RIGHT && !right_bound) {
    int j = t % block_height;
    int k = t / block_height;
    right_ghost[t] = temperature[IDX(block_width,1+j,1+k)];
  }
  if (my_ghost == TOP && !top_bound) {
    int i = t % block_width;
    int k = t / block_width;
    top_ghost[t] = temperature[IDX(1+i,1,1+k)];
  }
  if (my_ghost == BOTTOM && !bottom_bound) {
    int i = t % block_width;
    int k = t / block_width;
    bottom_ghost[t] = temperature[IDX(1+i,block_height,1+k)];
  }
  if (my_ghost == FRONT && !front_bound) {
    int i = t % block_width;
    int j = t / block_width;
    front_ghost[t] = temperature[IDX(1+i,1+j,1)];
  }
  if (my_ghost == BACK && !back_bound) {
    int i = t % block_width;
    int j = t / block_width;
    back_ghost[t] = temperature[IDX(1+i,1+j,block_depth)];
  }
}

__global__ void fusedPackingKernelv2(DataType* temperature, DataType* left_ghost,
    DataType* right_ghost, DataType* top_ghost, DataType* bottom_ghost,
    DataType* front_ghost, DataType* back_ghost, bool left_bound, bool right_bound,
    bool top_bound, bool bottom_bound, bool front_bound, bool back_bound,
    int block_width, int block_height, int block_depth) {
  int t = blockDim.x*blockIdx.x+threadIdx.x;

  int left_cut = block_height * block_depth;
  int right_cut = block_height * block_depth;
  int top_cut = block_width * block_depth;
  int bottom_cut = block_width * block_depth;
  int front_cut = block_width * block_height;
  int back_cut = block_width * block_height;

  if (!left_bound) {
    int j = t % block_height;
    int k = t / block_height;
    if (t < left_cut) {
      left_ghost[t] = temperature[IDX(1,1+j,1+k)];
    }
  }
  if (!right_bound) {
    int j = t % block_height;
    int k = t / block_height;
    if (t < right_cut) {
      right_ghost[t] = temperature[IDX(block_width,1+j,1+k)];
    }
  }
  if (!top_bound) {
    int i = t % block_width;
    int k = t / block_width;
    if (t < top_cut) {
      top_ghost[t] = temperature[IDX(1+i,1,1+k)];
    }
  }
  if (!bottom_bound) {
    int i = t % block_width;
    int k = t / block_width;
    if (t < bottom_cut) {
      bottom_ghost[t] = temperature[IDX(1+i,block_height,1+k)];
    }
  }
  if (!front_bound) {
    int i = t % block_width;
    int j = t / block_width;
    if (t < front_cut) {
      front_ghost[t] = temperature[IDX(1+i,1+j,1)];
    }
  }
  if (!back_bound) {
    int i = t % block_width;
    int j = t / block_width;
    if (t < back_cut) {
      back_ghost[t] = temperature[IDX(1+i,1+j,block_depth)];
    }
  }
}

__global__ void leftUnpackingKernel(DataType* temperature, DataType* ghost,
    int block_width, int block_height, int block_depth) {
  int j = blockDim.x*blockIdx.x+threadIdx.x;
  int k = blockDim.y*blockIdx.y+threadIdx.y;
  if (j < block_height && k < block_depth) {
    temperature[IDX(0,1+j,1+k)] = ghost[block_height*k+j];
  }
}

__global__ void rightUnpackingKernel(DataType* temperature, DataType* ghost,
    int block_width, int block_height, int block_depth) {
  int j = blockDim.x*blockIdx.x+threadIdx.x;
  int k = blockDim.y*blockIdx.y+threadIdx.y;
  if (j < block_height && k < block_depth) {
    temperature[IDX(block_width+1,1+j,1+k)] = ghost[block_height*k+j];
  }
}

__global__ void topUnpackingKernel(DataType* temperature, DataType* ghost,
    int block_width, int block_height, int block_depth) {
  int i = blockDim.x*blockIdx.x+threadIdx.x;
  int k = blockDim.y*blockIdx.y+threadIdx.y;
  if (i < block_width && k < block_depth) {
    temperature[IDX(1+i,0,1+k)] = ghost[block_width*k+i];
  }
}

__global__ void bottomUnpackingKernel(DataType* temperature, DataType* ghost,
    int block_width, int block_height, int block_depth) {
  int i = blockDim.x*blockIdx.x+threadIdx.x;
  int k = blockDim.y*blockIdx.y+threadIdx.y;
  if (i < block_width && k < block_depth) {
    temperature[IDX(1+i,block_height+1,1+k)] = ghost[block_width*k+i];
  }
}

__global__ void frontUnpackingKernel(DataType* temperature, DataType* ghost,
    int block_width, int block_height, int block_depth) {
  int i = blockDim.x*blockIdx.x+threadIdx.x;
  int j = blockDim.y*blockIdx.y+threadIdx.y;
  if (i < block_width && j < block_height) {
    temperature[IDX(1+i,1+j,0)] = ghost[block_width*j+i];
  }
}

__global__ void backUnpackingKernel(DataType* temperature, DataType* ghost,
    int block_width, int block_height, int block_depth) {
  int i = blockDim.x*blockIdx.x+threadIdx.x;
  int j = blockDim.y*blockIdx.y+threadIdx.y;
  if (i < block_width && j < block_height) {
    temperature[IDX(1+i,1+j,block_depth+1)] = ghost[block_width*j+i];
  }
}

__global__ void fusedUnpackingKernelv1(DataType* temperature, DataType* left_ghost,
    DataType* right_ghost, DataType* top_ghost, DataType* bottom_ghost,
    DataType* front_ghost, DataType* back_ghost, bool left_bound, bool right_bound,
    bool top_bound, bool bottom_bound, bool front_bound, bool back_bound,
    int block_width, int block_height, int block_depth) {
  int t = blockDim.x*blockIdx.x+threadIdx.x;

  int left_cut = block_height * block_depth;
  int right_cut = left_cut + block_height * block_depth;
  int top_cut = right_cut + block_width * block_depth;
  int bottom_cut = top_cut + block_width * block_depth;
  int front_cut = bottom_cut + block_width * block_height;
  int back_cut = front_cut + block_width * block_height;

  int my_ghost = -1;
  if (t >= 0 && t < left_cut) {
    my_ghost = LEFT;
  } else if (t >= left_cut && t < right_cut) {
    my_ghost = RIGHT;
    t -= left_cut;
  } else if (t >= right_cut && t < top_cut) {
    my_ghost = TOP;
    t -= right_cut;
  } else if (t >= top_cut && t < bottom_cut) {
    my_ghost = BOTTOM;
    t -= top_cut;
  } else if (t >= bottom_cut && t < front_cut) {
    my_ghost = FRONT;
    t -= bottom_cut;
  } else if (t >= front_cut && t < back_cut) {
    my_ghost = BACK;
    t -= front_cut;
  } else {
    return;
  }

  if (my_ghost == LEFT && !left_bound) {
    int j = t % block_height;
    int k = t / block_height;
    temperature[IDX(0,1+j,1+k)] = left_ghost[t];
  }
  if (my_ghost == RIGHT && !right_bound) {
    int j = t % block_height;
    int k = t / block_height;
    temperature[IDX(block_width+1,1+j,1+k)] = right_ghost[t];
  }
  if (my_ghost == TOP && !top_bound) {
    int i = t % block_width;
    int k = t / block_width;
    temperature[IDX(1+i,0,1+k)] = top_ghost[t];
  }
  if (my_ghost == BOTTOM && !bottom_bound) {
    int i = t % block_width;
    int k = t / block_width;
    temperature[IDX(1+i,block_height+1,1+k)] = bottom_ghost[t];
  }
  if (my_ghost == FRONT && !front_bound) {
    int i = t % block_width;
    int j = t / block_width;
    temperature[IDX(1+i,1+j,0)] = front_ghost[t];
  }
  if (my_ghost == BACK && !back_bound) {
    int i = t % block_width;
    int j = t / block_width;
    temperature[IDX(1+i,1+j,block_depth+1)] = back_ghost[t];
  }
}

__global__ void fusedUnpackingKernelv2(DataType* temperature, DataType* left_ghost,
    DataType* right_ghost, DataType* top_ghost, DataType* bottom_ghost,
    DataType* front_ghost, DataType* back_ghost, bool left_bound, bool right_bound,
    bool top_bound, bool bottom_bound, bool front_bound, bool back_bound,
    int block_width, int block_height, int block_depth) {
  int t = blockDim.x*blockIdx.x+threadIdx.x;

  int left_cut = block_height * block_depth;
  int right_cut = block_height * block_depth;
  int top_cut = block_width * block_depth;
  int bottom_cut = block_width * block_depth;
  int front_cut = block_width * block_height;
  int back_cut = block_width * block_height;

  if (!left_bound) {
    int j = t % block_height;
    int k = t / block_height;
    if (t < left_cut) {
      temperature[IDX(0,1+j,1+k)] = left_ghost[t];
    }
  }
  if (!right_bound) {
    int j = t % block_height;
    int k = t / block_height;
    if (t < right_cut) {
      temperature[IDX(block_width+1,1+j,1+k)] = right_ghost[t];
    }
  }
  if (!top_bound) {
    int i = t % block_width;
    int k = t / block_width;
    if (t < top_cut) {
      temperature[IDX(1+i,0,1+k)] = top_ghost[t];
    }
  }
  if (!bottom_bound) {
    int i = t % block_width;
    int k = t / block_width;
    if (t < bottom_cut) {
      temperature[IDX(1+i,block_height+1,1+k)] = bottom_ghost[t];
    }
  }
  if (!front_bound) {
    int i = t % block_width;
    int j = t / block_width;
    if (t < front_cut) {
      temperature[IDX(1+i,1+j,0)] = front_ghost[t];
    }
  }
  if (!back_bound) {
    int i = t % block_width;
    int j = t / block_width;
    if (t < back_cut) {
      temperature[IDX(1+i,1+j,block_depth+1)] = back_ghost[t];
    }
  }
}

void invokeInitKernel(DataType* temperature, int block_width, int block_height,
    int block_depth, cudaStream_t stream) {
  dim3 block_dim(TILE_SIZE_3D, TILE_SIZE_3D, TILE_SIZE_3D);
  dim3 grid_dim(((block_width+2)+(block_dim.x-1))/block_dim.x,
      ((block_height+2)+(block_dim.y-1))/block_dim.y,
      ((block_depth+2)+(block_dim.z-1))/block_dim.z);

  initKernel<<<grid_dim, block_dim, 0, stream>>>(temperature, block_width,
      block_height, block_depth);
  cudaCheck(cudaPeekAtLastError());
}

void invokeGhostInitKernels(const std::vector<DataType*>& ghosts,
    const std::vector<int>& ghost_counts, cudaStream_t stream) {
  dim3 block_dim(256);
  for (int i = 0; i < ghosts.size(); i++) {
    DataType* ghost = ghosts[i];
    int ghost_count = ghost_counts[i];

    dim3 grid_dim((ghost_count+block_dim.x-1)/block_dim.x);

    ghostInitKernel<<<grid_dim, block_dim, 0, stream>>>(ghost,
        ghost_count);
    cudaCheck(cudaPeekAtLastError());
  }
}

void invokeBoundaryKernels(DataType* temperature, int block_width,
    int block_height, int block_depth, bool bounds[], cudaStream_t stream) {
  dim3 block_dim(TILE_SIZE_2D, TILE_SIZE_2D);

  if (bounds[LEFT]) {
    dim3 grid_dim((block_height+(block_dim.x-1))/block_dim.x,
        (block_depth+(block_dim.y-1))/block_dim.y);
    leftBoundaryKernel<<<grid_dim, block_dim, 0, stream>>>(temperature,
        block_width, block_height, block_depth);
  }
  if (bounds[RIGHT]) {
    dim3 grid_dim((block_height+(block_dim.x-1))/block_dim.x,
        (block_depth+(block_dim.y-1))/block_dim.y);
    rightBoundaryKernel<<<grid_dim, block_dim, 0, stream>>>(temperature,
        block_width, block_height, block_depth);
  }
  if (bounds[TOP]) {
    dim3 grid_dim((block_width+(block_dim.x-1))/block_dim.x,
      (block_depth+(block_dim.y-1))/block_dim.y);
    topBoundaryKernel<<<grid_dim, block_dim, 0, stream>>>(temperature,
        block_width, block_height, block_depth);
  }
  if (bounds[BOTTOM]) {
    dim3 grid_dim((block_width+(block_dim.x-1))/block_dim.x,
      (block_depth+(block_dim.y-1))/block_dim.y);
    bottomBoundaryKernel<<<grid_dim, block_dim, 0, stream>>>(temperature,
        block_width, block_height, block_depth);
  }
  if (bounds[FRONT]) {
    dim3 grid_dim((block_width+(block_dim.x-1))/block_dim.x,
        (block_height+(block_dim.y-1))/block_dim.y);
    frontBoundaryKernel<<<grid_dim, block_dim, 0, stream>>>(temperature,
        block_width, block_height, block_depth);
  }
  if (bounds[BACK]) {
    dim3 grid_dim((block_width+(block_dim.x-1))/block_dim.x,
        (block_height+(block_dim.y-1))/block_dim.y);
    backBoundaryKernel<<<grid_dim, block_dim, 0, stream>>>(temperature,
        block_width, block_height, block_depth);
  }
  cudaCheck(cudaPeekAtLastError());
}

void invokeJacobiKernel(DataType* temperature, DataType* new_temperature,
    DataType* send_left_ghost, DataType* send_right_ghost, DataType* send_top_ghost,
    DataType* send_bottom_ghost, DataType* send_front_ghost, DataType* send_back_ghost,
    DataType* recv_left_ghost, DataType* recv_right_ghost, DataType* recv_top_ghost,
    DataType* recv_bottom_ghost, DataType* recv_front_ghost, DataType* recv_back_ghost,
    bool left_bound, bool right_bound, bool top_bound, bool bottom_bound,
    bool front_bound, bool back_bound, int block_width, int block_height, int block_depth,
    cudaStream_t stream, bool fuse_update_pack, bool fuse_update_all) {
  dim3 block_dim(TILE_SIZE_3D, TILE_SIZE_3D, TILE_SIZE_3D);
  dim3 grid_dim((block_width+(block_dim.x-1))/block_dim.x,
      (block_height+(block_dim.y-1))/block_dim.y,
      (block_depth+(block_dim.z-1))/block_dim.z);

  if (fuse_update_pack) {
    jacobiFusedPackingKernel<<<grid_dim, block_dim, 0, stream>>>(temperature, new_temperature,
        send_left_ghost, send_right_ghost, send_top_ghost, send_bottom_ghost,
        send_front_ghost, send_back_ghost, left_bound, right_bound, top_bound,
        bottom_bound, front_bound, back_bound, block_width, block_height, block_depth);
  } else if (fuse_update_all) {
    jacobiFusedAllKernel<<<grid_dim, block_dim, 0, stream>>>(temperature, new_temperature,
        send_left_ghost, send_right_ghost, send_top_ghost, send_bottom_ghost,
        send_front_ghost, send_back_ghost, recv_left_ghost, recv_right_ghost,
        recv_top_ghost, recv_bottom_ghost, recv_front_ghost, recv_back_ghost,
        left_bound, right_bound, top_bound, bottom_bound, front_bound, back_bound,
        block_width, block_height, block_depth);
  } else {
    jacobiKernel<<<grid_dim, block_dim, 0, stream>>>(temperature, new_temperature,
        block_width, block_height, block_depth);
  }
  cudaCheck(cudaPeekAtLastError());
}

void invokeJacobiInteriorKernel(DataType* temperature, DataType* new_temperature,
    int block_width, int block_height, int block_depth, cudaStream_t stream) {
  if (block_height <= 2 || block_depth <= 2 || block_depth <= 2) return;

  dim3 block_dim(TILE_SIZE_3D, TILE_SIZE_3D, TILE_SIZE_3D);
  dim3 grid_dim(((block_width-2)+(block_dim.x-1))/block_dim.x,
      ((block_height-2)+(block_dim.y-1))/block_dim.y,
      ((block_depth-2)+(block_dim.z-1))/block_dim.z);

  jacobiInteriorKernel<<<grid_dim, block_dim, 0, stream>>>(temperature, new_temperature,
      block_width, block_height, block_depth);
  cudaCheck(cudaPeekAtLastError());
}

void invokeJacobiBoundaryKernel(DataType* temperature, DataType* new_temperature,
    int block_width, int block_height, int block_depth, cudaStream_t stream) {
  int n_elems = std::max({block_height * block_depth,
      block_width * block_depth,
      block_width * block_height});
  dim3 block_dim(256);
  dim3 grid_dim((n_elems+block_dim.x-1)/block_dim.x);

  jacobiBoundaryKernel<<<grid_dim, block_dim, 0, stream>>>(temperature, new_temperature,
      block_width, block_height, block_depth);
  cudaCheck(cudaPeekAtLastError());
}

void packGhostsDevice(DataType* temperature,
    DataType* d_ghosts[], DataType* h_ghosts[], bool bounds[],
    int block_width, int block_height, int block_depth,
    size_t x_surf_size, size_t y_surf_size, size_t z_surf_size,
    cudaStream_t comm_stream, cudaStream_t d2h_stream, cudaEvent_t pack_events[],
    bool cuda_aware) {
  dim3 block_dim(TILE_SIZE_2D, TILE_SIZE_2D);
  if (!bounds[LEFT]) {
    dim3 grid_dim((block_height+(block_dim.x-1))/block_dim.x,
        (block_depth+(block_dim.y-1))/block_dim.y);
    leftPackingKernel<<<grid_dim, block_dim, 0, comm_stream>>>(temperature,
        d_ghosts[LEFT], block_width, block_height, block_depth);
    if (!cuda_aware) {
      cudaEventRecord(pack_events[LEFT], comm_stream);
      cudaStreamWaitEvent(d2h_stream, pack_events[LEFT], 0);
      cudaMemcpyAsync(h_ghosts[LEFT], d_ghosts[LEFT], x_surf_size,
          cudaMemcpyDeviceToHost, d2h_stream);
    }
  }
  if (!bounds[RIGHT]) {
    dim3 grid_dim((block_height+(block_dim.x-1))/block_dim.x,
        (block_depth+(block_dim.y-1))/block_dim.y);
    rightPackingKernel<<<grid_dim, block_dim, 0, comm_stream>>>(temperature,
        d_ghosts[RIGHT], block_width, block_height, block_depth);
    if (!cuda_aware) {
      cudaEventRecord(pack_events[RIGHT], comm_stream);
      cudaStreamWaitEvent(d2h_stream, pack_events[RIGHT], 0);
      cudaMemcpyAsync(h_ghosts[RIGHT], d_ghosts[RIGHT], x_surf_size,
          cudaMemcpyDeviceToHost, d2h_stream);
    }
  }
  if (!bounds[TOP]) {
    dim3 grid_dim((block_width+(block_dim.x-1))/block_dim.x,
        (block_depth+(block_dim.y-1))/block_dim.y);
    topPackingKernel<<<grid_dim, block_dim, 0, comm_stream>>>(temperature,
        d_ghosts[TOP], block_width, block_height, block_depth);
    if (!cuda_aware) {
      cudaEventRecord(pack_events[TOP], comm_stream);
      cudaStreamWaitEvent(d2h_stream, pack_events[TOP], 0);
      cudaMemcpyAsync(h_ghosts[TOP], d_ghosts[TOP], y_surf_size,
          cudaMemcpyDeviceToHost, d2h_stream);
    }
  }
  if (!bounds[BOTTOM]) {
    dim3 grid_dim((block_width+(block_dim.x-1))/block_dim.x,
        (block_depth+(block_dim.y-1))/block_dim.y);
    bottomPackingKernel<<<grid_dim, block_dim, 0, comm_stream>>>(temperature,
        d_ghosts[BOTTOM], block_width, block_height, block_depth);
    if (!cuda_aware) {
      cudaEventRecord(pack_events[BOTTOM], comm_stream);
      cudaStreamWaitEvent(d2h_stream, pack_events[BOTTOM], 0);
      cudaMemcpyAsync(h_ghosts[BOTTOM], d_ghosts[BOTTOM], y_surf_size,
          cudaMemcpyDeviceToHost, d2h_stream);
    }
  }
  if (!bounds[FRONT]) {
    dim3 grid_dim((block_width+(block_dim.x-1))/block_dim.x,
        (block_height+(block_dim.y-1))/block_dim.y);
    frontPackingKernel<<<grid_dim, block_dim, 0, comm_stream>>>(temperature,
        d_ghosts[FRONT], block_width, block_height, block_depth);
    if (!cuda_aware) {
      cudaEventRecord(pack_events[FRONT], comm_stream);
      cudaStreamWaitEvent(d2h_stream, pack_events[FRONT], 0);
      cudaMemcpyAsync(h_ghosts[FRONT], d_ghosts[FRONT], z_surf_size,
          cudaMemcpyDeviceToHost, d2h_stream);
    }
  }
  if (!bounds[BACK]) {
    dim3 grid_dim((block_width+(block_dim.x-1))/block_dim.x,
        (block_height+(block_dim.y-1))/block_dim.y);
    backPackingKernel<<<grid_dim, block_dim, 0, comm_stream>>>(temperature,
        d_ghosts[BACK], block_width, block_height, block_depth);
    if (!cuda_aware) {
      cudaEventRecord(pack_events[BACK], comm_stream);
      cudaStreamWaitEvent(d2h_stream, pack_events[BACK], 0);
      cudaMemcpyAsync(h_ghosts[BACK], d_ghosts[BACK], z_surf_size,
          cudaMemcpyDeviceToHost, d2h_stream);
    }
  }
  cudaCheck(cudaPeekAtLastError());
}

#define PACK_FUSE_VER 2

void packGhostsFusedDevice(DataType* temperature, DataType* left_ghost,
    DataType* right_ghost, DataType* top_ghost, DataType* bottom_ghost,
    DataType* front_ghost, DataType* back_ghost, bool left_bound, bool right_bound,
    bool top_bound, bool bottom_bound, bool front_bound, bool back_bound,
    int block_width, int block_height, int block_depth, cudaStream_t comm_stream) {
#if PACK_FUSE_VER == 1
  int n_elems = 2*(block_height*block_depth+block_width*block_depth+block_width*block_height);
#elif PACK_FUSE_VER == 2
  int n_elems = std::max({block_height*block_depth,block_width*block_depth,block_width*block_height});
#endif
  dim3 block_dim(256);
  dim3 grid_dim((n_elems+block_dim.x-1)/block_dim.x);
#if PACK_FUSE_VER == 1
  fusedPackingKernelv1<<<grid_dim, block_dim, 0, comm_stream>>>(temperature,
#elif PACK_FUSE_VER == 2
  fusedPackingKernelv2<<<grid_dim, block_dim, 0, comm_stream>>>(temperature,
#endif
      left_ghost, right_ghost, top_ghost, bottom_ghost, front_ghost, back_ghost,
      left_bound, right_bound, top_bound, bottom_bound, front_bound, back_bound,
      block_width, block_height, block_depth);
  cudaCheck(cudaPeekAtLastError());
}

void unpackGhostDevice(DataType* temperature, DataType* d_ghost, DataType* h_ghost,
    int dir, int block_width, int block_height, int block_depth, size_t ghost_size,
    cudaStream_t comm_stream, cudaStream_t h2d_stream, cudaEvent_t unpack_events[],
    bool cuda_aware) {
  if (!cuda_aware) {
    cudaMemcpyAsync(d_ghost, h_ghost, ghost_size, cudaMemcpyHostToDevice,
        h2d_stream);
    cudaEventRecord(unpack_events[dir], h2d_stream);
    cudaStreamWaitEvent(comm_stream, unpack_events[dir], 0);
  }

  dim3 block_dim(TILE_SIZE_2D, TILE_SIZE_2D);
  if (dir == LEFT) {
    dim3 grid_dim((block_height+(block_dim.x-1))/block_dim.x,
        (block_depth+(block_dim.y-1))/block_dim.y);
    leftUnpackingKernel<<<grid_dim, block_dim, 0, comm_stream>>>(temperature,
        d_ghost, block_width, block_height, block_depth);
  } else if (dir == RIGHT) {
    dim3 grid_dim((block_height+(block_dim.x-1))/block_dim.x,
        (block_depth+(block_dim.y-1))/block_dim.y);
    rightUnpackingKernel<<<grid_dim, block_dim, 0, comm_stream>>>(temperature,
        d_ghost, block_width, block_height, block_depth);
  } else if (dir == TOP) {
    dim3 grid_dim((block_width+(block_dim.x-1))/block_dim.x,
        (block_depth+(block_dim.y-1))/block_dim.y);
    topUnpackingKernel<<<grid_dim, block_dim, 0, comm_stream>>>(temperature,
        d_ghost, block_width, block_height, block_depth);
  } else if (dir == BOTTOM) {
    dim3 grid_dim((block_width+(block_dim.x-1))/block_dim.x,
        (block_depth+(block_dim.y-1))/block_dim.y);
    bottomUnpackingKernel<<<grid_dim, block_dim, 0, comm_stream>>>(temperature,
        d_ghost, block_width, block_height, block_depth);
  } else if (dir == FRONT) {
    dim3 grid_dim((block_width+(block_dim.x-1))/block_dim.x,
        (block_height+(block_dim.y-1))/block_dim.y);
    frontUnpackingKernel<<<grid_dim, block_dim, 0, comm_stream>>>(temperature,
        d_ghost, block_width, block_height, block_depth);
  } else if (dir == BACK) {
    dim3 grid_dim((block_width+(block_dim.x-1))/block_dim.x,
        (block_height+(block_dim.y-1))/block_dim.y);
    backUnpackingKernel<<<grid_dim, block_dim, 0, comm_stream>>>(temperature,
        d_ghost, block_width, block_height, block_depth);
  }
  cudaCheck(cudaPeekAtLastError());
}

#define UNPACK_FUSE_VER 2

void unpackGhostsFusedDevice(DataType* temperature, DataType* left_ghost,
    DataType* right_ghost, DataType* top_ghost, DataType* bottom_ghost,
    DataType* front_ghost, DataType* back_ghost, bool left_bound, bool right_bound,
    bool top_bound, bool bottom_bound, bool front_bound, bool back_bound,
    int block_width, int block_height, int block_depth, cudaStream_t comm_stream) {
#if UNPACK_FUSE_VER == 1
  int n_elems = 2*(block_height*block_depth+block_width*block_depth+block_width*block_height);
#elif UNPACK_FUSE_VER == 2
  int n_elems = std::max({block_height*block_depth,block_width*block_depth,block_width*block_height});
#endif
  dim3 block_dim(256);
  dim3 grid_dim((n_elems+block_dim.x-1)/block_dim.x);
#if UNPACK_FUSE_VER == 1
  fusedUnpackingKernelv1<<<grid_dim, block_dim, 0, comm_stream>>>(temperature,
#elif UNPACK_FUSE_VER == 2
  fusedUnpackingKernelv2<<<grid_dim, block_dim, 0, comm_stream>>>(temperature,
#endif
      left_ghost, right_ghost, top_ghost, bottom_ghost, front_ghost, back_ghost,
      left_bound, right_bound, top_bound, bottom_bound, front_bound, back_bound,
      block_width, block_height, block_depth);
  cudaCheck(cudaPeekAtLastError());
}

void setUnpackNode(cudaKernelNodeParams& params, DataType* temperature,
  DataType* ghost, int dir, int block_width, int block_height, int block_depth) {
  dim3 block_dim(TILE_SIZE_2D, TILE_SIZE_2D);
  dim3 grid_dim;
  if (dir == LEFT) {
    grid_dim = dim3((block_height+(block_dim.x-1))/block_dim.x,
        (block_depth+(block_dim.y-1))/block_dim.y);
    params.func = (void*)leftUnpackingKernel;
  } else if (dir == RIGHT) {
    grid_dim = dim3((block_height+(block_dim.x-1))/block_dim.x,
        (block_depth+(block_dim.y-1))/block_dim.y);
    params.func = (void*)rightUnpackingKernel;
  } else if (dir == TOP) {
    grid_dim = dim3((block_width+(block_dim.x-1))/block_dim.x,
        (block_depth+(block_dim.y-1))/block_dim.y);
    params.func = (void*)topUnpackingKernel;
  } else if (dir == BOTTOM) {
    grid_dim = dim3((block_width+(block_dim.x-1))/block_dim.x,
        (block_depth+(block_dim.y-1))/block_dim.y);
    params.func = (void*)bottomUnpackingKernel;
  } else if (dir == FRONT) {
    grid_dim = dim3((block_width+(block_dim.x-1))/block_dim.x,
        (block_height+(block_dim.y-1))/block_dim.y);
    params.func = (void*)frontUnpackingKernel;
  } else if (dir == BACK) {
    grid_dim = dim3((block_width+(block_dim.x-1))/block_dim.x,
        (block_height+(block_dim.y-1))/block_dim.y);
    params.func = (void*)backUnpackingKernel;
  }

  void* kernel_args[] = {(void*)&temperature, (void*)&ghost, &block_width,
    &block_height, &block_depth};

  params.blockDim = block_dim;
  params.gridDim = grid_dim;
  params.sharedMemBytes = 0;
  params.kernelParams = kernel_args;
  params.extra = NULL;
}

void setUnpackFusedNode(cudaKernelNodeParams& params, DataType* temperature,
    DataType* ghosts[], bool bounds[], int block_width, int block_height,
    int block_depth) {
#if UNPACK_FUSE_VER == 1
  int n_elems = 2*(block_height*block_depth+block_width*block_depth+block_width*block_height);
#elif UNPACK_FUSE_VER == 2
  int n_elems = std::max({block_height*block_depth,block_width*block_depth,block_width*block_height});
#endif
  dim3 block_dim(256);
  dim3 grid_dim((n_elems+block_dim.x-1)/block_dim.x);
  params.blockDim = block_dim;
  params.gridDim = grid_dim;
#if UNPACK_FUSE_VER == 1
  params.func = (void*)fusedUnpackingKernelv1;
#elif UNPACK_FUSE_VER == 2
  params.func = (void*)fusedUnpackingKernelv2;
#endif

  void* kernel_args[] = {(void*)&temperature, (void*)&ghosts[LEFT],
    (void*)&ghosts[RIGHT], (void*)&ghosts[TOP], (void*)&ghosts[BOTTOM],
    (void*)&ghosts[FRONT], (void*)&ghosts[BACK], &bounds[LEFT], &bounds[RIGHT],
    &bounds[TOP], &bounds[BOTTOM], &bounds[FRONT], &bounds[BACK], &block_width,
    &block_height, &block_depth};

  params.sharedMemBytes = 0;
  params.kernelParams = kernel_args;
  params.extra = NULL;
}

void setUpdateNode(cudaKernelNodeParams& params, DataType* temperature,
    DataType* new_temperature, DataType* send_ghosts[], DataType* recv_ghosts[],
    bool bounds[], int block_width, int block_height, int block_depth,
    bool fuse_update_pack, bool fuse_update_all) {
  dim3 block_dim(TILE_SIZE_3D, TILE_SIZE_3D, TILE_SIZE_3D);
  dim3 grid_dim((block_width+(block_dim.x-1))/block_dim.x,
      (block_height+(block_dim.y-1))/block_dim.y,
      (block_depth+(block_dim.z-1))/block_dim.z);
  params.blockDim = block_dim;
  params.gridDim = grid_dim;

  if (fuse_update_pack) {
    params.func = (void*)jacobiFusedPackingKernel;
    void* kernel_args[] = {(void*)&temperature, (void*)&new_temperature,
      (void*)&send_ghosts[LEFT], (void*)&send_ghosts[RIGHT], (void*)&send_ghosts[TOP],
      (void*)&send_ghosts[BOTTOM], (void*)&send_ghosts[FRONT], (void*)&send_ghosts[BACK],
      &bounds[LEFT], &bounds[RIGHT], &bounds[TOP], &bounds[BOTTOM], &bounds[FRONT],
      &bounds[BACK], &block_width, &block_height, &block_depth};
    params.kernelParams = kernel_args;
  } else if (fuse_update_all) {
    params.func = (void*)jacobiFusedAllKernel;
    void* kernel_args[] = {(void*)&temperature, (void*)&new_temperature,
      (void*)&send_ghosts[LEFT], (void*)&send_ghosts[RIGHT], (void*)&send_ghosts[TOP],
      (void*)&send_ghosts[BOTTOM], (void*)&send_ghosts[FRONT], (void*)&send_ghosts[BACK],
      (void*)&recv_ghosts[LEFT], (void*)&recv_ghosts[RIGHT], (void*)&recv_ghosts[TOP],
      (void*)&recv_ghosts[BOTTOM], (void*)&recv_ghosts[FRONT], (void*)&recv_ghosts[BACK],
      &bounds[LEFT], &bounds[RIGHT], &bounds[TOP], &bounds[BOTTOM], &bounds[FRONT],
      &bounds[BACK], &block_width, &block_height, &block_depth};
    params.kernelParams = kernel_args;
  } else {
    params.func = (void*)jacobiKernel;
    void* kernel_args[] = {(void*)&temperature, (void*)&new_temperature,
      &block_width, &block_height, &block_depth};
    params.kernelParams = kernel_args;
  }

  params.sharedMemBytes = 0;
  params.extra = NULL;
}

void setPackNode(cudaKernelNodeParams& params, DataType* temperature,
  DataType* ghost, int dir, int block_width, int block_height, int block_depth) {
  dim3 block_dim(TILE_SIZE_2D, TILE_SIZE_2D);
  dim3 grid_dim;
  if (dir == LEFT) {
    grid_dim = dim3((block_height+(block_dim.x-1))/block_dim.x,
        (block_depth+(block_dim.y-1))/block_dim.y);
    params.func = (void*)leftPackingKernel;
  } else if (dir == RIGHT) {
    grid_dim = dim3((block_height+(block_dim.x-1))/block_dim.x,
        (block_depth+(block_dim.y-1))/block_dim.y);
    params.func = (void*)rightPackingKernel;
  } else if (dir == TOP) {
    grid_dim = dim3((block_width+(block_dim.x-1))/block_dim.x,
        (block_depth+(block_dim.y-1))/block_dim.y);
    params.func = (void*)topPackingKernel;
  } else if (dir == BOTTOM) {
    grid_dim = dim3((block_width+(block_dim.x-1))/block_dim.x,
        (block_depth+(block_dim.y-1))/block_dim.y);
    params.func = (void*)bottomPackingKernel;
  } else if (dir == FRONT) {
    grid_dim = dim3((block_width+(block_dim.x-1))/block_dim.x,
        (block_height+(block_dim.y-1))/block_dim.y);
    params.func = (void*)frontPackingKernel;
  } else if (dir == BACK) {
    grid_dim = dim3((block_width+(block_dim.x-1))/block_dim.x,
        (block_height+(block_dim.y-1))/block_dim.y);
    params.func = (void*)backPackingKernel;
  }

  void* kernel_args[] = {(void*)&temperature, (void*)&ghost, &block_width,
    &block_height, &block_depth};

  params.blockDim = block_dim;
  params.gridDim = grid_dim;
  params.sharedMemBytes = 0;
  params.kernelParams = kernel_args;
  params.extra = NULL;
}

void setPackFusedNode(cudaKernelNodeParams& params, DataType* temperature,
    DataType* ghosts[], bool bounds[], int block_width, int block_height,
    int block_depth) {
#if PACK_FUSE_VER == 1
  int n_elems = 2*(block_height*block_depth+block_width*block_depth+block_width*block_height);
#elif PACK_FUSE_VER == 2
  int n_elems = std::max({block_height*block_depth,block_width*block_depth,block_width*block_height});
#endif
  dim3 block_dim(256);
  dim3 grid_dim((n_elems+block_dim.x-1)/block_dim.x);
  params.blockDim = block_dim;
  params.gridDim = grid_dim;
#if PACK_FUSE_VER == 1
  params.func = (void*)fusedPackingKernelv1;
#elif PACK_FUSE_VER == 2
  params.func = (void*)fusedPackingKernelv2;
#endif

  void* kernel_args[] = {(void*)&temperature, (void*)&ghosts[LEFT],
    (void*)&ghosts[RIGHT], (void*)&ghosts[TOP], (void*)&ghosts[BOTTOM],
    (void*)&ghosts[FRONT], (void*)&ghosts[BACK], &bounds[LEFT], &bounds[RIGHT],
    &bounds[TOP], &bounds[BOTTOM], &bounds[FRONT], &bounds[BACK], &block_width,
    &block_height, &block_depth};

  params.sharedMemBytes = 0;
  params.kernelParams = kernel_args;
  params.extra = NULL;
}

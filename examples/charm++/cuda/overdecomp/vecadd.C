#include "vecadd.decl.h"
#include "hapi.h"
#ifdef USE_NVTX
#include "hapi_nvtx.h"
#endif

/* readonly */ CProxy_Main mainProxy;
/* readonly */ int vector_size;
/* readonly */ int split;
/* readonly */ int chunk;

extern void cudaVecAdd(int, double*, double*, double*, double*, double*, double*,
                       cudaStream_t);

class Main : public CBase_Main {
 private:
  CProxy_Chunk chunks;
  double program_start_time;
  double kernel_start_time;

 public:
  Main(CkArgMsg* m) {
#ifdef USE_NVTX
    NVTXTracer nvtx_range("Main::Main", NVTXColor::Turquoise);
#endif

    mainProxy = thisProxy;
    vector_size = 1024;
    split = 1; // Equal to number of chares

    // Process command line arguments
    int c;
    while ((c = getopt(m->argc, m->argv, "n:s:")) != -1) {
      switch (c) {
        case 'n':
          vector_size = atoi(optarg);
          break;
        case 's':
          split = atoi(optarg);
          break;
        default:
          CkPrintf("Usage: %s -n [vector size] -s [split]\n", m->argv[0]);
          CkExit();
      }
    }
    delete m;

    // Data size per PE
    chunk = vector_size / split;

    CkPrintf("vector size: %d, split: %d, chunk: %d\n", vector_size, split, chunk);

    if (vector_size % split != 0) {
      CkAbort("Vector size should be divisible by split");
    }

    program_start_time = CkWallTimer();

    // Create chunk chares and initiate H2D data transfers
    chunks = CProxy_Chunk::ckNew(split);
    chunks.h2d();
  }

  void h2d_complete() {
#ifdef USE_NVTX
    NVTXTracer nvtx_range("Main::h2d_complete", NVTXColor::Turquoise);
#endif

    kernel_start_time = CkWallTimer();

    chunks.kernel();
  }

  void kernel_complete() {
#ifdef USE_NVTX
    NVTXTracer nvtx_range("Main::kernel_complete", NVTXColor::Turquoise);
#endif

    CkPrintf("\nKernel time: %.6lf s\n", CkWallTimer() - kernel_start_time);

    chunks.d2h();
  }

  void d2h_complete() {
#ifdef USE_NVTX
    NVTXTracer nvtx_range("Main::d2h_complete", NVTXColor::Turquoise);
#endif

    CkPrintf("Program time: %.6lf s\n", CkWallTimer() - program_start_time);
    CkExit();
  }
};

class Chunk : public CBase_Chunk {
 private:
  double* h_A;
  double* h_B;
  double* h_C;
  double* d_A;
  double* d_B;
  double* d_C;
  size_t size;
  cudaStream_t stream;

 public:
  Chunk() {
#ifdef USE_NVTX
    NVTXTracer nvtx_range("Chunk::Chunk", NVTXColor::WetAsphalt);
#endif

    size = chunk * sizeof(double);
    hapiCheck(cudaMallocHost(&h_A, size));
    hapiCheck(cudaMallocHost(&h_B, size));
    hapiCheck(cudaMallocHost(&h_C, size));
    hapiCheck(cudaMalloc(&d_A, size));
    hapiCheck(cudaMalloc(&d_B, size));
    hapiCheck(cudaMalloc(&d_C, size));
    hapiCheck(cudaStreamCreate(&stream));

    for (int i = 0; i < chunk; i++) {
      h_A[i] = (double)i;
      h_B[i] = (double)i;
    }
  }

  ~Chunk() {
#ifdef USE_NVTX
    NVTXTracer nvtx_range("Chunk::~Chunk", NVTXColor::WetAsphalt);
#endif

    hapiCheck(cudaFreeHost(h_A));
    hapiCheck(cudaFreeHost(h_B));
    hapiCheck(cudaFreeHost(h_C));
    hapiCheck(cudaFree(d_A));
    hapiCheck(cudaFree(d_B));
    hapiCheck(cudaFree(d_C));
    hapiCheck(cudaStreamDestroy(stream));
  }

  void h2d() {
#ifdef USE_NVTX
    NVTXTracer nvtx_range("Chunk::h2d", NVTXColor::Carrot);
#endif

    // Copy input vectors to device
    hapiCheck(cudaMemcpyAsync(d_A, h_A, size, cudaMemcpyHostToDevice, stream));
    hapiCheck(cudaMemcpyAsync(d_B, h_B, size, cudaMemcpyHostToDevice, stream));

    // Set up callback
    CkCallback* cb = new CkCallback(CkIndex_Chunk::h2d_done(), thisProxy[thisIndex]);
    hapiAddCallback(stream, cb);
  }

  void h2d_done() {
#ifdef USE_NVTX
    NVTXTracer nvtx_range("Chunk::h2d_done", NVTXColor::Clouds);
#endif

    // Synchronize
    contribute(CkCallback(CkReductionTarget(Main, h2d_complete), mainProxy));
  }

  void kernel() {
#ifdef USE_NVTX
    NVTXTracer nvtx_range("Chunk::kernel", NVTXColor::Carrot);
#endif

    // Invoke kernel
    cudaVecAdd(chunk, h_A, h_B, h_C, d_A, d_B, d_C, stream);

    // Set up callback
    CkCallback* cb = new CkCallback(CkIndex_Chunk::kernel_done(), thisProxy[thisIndex]);
    hapiAddCallback(stream, cb);
  }

  void kernel_done() {
#ifdef USE_NVTX
    NVTXTracer nvtx_range("Chunk::kernel_done", NVTXColor::Clouds);
#endif

    // Synchronize
    contribute(CkCallback(CkReductionTarget(Main, kernel_complete), mainProxy));
  }

  void d2h() {
#ifdef USE_NVTX
    NVTXTracer nvtx_range("Chunk::d2h", NVTXColor::Carrot);
#endif

    // Copy output vector to host
    hapiCheck(cudaMemcpyAsync(h_C, d_C, size, cudaMemcpyDeviceToHost, stream));

    // Set up callback
    CkCallback* cb = new CkCallback(CkIndex_Chunk::d2h_done(), thisProxy[thisIndex]);
    hapiAddCallback(stream, cb);
  }

  void d2h_done() {
#ifdef USE_NVTX
    NVTXTracer nvtx_range("Chunk::d2h_done", NVTXColor::Clouds);
#endif

    contribute(CkCallback(CkReductionTarget(Main, d2h_complete), mainProxy));
  }
};

#include "vecadd.def.h"

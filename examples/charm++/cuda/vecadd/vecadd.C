#include <time.h>
#include "vecadd.decl.h"
#include "hapi.h"
#ifdef USE_NVTX
#include "hapi_nvtx.h"
#endif

/* readonly */ CProxy_Main mainProxy;
/* readonly */ int vectorSize;
/* readonly */ int numChares;
/* readonly */ int numIters;
/* readonly */ int numWarmups;
/* readonly */ bool cudaSync;
/* readonly */ bool noMemcpy;

#ifdef USE_WR
extern void cudaVecAdd(int, float*, float*, float*, cudaStream_t, void*);
#else
extern void cudaVecAdd(int, float*, float*, float*, float*, float*, float*,
                       cudaStream_t, void*);
#endif

void randomInit(float* data, int size) {
#ifdef USE_NVTX
  NVTXTracer nvtx_range("randomInit", NVTXColor::PeterRiver);
#endif
  for (int i = 0; i < size; ++i) {
    data[i] = rand() / (float)RAND_MAX;
  }
}

class Main : public CBase_Main {
 private:
  CProxy_Workers workers;
  double startTime;

 public:
  Main(CkArgMsg* m) {
#ifdef USE_NVTX
    NVTXTracer nvtx_range("Main::Main", NVTXColor::Turquoise);
#endif

    // default values
    mainProxy = thisProxy;
    vectorSize = 1024;
    numChares = 4;
    numIters = 100;
    numWarmups = 10;
    cudaSync = false;
    noMemcpy = false;

    // handle arguments
    int c;
    while ((c = getopt(m->argc, m->argv, "c:s:i:w:yn")) != -1) {
      switch (c) {
        case 'c':
          numChares = atoi(optarg);
          break;
        case 's':
          vectorSize = atoi(optarg);
          break;
        case 'i':
          numIters = atoi(optarg);
          break;
        case 'w':
          numWarmups = atoi(optarg);
          break;
        case 'y':
          cudaSync = true;
          break;
        case 'n':
          noMemcpy = true;
          break;
        default:
          CkPrintf("Usage: %s -c [chares] -s [vector size] -i [iterations] "
              "-w [warmups] -y [CUDA sync] -n [no memcpys]\n",
              m->argv[0]);
          CkExit();
      }
    }
    delete m;

    // print configuration
    CkPrintf("\n[CUDA vecadd example]\n");
    CkPrintf("Chares: %d\n", numChares);
    CkPrintf("Vector size: total %d, sub %d\n", vectorSize, vectorSize / numChares);
    CkPrintf("Iterations: %d\n", numIters);
    CkPrintf("Warmups: %d\n", numWarmups);
    CkPrintf("CUDA sync: %d\n", cudaSync);
    CkPrintf("No memcpy: %d\n", noMemcpy);

    // create 1D chare array
    workers = CProxy_Workers::ckNew(numChares);

    // start measuring execution time
    startTime = CkWallTimer();

    // fire off all chares in array
    workers.begin();
  }

  void warmupDone() {
    startTime = CkWallTimer();
    workers.begin();
  }

  void done() {
#ifdef USE_NVTX
    NVTXTracer nvtx_range("Main::done", NVTXColor::Turquoise);
#endif

    double dur = CkWallTimer() - startTime;
    CkPrintf("\nElapsed time: %lf s\n", dur);
    CkPrintf("Average time per iteration: %.3lf ms\n", dur / numIters * 1e3);
    CkExit();
  }
};

class Workers : public CBase_Workers {
 private:
  float* h_A;
  float* h_B;
  float* h_C;
#ifndef USE_WR
  float* d_A;
  float* d_B;
  float* d_C;
#endif
  int iter;
  int myVectorSize;
  cudaStream_t stream;
  CkCallback* cb;

 public:
  Workers() {
#ifdef USE_NVTX
    NVTXTracer nvtx_range("Workers::Workers", NVTXColor::WetAsphalt);
#endif

    iter = 0;
    myVectorSize = vectorSize / numChares;
    int size = myVectorSize * sizeof(float);
    hapiCheck(cudaMallocHost(&h_A, size));
    hapiCheck(cudaMallocHost(&h_B, size));
    hapiCheck(cudaMallocHost(&h_C, size));
    hapiCheck(cudaStreamCreate(&stream));
#ifndef USE_WR
    hapiCheck(cudaMalloc(&d_A, size));
    hapiCheck(cudaMalloc(&d_B, size));
    hapiCheck(cudaMalloc(&d_C, size));
#endif

    CkArrayIndex1D myIndex = CkArrayIndex1D(thisIndex);
    cb = new CkCallback(CkIndex_Workers::complete(), myIndex, thisArrayID);

    srand(time(NULL));
    randomInit(h_A, myVectorSize);
    randomInit(h_B, myVectorSize);
  }

  ~Workers() {
#ifdef USE_NVTX
    NVTXTracer nvtx_range("Workers::~Workers", NVTXColor::WetAsphalt);
#endif

    hapiCheck(cudaFreeHost(h_A));
    hapiCheck(cudaFreeHost(h_B));
    hapiCheck(cudaFreeHost(h_C));
    hapiCheck(cudaStreamDestroy(stream));
#ifndef USE_WR
    hapiCheck(cudaFree(d_A));
    hapiCheck(cudaFree(d_B));
    hapiCheck(cudaFree(d_C));
#endif
  }

  void begin() {
#ifdef USE_NVTX
    NVTXTracer nvtx_range("Workers::begin", NVTXColor::Carrot);
#endif

    iter++;
#ifdef USE_WR
    cudaVecAdd(myVectorSize, h_A, h_B, h_C, stream, (void*)cb);
#else
    size_t size = myVectorSize * sizeof(float);
    if ((iter == 0) || !noMemcpy) {
      hapiCheck(cudaMemcpyAsync(d_A, h_A, size, cudaMemcpyHostToDevice, stream));
      hapiCheck(cudaMemcpyAsync(d_B, h_B, size, cudaMemcpyHostToDevice, stream));
    }
    cudaVecAdd(myVectorSize, h_A, h_B, h_C, d_A, d_B, d_C, stream, (void*)cb);
    if ((iter == numWarmups + numIters) || !noMemcpy) {
      hapiCheck(cudaMemcpyAsync(h_C, d_C, size, cudaMemcpyDeviceToHost, stream));
    }
    if (cudaSync) {
      cudaStreamSynchronize(stream);
      cb->send();
    } else {
      hapiAddCallback(stream, cb);
    }
#endif
  }

  void complete() {
#ifdef USE_NVTX
    NVTXTracer nvtx_range("Workers::complete", NVTXColor::Clouds);
#endif

#ifdef DEBUG
    CkPrintf("[%d] A\n", thisIndex);
    for (int i = 0; i < myVectorSize; i++) {
      CkPrintf("%.2f ", h_A[i]);
    }
    CkPrintf("\n");

    CkPrintf("[%d] B\n", thisIndex);
    for (int i = 0; i < myVectorSize; i++) {
      CkPrintf("%.2f ", h_B[i]);
    }
    CkPrintf("\n");

    CkPrintf("[%d] C\n", thisIndex);
    for (int i = 0; i < myVectorSize; i++) {
      CkPrintf("%.2f ", h_C[i]);
    }
    CkPrintf("\n");

    CkPrintf("[%d] C-gold\n", thisIndex);
    for (int j = 0; j < myVectorSize; j++) {
      h_C[j] = h_A[j] + h_B[j];
      CkPrintf("%.2f ", h_C[j]);
    }
    CkPrintf("\n");
#endif

    if (iter == numWarmups) {
      contribute(CkCallback(CkIndex_Main::warmupDone(), mainProxy));
    } else if (iter == numWarmups + numIters) {
      contribute(CkCallback(CkIndex_Main::done(), mainProxy));
    } else {
      begin();
    }
  }
};

#include "vecadd.def.h"

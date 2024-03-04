#include "bandwidth.decl.h"
#include "hapi.h"
#include <vector>

#define USE_WAITALL 0

#define MAX_ITERS 1000000
#define LARGE_MESSAGE_SIZE 8192

/* readonly */ CProxy_Main main_proxy;
/* readonly */ CProxy_Block block_proxy;
/* readonly */ size_t min_size;
/* readonly */ size_t max_size;
/* readonly */ int n_iters_reg;
/* readonly */ int n_iters_large;
/* readonly */ int warmup_iters;
/* readonly */ int window_size;
/* readonly */ bool validate;

class Main : public CBase_Main {
  int cur_size;
  double start_time;

 public:
  Main(CkArgMsg* m) {
    main_proxy = thisProxy;
    min_size = 1;
    max_size = 4194304;
    n_iters_reg = 1000;
    n_iters_large = 100;
    warmup_iters = 10;
    window_size = 64;

    if (CkNumPes() != 2) {
      CkPrintf("Error: there should be 2 PEs");
      CkExit(1);
    }

    // Process command line arguments
    int c;
    while ((c = getopt(m->argc, m->argv, "s:x:i:l:w:d:")) != -1) {
      switch (c) {
        case 's':
          min_size = atoi(optarg);
          break;
        case 'x':
          max_size = atoi(optarg);
          break;
        case 'i':
          n_iters_reg = atoi(optarg);
          break;
        case 'l':
          n_iters_large = atoi(optarg);
          break;
        case 'w':
          warmup_iters = atoi(optarg);
          break;
        case 'd':
          window_size = atoi(optarg);
          break;
        default:
          CkPrintf("Unknown command line argument detected");
          CkExit(1);
      }
    }
    delete m;

    if (n_iters_reg > MAX_ITERS || n_iters_large > MAX_ITERS) {
      CkPrintf("Number of iterations must be less than %d\n", MAX_ITERS);
      CkExit(1);
    }

    // Print info
    CkPrintf("# Charm++ GPU Bandwidth Test\n"
        "# Message sizes: %lu - %lu bytes\n# Window size: %d\n"
        "# Iterations: %d regular, %d large\n# Warmup: %d\n",
        min_size, max_size, window_size, n_iters_reg, n_iters_large, warmup_iters);

    // Create block group chare
    block_proxy = CProxy_Block::ckNew();
    start_time = CkWallTimer();
    block_proxy.init();
  }

  void initDone() {
    CkPrintf("Starting test...\n");
    block_proxy.test();
  }

  void terminate() {
    CkPrintf("Elapsed: %.6lf s\n", CkWallTimer() - start_time);
    CkExit();
  }
};

class Block : public CBase_Block {
public:
  int iter;
  int peer;

  double start_time;
  double* times;

  char* h_local_data;
  char* h_remote_data;
  char* d_local_data;
  char* d_remote_data;
  bool memory_allocated;

  cudaStream_t stream;
  bool stream_created;

  CkChannel channel;
  CkFuture* futures;
#if USE_WAITALL
  std::vector<CkFutureID> future_ids;
#endif

  Block() {
    memory_allocated = false;
    stream_created = false;
  }

  ~Block() {
    if (memory_allocated) {
      free(times);
      hapiCheck(cudaFreeHost(h_local_data));
      hapiCheck(cudaFreeHost(h_remote_data));
      hapiCheck(cudaFree(d_local_data));
      hapiCheck(cudaFree(d_remote_data));
      delete futures;
#if USE_WAITALL
      future_ids.resize(0);
#endif
    }

    if (stream_created) cudaStreamDestroy(stream);
  }

  void init() {
    // Allocate memory for timers
    if (memory_allocated) free(times);
    times = (double*)malloc(MAX_ITERS * sizeof(double));

    // Determine the peer index
    peer = (thisIndex == 0) ? 1 : 0;

    // Allocate memory
    if (memory_allocated) {
      hapiCheck(cudaFreeHost(h_local_data));
      hapiCheck(cudaFreeHost(h_remote_data));
      hapiCheck(cudaFree(d_local_data));
      hapiCheck(cudaFree(d_remote_data));
      delete futures;
#if USE_WAITALL
      future_ids.resize(0);
#endif
    }
    hapiCheck(cudaMallocHost(&h_local_data, max_size));
    hapiCheck(cudaMallocHost(&h_remote_data, max_size));
    hapiCheck(cudaMalloc(&d_local_data, max_size));
    hapiCheck(cudaMalloc(&d_remote_data, max_size));
    futures = new CkFuture[window_size];
    memory_allocated = true;

    // Create CUDA stream
    if (!stream_created) {
      cudaStreamCreate(&stream);
      stream_created = true;
    }

    // Create channel between the pair of chares
    // (ID needs to be unique in the program)
    channel = CkChannel(0, thisProxy[peer]);

    // Reduce back to main
    contribute(CkCallback(CkReductionTarget(Main, initDone), main_proxy));
  }

  void test() {
    for (size_t cur_size = min_size; cur_size <= max_size; cur_size *= 2) {
      hapiCheck(cudaMemset(d_local_data, 'a', cur_size));
      hapiCheck(cudaMemset(d_remote_data, 'b', cur_size));

      int n_iters = (cur_size > LARGE_MESSAGE_SIZE) ? n_iters_large : n_iters_reg;

      for (int iter = 0; iter < warmup_iters + n_iters; iter++) {
        for (int i = 0; i < window_size; i++) {
          futures[i] = CkCreateFuture();
#if USE_WAITALL
          future_ids.push_back(futures[i].id);
#endif
        }
        start_time = CkWallTimer();

        if (thisIndex == 0) {
          for (int i = 0; i < window_size; i++) {
            channel.send(d_local_data, cur_size, &futures[i]);
          }

          // Note: For loop with CkWaitFuture seems to exhibit better performance
          // than using CkWaitAllIDs
#if USE_WAITALL
          CkWaitAllIDs(future_ids);
#else
          for (int i = 0; i < window_size; i++) {
            CkWaitFuture(futures[i]);
          }
#endif
          channel.recv(d_remote_data, cur_size, CkCallbackResumeThread());
        } else {
          for (int i = 0; i < window_size; i++) {
            channel.recv(d_remote_data, cur_size, &futures[i]);
          }
#if USE_WAITALL
          CkWaitAllIDs(future_ids);
#else
          for (int i = 0; i < window_size; i++) {
            CkWaitFuture(futures[i]);
          }
#endif
          channel.send(d_local_data, cur_size, CkCallbackResumeThread());
        }

        if (iter >= warmup_iters) {
          times[iter-warmup_iters] = CkWallTimer() - start_time;
        }

        for (int i = 0; i < window_size; i++) {
          CkReleaseFuture(futures[i]);
#if USE_WAITALL
          future_ids.clear();
#endif
        }
      }

      // Calculate average/mean time
      double times_sum = 0;
      for (int i = 0; i < n_iters; i++) {
        times_sum += times[i];
      }
      double times_mean = times_sum / n_iters;

      if (thisIndex == 0) {
        CkPrintf("Bandwidth for %lu bytes: %.3lf MB/s\n",
            cur_size, cur_size / 1e6 * window_size / times_mean);
      }

      if (validate) {
        hapiCheck(cudaMemcpy(h_remote_data, d_remote_data, cur_size, cudaMemcpyDeviceToHost));
        for (int i = 0; i < cur_size; i++) {
          if (h_remote_data[i] != 'a') {
            CkPrintf("Validation error: received data at %d is incorrect (%c), message size %lu\n",
                i, h_remote_data[i], cur_size);
            break;
          }
        }
        if (thisIndex == 0) CkPrintf("Validation passed\n");
      }
    }

    // Reduce back to main
    contribute(CkCallback(CkReductionTarget(Main, terminate), main_proxy));
  }
};

#include "bandwidth.def.h"

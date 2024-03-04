#include "latency.decl.h"
#include "hapi.h"

#define MAX_ITERS 1000000
#define LARGE_MESSAGE_SIZE 8192

/* readonly */ CProxy_Main main_proxy;
/* readonly */ CProxy_Block block_proxy;
/* readonly */ size_t min_size;
/* readonly */ size_t max_size;
/* readonly */ int n_iters_reg;
/* readonly */ int n_iters_large;
/* readonly */ int warmup_iters;
/* readonly */ bool validate;

class Main : public CBase_Main {
  double start_time;

 public:
  Main(CkArgMsg* m) {
    main_proxy = thisProxy;
    min_size = 1;
    max_size = 4194304;
    n_iters_reg = 1000;
    n_iters_large = 100;
    warmup_iters = 10;

    if (CkNumPes() != 2) {
      CkPrintf("Error: should be run with 2 PEs");
      CkExit(1);
    }

    // Process command line arguments
    int c;
    while ((c = getopt(m->argc, m->argv, "s:x:i:l:w:v")) != -1) {
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
        case 'v':
          validate = true;
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
    CkPrintf("# Charm++ GPU Latency Test (w/ Channels)\n"
        "# Message sizes: %lu - %lu bytes\n"
        "# Iterations: %d regular, %d large\n"
        "# Warmup: %d\n"
        "# Validation: %s\n",
        min_size, max_size, n_iters_reg, n_iters_large, warmup_iters,
        validate ? "true" : "false");

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
    }
    hapiCheck(cudaMallocHost(&h_local_data, max_size));
    hapiCheck(cudaMallocHost(&h_remote_data, max_size));
    hapiCheck(cudaMalloc(&d_local_data, max_size));
    hapiCheck(cudaMalloc(&d_remote_data, max_size));
    memory_allocated = true;

    // Create CUDA stream
    if (!stream_created) {
      cudaStreamCreate(&stream);
      stream_created = true;
    }

    // Create channel between the pair of chares (needs to be unique in the program)
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
        start_time = CkWallTimer();

        if (thisIndex == 0) {
          channel.send(d_local_data, cur_size, CkCallbackResumeThread());
          channel.recv(d_remote_data, cur_size, CkCallbackResumeThread());
        } else {
          channel.recv(d_remote_data, cur_size, CkCallbackResumeThread());
          channel.send(d_local_data, cur_size, CkCallbackResumeThread());
        }

        if (iter >= warmup_iters) {
          times[iter-warmup_iters] = (CkWallTimer() - start_time) / 2.0;
        }
      }

      // Calculate average/mean pingpong time
      double times_sum = 0;
      for (int i = 0; i < n_iters; i++) {
        times_sum += times[i];
      }
      double times_mean = times_sum / n_iters;

      // Calculate standard deviation
      double stdev = 0;
      for (int i = 0; i < n_iters; i++) {
        stdev += (times[i] - times_mean) * (times[i] - times_mean);
      }
      stdev = sqrt(stdev / n_iters);

      if (thisIndex == 0) {
        CkPrintf("Latency for %lu bytes: %.3lf += %.3lf us\n",
            cur_size, times_mean * 1e6, stdev * 1e6);
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

#include "latency.def.h"

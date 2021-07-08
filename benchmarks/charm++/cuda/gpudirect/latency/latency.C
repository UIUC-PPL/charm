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

class Main : public CBase_Main {
  bool zerocopy;
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
    zerocopy = false;

    if (CkNumPes() != 2) {
      CkPrintf("Error: there should be 2 PEs");
      CkExit(1);
    }

    // Process command line arguments
    int c;
    while ((c = getopt(m->argc, m->argv, "s:x:i:l:w:z")) != -1) {
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
        case 'z':
          zerocopy = true;
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
    CkPrintf("# Charm++ GPU Latency Test\n"
        "# Message sizes: %lu - %lu bytes\n"
        "# Iterations: %d regular, %d large\n"
        "# Warmup: %d\n"
        "# Zerocopy only: %s\n",
        min_size, max_size, n_iters_reg, n_iters_large, warmup_iters,
        zerocopy ? "true" : "false");

    // Create block group chare
    block_proxy = CProxy_Block::ckNew();
    start_time = CkWallTimer();
    block_proxy.init();
  }

  void initDone() {
    CkPrintf("Starting %s test...\n", zerocopy ? "zerocopy" : "regular");
    cur_size = min_size;
    testSetup();
  }

  void testSetup() {
    // Tell chares to memset their GPU data, will reduce back to testStart
    block_proxy.memset(cur_size);
  }

  void testStart() {
    // Start ping
    block_proxy[0].send(cur_size, zerocopy);
  }

  void testEnd() {
    cur_size *= 2;
    if (cur_size <= max_size) {
      // Proceed to next message size
      thisProxy.testSetup();
    } else {
      if (!zerocopy) {
        // Regular case done, proceed to zerocopy case
        zerocopy = true;
        block_proxy.init();
      } else {
        terminate();
      }
    }
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

  CkDeviceBuffer send_buffer;

  Block() {
    memory_allocated = false;
    stream_created = false;
  }

  ~Block() {
    if (memory_allocated) {
      if (CkMyPe() == 0) free(times);
      hapiCheck(cudaFreeHost(h_local_data));
      hapiCheck(cudaFreeHost(h_remote_data));
      hapiCheck(cudaFree(d_local_data));
      hapiCheck(cudaFree(d_remote_data));
    }

    if (stream_created) cudaStreamDestroy(stream);
  }

  void init() {
    // Reset iteration counter
    iter = 1;

    // Allocate memory for timers
    if (CkMyPe() == 0) {
      if (memory_allocated) free(times);
      times = (double*)malloc(MAX_ITERS * sizeof(double));
    }

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

    // Set up buffer metadata
    send_buffer = CkDeviceBuffer(d_local_data);

    // Reduce back to main
    contribute(CkCallback(CkReductionTarget(Main, initDone), main_proxy));
  }

  void memset(size_t size) {
    hapiCheck(cudaMemset(d_local_data, 'a', size));
    hapiCheck(cudaMemset(d_remote_data, 'b', size));

    // Reduce back to main
    contribute(CkCallback(CkReductionTarget(Main, testStart), main_proxy));
  }

  void send(size_t size, bool zerocopy) {
    if (CkMyPe() == 0) start_time = CkWallTimer();

    if (!zerocopy) {
      hapiCheck(cudaMemcpyAsync(h_local_data, d_local_data, size,
            cudaMemcpyDeviceToHost, stream));
      cudaStreamSynchronize(stream);
      thisProxy[peer].receiveReg(size, h_local_data);
    } else {
      thisProxy[peer].receiveZC(size, send_buffer);
    }
  }

  void receiveReg(size_t size, char* data) {
    // XXX: Do cudaMemcpy straight from data? It won't be pinned memory though
    memcpy(h_remote_data, data, size);
    hapiCheck(cudaMemcpyAsync(d_remote_data, h_remote_data, size,
          cudaMemcpyHostToDevice, stream));
    cudaStreamSynchronize(stream);

    afterReceive(size, false);
  }

  // First receive (post entry method), user should set the destination buffer
  void receiveZC(size_t& size, char*& data, CkDeviceBufferPost* devicePost) {
    // Inform the runtime where the incoming data should be stored
    // and which CUDA stream should be used for the transfer
    data = d_remote_data;
    devicePost[0].cuda_stream = stream; // Not used with UCX
  }

  // Second receive (regular entry method), invoked once the data transfers complete
  void receiveZC(size_t size, char* data) {
    afterReceive(size, true);
  }

  void afterReceive(size_t size, bool zerocopy) {
    int n_iters = (size > LARGE_MESSAGE_SIZE) ? n_iters_large : n_iters_reg;

    if (CkMyPe() == 1) {
      // PE 1: send pong
      send(size, zerocopy);
    } else {
      // PE 0: received pong
      if (iter > warmup_iters) {
        times[iter-warmup_iters-1] = (CkWallTimer() - start_time) / 2.0;
      }

      // Start next iteration or end test for current size
      if (iter++ == warmup_iters + n_iters) {
        // Reset iteration
        iter = 1;

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

        CkPrintf("Latency for %lu bytes: %.3lf += %.3lf us\n",
            size, times_mean * 1e6, stdev * 1e6);
        main_proxy.testEnd();
      } else {
        send(size, zerocopy);
      }
    }
  }
};

#include "latency.def.h"

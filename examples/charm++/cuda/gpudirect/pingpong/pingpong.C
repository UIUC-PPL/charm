#include "hapi.h"
#include "pingpong.decl.h"

/* readonly */ CProxy_Main main_proxy;
/* readonly */ CProxy_Pingpong pp_proxy;
/* readonly */ size_t size;
/* readonly */ int n_iters;
/* readonly */ int warmup_iters;

#define USE_TIMER 1
#define TIMERS_CNT 16384
#define TIMERS_PER_ITER 6
#define DURS_CNT (TIMERS_PER_ITER-1)

class CallbackMsg : public CMessage_CallbackMsg {
public:
  bool recv;
  int src;

  CallbackMsg(bool recv_, int src_) : recv(recv_), src(src_) {}
};

class Main : public CBase_Main {
  double init_start_time;
  double start_time;

public:
  Main(CkArgMsg* m) {
    // Set default values
    main_proxy = thisProxy;
    n_iters = 100;
    warmup_iters = 10;
    size = 4;

    // Process arguments
    int c;
    while ((c = getopt(m->argc, m->argv, "s:w:i:")) != -1) {
      switch (c) {
        case 's':
          size = atoi(optarg);
          break;
        case 'i':
          n_iters = atoi(optarg);
          break;
        case 'w':
          warmup_iters = atoi(optarg);
          break;
        default:
          CkExit();
      }
    }
    delete m;

    // Print configuration
    CkPrintf("\n[CUDA Pingpong Example]\n");
    CkPrintf("Size: %lu, Iterations: %d, Warm-up: %d\n", size, n_iters, warmup_iters);

    // Create and initialize chares
    pp_proxy = CProxy_Pingpong::ckNew(2);
    init_start_time = CkWallTimer();
    pp_proxy.init();
  }

  void startIter() {
    start_time = CkWallTimer();
    pp_proxy.run();
  }

  void initDone() {
    CkPrintf("Init time: %.3lf s\n", CkWallTimer() - init_start_time);
    startIter();
  }

  void warmupDone() {
    startIter();
  }

  void allDone() {
    double total_time = CkWallTimer() - start_time;
    CkPrintf("Total time: %.3lf s\nAverage iteration time: %.3lf us\n",
        total_time, (total_time / n_iters) * 1e6);
    CkExit();
  }
};

class Pingpong : public CBase_Pingpong {
  Pingpong_SDAG_CODE

public:
  int my_iter;
  int peer;

  CkChannel channel;
  CkCallback channel_cb;

  char* d_send_data;
  char* d_recv_data;

#if USE_TIMER
  double timers[TIMERS_CNT];
  int timer_idx;
#endif

  Pingpong() {}

  ~Pingpong() {
    hapiCheck(cudaFree(d_send_data));
    hapiCheck(cudaFree(d_recv_data));
  }

  void init() {
    my_iter = 0;
    peer = (thisIndex == 0) ? 1 : 0;
#if USE_TIMER
    for (int i = 0; i < TIMERS_CNT; i++) {
      timers[i] = 0.0;
    }
    timer_idx = 0;
#endif

    channel = CkChannel(0, thisProxy[peer]);
    channel_cb = CkCallback(CkIndex_Pingpong::channelCallback(nullptr), thisProxy[thisIndex]);

    cudaMalloc(&d_send_data, size);
    cudaMalloc(&d_recv_data, size);

    contribute(CkCallback(CkReductionTarget(Main, initDone), main_proxy));
  }

  void comm(bool first) {
#if USE_TIMER
    timers[timer_idx++] = CkWallTimer();
#endif

    channel_cb.setRefNum(my_iter);

    if (thisIndex == 0 && first == true || thisIndex == 1 && first == false) {
      channel.send(d_send_data, size, true, channel_cb, new CallbackMsg(false, thisIndex));
    } else if (thisIndex == 1 && first == true || thisIndex == 0 && first == false) {
      channel.recv(d_recv_data, size, true, channel_cb, new CallbackMsg(true, thisIndex));
    }

#if USE_TIMER
    timers[timer_idx++] = CkWallTimer();
#endif
  }

  void processCallback() {
#if USE_TIMER
    timers[timer_idx++] = CkWallTimer();
#endif
  }

  void end() {
    my_iter++;
    if (my_iter == warmup_iters) {
      contribute(CkCallback(CkReductionTarget(Main, warmupDone), main_proxy));
    } else if (my_iter == warmup_iters + n_iters) {
#if USE_TIMER
      double durations[DURS_CNT];

      for (int i = 0; i < DURS_CNT; i++) {
        durations[i] = 0.0;
      }

      for (int i = warmup_iters; i < (warmup_iters + n_iters); i++) {
        int idx = i * TIMERS_PER_ITER;
        for (int j = 0; j < DURS_CNT; j++) {
          durations[j] += (timers[idx+j+1] - timers[idx+j]);
        }
      }

      for (int i = 0; i < DURS_CNT; i++) {
        durations[i] /= n_iters;
        if (thisIndex == 0) {
          CkPrintf("Duration %d: %.3lf us\n", i, durations[i] * 1e6);
        }
      }
#endif

      contribute(CkCallback(CkReductionTarget(Main, allDone), main_proxy));
    } else {
      thisProxy[thisIndex].run();
    }
  }
};

#include "pingpong.def.h"

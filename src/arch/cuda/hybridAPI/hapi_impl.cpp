#include <stdio.h>
#include <stdlib.h>
#include <algorithm>
#include <queue>
#include <atomic>
#include <vector>

#include <cuda_runtime.h>

#include "converse.h"
#include "hapi.h"
#include "hapi_impl.h"
#ifdef HAPI_NVTX_PROFILE
#include "hapi_nvtx.h"
#endif

#if defined HAPI_TRACE || defined HAPI_INSTRUMENT_WRS
extern "C" double CmiWallTimer();
#endif

#ifdef HAPI_TRACE
#define QUEUE_SIZE_INIT 128
extern "C" int traceRegisterUserEvent(const char* x, int e);
extern "C" void traceUserBracketEvent(int e, double beginT, double endT);

typedef struct gpuEventTimer {
  int stage;
  double cmi_start_time;
  double cmi_end_time;
  int event_type;
  const char* trace_name;
} gpuEventTimer;
#endif

// Event stages used for profiling.
enum WorkRequestStage{
  DataSetup        = 1,
  KernelExecution  = 2,
  DataCleanup      = 3
};

enum ProfilingStage{
  GpuMemSetup   = 8800,
  GpuKernelExec = 8801,
  GpuMemCleanup = 8802
};

#ifndef HAPI_CUDA_CALLBACK
typedef struct hapiEvent {
  cudaEvent_t event;
  void* cb;
  void* cb_msg;
  hapiWorkRequest* wr; // if this is not NULL, buffers and request itself are deallocated

  hapiEvent(cudaEvent_t event_, void* cb_, void* cb_msg_, hapiWorkRequest* wr_ = NULL)
            : event(event_), cb(cb_), cb_msg(cb_msg_), wr(wr_) {}
} hapiEvent;

CpvDeclare(std::queue<hapiEvent>, hapi_event_queue);
CpvDeclare(std::queue<cudaEvent_t>, cuda_event_pool);
CsvDeclare(int, event_pool_size);
CsvDeclare(int, event_pool_inc);
CsvDeclare(bool, use_event_pool);
#endif
CpvDeclare(int, n_hapi_events);

void initEventQueues(char** argv) {
#ifndef HAPI_CUDA_CALLBACK
  CpvInitialize(std::queue<hapiEvent>, hapi_event_queue);
  CpvInitialize(std::queue<cudaEvent_t>, cuda_event_pool);
  CsvInitialize(int, event_pool_size);
  CsvInitialize(int, event_pool_inc);
  CsvInitialize(bool, use_event_pool);

  // First PE of each logical node processes options
  if (CmiMyRank() == 0) {
    if (CmiGetArgFlagDesc(argv, "+gpueventpooloff", "Turn off CUDA Event pool")) {
      CsvAccess(use_event_pool) = false;
    }
    else {
      CsvAccess(use_event_pool) = true;
      CsvAccess(event_pool_size) = CsvAccess(event_pool_inc) = 128;

      // CUDA Event pool options
      CmiGetArgIntDesc(argv, "+gpueventpoolsize", &CsvAccess(event_pool_size),
          "Initial size of CUDA Event pool");
      CmiGetArgIntDesc(argv, "+gpueventpoolinc", &CsvAccess(event_pool_inc),
          "Increment size of CUDA Event pool");

      if (CmiMyPe() == 0) {
        CmiPrintf("HAPI> Creating CUDA Event pool with size %d, inc %d\n",
            CsvAccess(event_pool_size), CsvAccess(event_pool_inc));
      }
    }
  }

  CmiNodeBarrier();

  if (CsvAccess(use_event_pool)) {
    // Create per-PE CUDA Event pool
    double create_start_time = CmiWallTimer();

    for (int i = 0; i < CsvAccess(event_pool_size); i++) {
      cudaEvent_t ev;
      cudaEventCreateWithFlags(&ev, cudaEventDisableTiming);
      CpvAccess(cuda_event_pool).push(ev);
    }

    if (CmiMyPe() == 0) {
      CmiPrintf("HAPI> Created CUDA Event pool in %.3lf ms\n",
          (CmiWallTimer() - create_start_time) * 1000);
    }
  }
#endif
  CpvInitialize(int, n_hapi_events);
  CpvAccess(n_hapi_events) = 0;
}

void destroyEventQueues() {
#ifndef HAPI_CUDA_CALLBACK
  // Destroy CUDA Event pool
  int to_destroy = CpvAccess(cuda_event_pool).size();
  for (int i = 0; i < to_destroy; i++) {
    cudaEvent_t& ev = CpvAccess(cuda_event_pool).front();
    cudaEventDestroy(ev);
    CpvAccess(cuda_event_pool).pop();
  }
#endif
}

// Used to invoke user's Charm++ callback function
void (*hapiInvokeCallback)(void*, void*) = NULL;

// Functions used to support quiescence detection.
void (*hapiQdCreate)(int) = NULL;
void (*hapiQdProcess)(int) = NULL;

// Initial size of the user-addressed portion of host/device buffer arrays;
// the system-addressed portion of host/device buffer arrays (used when there
// is no need to share buffers between work requests) will be equivalant in size.
// FIXME hard-coded maximum
#if CMK_SMP
#define NUM_BUFFERS 4096
#else
#define NUM_BUFFERS 256
#endif

#define MAX_PINNED_REQ 64
#define MAX_DELAYED_FREE_REQS 64

// Contains data and methods needed by HAPI.
class GPUManager {

public:
// Update for new row, again this shouldn't be hard coded!
#define HAPI_MEMPOOL_NUM_SLOTS 20
// Pre-allocated buffers will be at least this big (in bytes).
#define HAPI_MEMPOOL_MIN_BUFFER_SIZE 256
// Scale the amount of memory each node pins.
#define HAPI_MEMPOOL_SCALE 1.0

  std::vector<BufferPool> mempool_free_bufs_;
  std::vector<size_t> mempool_boundaries_;
  bool mempool_initialized_;

  // The runtime system keeps track of all allocated buffers on the GPU.
  // The following arrays contain pointers to host (CPU) data and the
  // corresponding data on the device (GPU).
  void **host_buffers_;
  void **device_buffers_;

  // Used to assign buffer IDs automatically by the system if the user
  // specifies an invalid buffer ID.
  int next_buffer_;

  cudaStream_t *streams_;
  int n_streams_;
  int last_stream_id_;

#ifdef HAPI_CUDA_CALLBACK
  int host_to_device_cb_idx_;
  int kernel_cb_idx_;
  int device_to_host_cb_idx_;
  int light_cb_idx_; // for lightweight version
#endif

  int running_kernel_idx_;
  int data_setup_idx_;
  int data_cleanup_idx_;

#ifdef HAPI_TRACE
  gpuEventTimer gpu_events_[QUEUE_SIZE_INIT * 3];
  std::atomic<int> time_idx_;
#endif

#ifdef HAPI_INSTRUMENT_WRS
  std::vector<std::vector<std::vector<hapiRequestTimeInfo>>> avg_times_;
  bool init_instr_;
#endif

#if CMK_SMP
  CmiNodeLock queue_lock_;
  CmiNodeLock progress_lock_;
  CmiNodeLock stream_lock_;
  CmiNodeLock mempool_lock_;
  CmiNodeLock inst_lock_;
#endif

  cudaDeviceProp device_prop_;
#ifdef HAPI_CUDA_CALLBACK
  bool cb_support;
#endif

  void init();
  int createStreams();
  int createNStreams(int);
  void destroyStreams();
  cudaStream_t getNextStream();
  cudaStream_t getStream(int);
  int getNStreams();
  void allocateBuffers(hapiWorkRequest*);
  void hostToDeviceTransfer(hapiWorkRequest*);
  void deviceToHostTransfer(hapiWorkRequest*);
  void freeBuffers(hapiWorkRequest*);
  void runKernel(hapiWorkRequest*);
};

// Declare GPU Manager as a process-shared object.
CsvDeclare(GPUManager, gpu_manager);

void GPUManager::init() {
  next_buffer_ = NUM_BUFFERS;
  streams_ = NULL;
  n_streams_ = 0;
  last_stream_id_ = -1;
  running_kernel_idx_ = 0;
  data_setup_idx_ = 0;
  data_cleanup_idx_ = 0;

#if CMK_SMP
  // create mutex locks
  queue_lock_ = CmiCreateLock();
  progress_lock_ = CmiCreateLock();
  stream_lock_ = CmiCreateLock();
  mempool_lock_ = CmiCreateLock();
  inst_lock_ = CmiCreateLock();
#endif

#ifdef HAPI_TRACE
  time_idx_ = 0;
#endif

  // store CUDA device properties
  int device;
  hapiCheck(cudaGetDevice(&device));
  hapiCheck(cudaGetDeviceProperties(&device_prop_, device));

#ifdef HAPI_CUDA_CALLBACK
  // check if CUDA callback is supported
  // CUDA 5.0 (compute capability 3.0) or newer
  cb_support = (device_prop_.major >= 3);
  if (!cb_support) {
    CmiAbort("[HAPI] CUDA callback is not supported on this device");
  }
#endif

  // allocate host/device buffers array (both user and system-addressed)
  host_buffers_ = new void*[NUM_BUFFERS*2];
  device_buffers_ = new void*[NUM_BUFFERS*2];

  // initialize device array to NULL
  for (int i = 0; i < NUM_BUFFERS*2; i++) {
    device_buffers_[i] = NULL;
  }

#ifdef HAPI_TRACE
  traceRegisterUserEvent("GPU Memory Setup", GpuMemSetup);
  traceRegisterUserEvent("GPU Kernel Execution", GpuKernelExec);
  traceRegisterUserEvent("GPU Memory Cleanup", GpuMemCleanup);
#endif

  // set up mempool metadata
  mempool_initialized_ = false;
  mempool_boundaries_.resize(HAPI_MEMPOOL_NUM_SLOTS);

  size_t buf_size = HAPI_MEMPOOL_MIN_BUFFER_SIZE;
  for(int i = 0; i < HAPI_MEMPOOL_NUM_SLOTS; i++){
    mempool_boundaries_[i] = buf_size;
    buf_size = buf_size << 1;
  }

#ifdef HAPI_INSTRUMENT_WRS
  init_instr_ = false;
#endif
}

// Creates streams equal to the maximum number of concurrent kernels,
// which depends on the compute capability of the device.
// Returns the number of created streams.
int GPUManager::createStreams() {
  int new_n_streams = 0;

  if (device_prop_.major == 3) {
    if (device_prop_.minor == 0)
      new_n_streams = 16;
    else if (device_prop_.minor == 2)
      new_n_streams = 4;
    else // 3.5, 3.7 or unknown 3.x
      new_n_streams = 32;
  }
  else if (device_prop_.major == 5) {
    if (device_prop_.minor == 3)
      new_n_streams = 16;
    else // 5.0, 5.2 or unknown 5.x
      new_n_streams = 32;
  }
  else if (device_prop_.major == 6) {
    if (device_prop_.minor == 1)
      new_n_streams = 32;
    else if (device_prop_.minor == 2)
      new_n_streams = 16;
    else // 6.0 or unknown 6.x
      new_n_streams = 128;
  }
  else // unknown (future) compute capability
    new_n_streams = 128;
#if !CMK_SMP
  // Allocate total physical streams between GPU managers sharing a device...
  // i.e. PEs / num devices
  int device_count;
  hapiCheck(cudaGetDeviceCount(&device_count));
  int pes_per_device = CmiNumPesOnPhysicalNode(0) / device_count;
  pes_per_device = pes_per_device > 0 ? pes_per_device : 1;
  new_n_streams =  (new_n_streams + pes_per_device - 1) / pes_per_device;
#endif

  int total_n_streams = createNStreams(new_n_streams);

  return total_n_streams;
}

int GPUManager::createNStreams(int new_n_streams) {
  if (new_n_streams <= n_streams_) {
    return n_streams_;
  }

  cudaStream_t* old_streams = streams_;

  streams_ = new cudaStream_t[new_n_streams];

  int i = 0;
  // Copy old streams
  for (; i < n_streams_; i++) {
    // TODO alt. use memcpy?
    streams_[i] = old_streams[i];
  }

  // Create new streams
  for (; i < new_n_streams; i++) {
    hapiCheck(cudaStreamCreate(&streams_[i]));
  }

  // Update
  n_streams_ = new_n_streams;
  delete [] old_streams;

  return n_streams_;
}

void GPUManager::destroyStreams() {
  if (streams_) {
    for (int i = 0; i < n_streams_; i++) {
      hapiCheck(cudaStreamDestroy(streams_[i]));
    }
  }
}

cudaStream_t GPUManager::getNextStream() {
  if (streams_ == NULL)
    return NULL;

  last_stream_id_ = (++last_stream_id_) % n_streams_;
  return streams_[last_stream_id_];
}

cudaStream_t GPUManager::getStream(int i) {
  if (streams_ == NULL)
    return NULL;

  if (i < 0 || i >= n_streams_)
    CmiAbort("[HAPI] invalid stream ID");
  return streams_[i];
}

int GPUManager::getNStreams() {
  if (!streams_) // NULL - default stream
    return 1;

  return n_streams_;
}

// Allocates device buffers.
void GPUManager::allocateBuffers(hapiWorkRequest* wr) {
  for (int i = 0; i < wr->getBufferCount(); i++) {
    hapiBufferInfo& bi = wr->buffers[i];
    int index = bi.id;
    size_t size = bi.size;

    // if index value is invalid, use an available ID
    if (index < 0 || index >= NUM_BUFFERS) {
      bool is_found = false;
      for (int j = next_buffer_; j < NUM_BUFFERS*2; j++) {
        if (device_buffers_[j] == NULL) {
          index = j;
          is_found = true;
          break;
        }
      }

      // if no index was found, try to search for a value at the
      // beginning of the system addressed space
      if (!is_found) {
        for (int j = NUM_BUFFERS; j < next_buffer_; j++) {
          if (device_buffers_[j] == NULL) {
            index = j;
            is_found = true;
            break;
          }
        }
      }

      if (!is_found) {
        CmiAbort("[HAPI] ran out of device buffer indices");
      }

      next_buffer_ = index + 1;
      if (next_buffer_ == NUM_BUFFERS*2) {
        next_buffer_ = NUM_BUFFERS;
      }

      bi.id = index;
    }

    if (device_buffers_[index] == NULL) {
      // allocate device memory
      hapiCheck(cudaMalloc((void **)&device_buffers_[index], size));

#ifdef HAPI_DEBUG
      CmiPrintf("[HAPI] allocated buffer %d at %p, time: %.2f, size: %zu\n",
             index, device_buffers_[index], cutGetTimerValue(timerHandle),
             size);
#endif
    }
  }
}

#ifndef HAPI_CUDA_CALLBACK
void recordEvent(cudaStream_t stream, void* cb, void* cb_msg, hapiWorkRequest* wr = NULL) {
  cudaEvent_t ev;

  if (CsvAccess(use_event_pool)) {
    if (CpvAccess(cuda_event_pool).empty()) {
      // CUDA Event pool empty, create more Events
      for (int i = 0; i < CsvAccess(event_pool_inc); i++) {
        cudaEvent_t new_ev;
        cudaEventCreateWithFlags(&new_ev, cudaEventDisableTiming);
        CpvAccess(cuda_event_pool).push(new_ev);
      }
    }

    ev = CpvAccess(cuda_event_pool).front();
    CpvAccess(cuda_event_pool).pop();
  }
  else {
    cudaEventCreateWithFlags(&ev, cudaEventDisableTiming);
  }
  cudaEventRecord(ev, stream);

  hapiEvent hev(ev, cb, cb_msg, wr);

  // push event information in queue
  CpvAccess(hapi_event_queue).push(hev);

  // increase count so that scheduler can poll the queue
  CpvAccess(n_hapi_events)++;
}
#endif

// Initiates host-to-device data transfer.
void GPUManager::hostToDeviceTransfer(hapiWorkRequest* wr) {
  for (int i = 0; i < wr->getBufferCount(); i++) {
    hapiBufferInfo& bi = wr->buffers[i];
    int index = bi.id;
    size_t size = bi.size;
    host_buffers_[index] = bi.host_buffer;

    if (bi.transfer_to_device) {
      hapiCheck(cudaMemcpyAsync(device_buffers_[index], host_buffers_[index], size,
                                cudaMemcpyHostToDevice, wr->stream));

#ifdef HAPI_DEBUG
      CmiPrintf("[HAPI] transferring buffer %d from host to device, time: %.2f, "
             "size: %zu\n", index, cutGetTimerValue(timerHandle), size);
#endif
    }
  }
}

// Initiates device-to-host data transfer.
void GPUManager::deviceToHostTransfer(hapiWorkRequest* wr) {
  for (int i = 0; i < wr->getBufferCount(); i++) {
    hapiBufferInfo& bi = wr->buffers[i];
    int index = bi.id;
    size_t size = bi.size;

    if (bi.transfer_to_host) {
      hapiCheck(cudaMemcpyAsync(host_buffers_[index], device_buffers_[index], size,
                                cudaMemcpyDeviceToHost, wr->stream));

#ifdef HAPI_DEBUG
      CmiPrintf("[HAPI] transferring buffer %d from device to host, time %.2f, "
             "size: %zu\n", index, cutGetTimerValue(timerHandle), size);
#endif
    }
  }
}

// Frees device buffers.
void GPUManager::freeBuffers(hapiWorkRequest* wr) {
  for (int i = 0; i < wr->getBufferCount(); i++) {
    hapiBufferInfo& bi = wr->buffers[i];
    int index = bi.id;

    if (bi.need_free) {
      hapiCheck(cudaFree(device_buffers_[index]));
      device_buffers_[index] = NULL;

#ifdef HAPI_DEBUG
      CmiPrintf("[HAPI] freed buffer %d, time %.2f\n",
             index, cutGetTimerValue(timerHandle));
#endif
    }
  }
}

inline static void hapiWorkRequestCleanup(hapiWorkRequest* wr) {
#if CMK_SMP
  CmiLock(CsvAccess(gpu_manager).progress_lock_);
#endif

  // free device buffers
  CsvAccess(gpu_manager).freeBuffers(wr);

#if CMK_SMP
  CmiUnlock(CsvAccess(gpu_manager).progress_lock_);
#endif

  // free hapiWorkRequest
  delete wr;
}

// Run the user's kernel for the given work request.
// This used to be a switch statement defined by the user to allow the runtime
// to execute the correct kernel.
void GPUManager::runKernel(hapiWorkRequest* wr) {
	if (wr->runKernel) {
		wr->runKernel(wr, wr->stream, device_buffers_);
	}
	// else, might be only for data transfer (or might be a bug?)
}

#ifdef HAPI_CUDA_CALLBACK
// Invokes user's host-to-device callback.
static void* hostToDeviceCallback(void* arg) {
#ifdef HAPI_NVTX_PROFILE
  NVTXTracer nvtx_range("hostToDeviceCallback", NVTXColor::Asbestos);
#endif
  hapiWorkRequest* wr = *((hapiWorkRequest**)((char*)arg + CmiMsgHeaderSizeBytes + sizeof(int)));
  CmiAssert(hapiInvokeCallback);
  hapiInvokeCallback(wr->host_to_device_cb);

  // inform QD that the host-to-device transfer is complete
  CmiAssert(hapiQdProcess);
  hapiQdProcess(1);

  return NULL;
}

// Invokes user's kernel execution callback.
static void* kernelCallback(void* arg) {
#ifdef HAPI_NVTX_PROFILE
  NVTXTracer nvtx_range("kernelCallback", NVTXColor::Asbestos);
#endif
  hapiWorkRequest* wr = *((hapiWorkRequest**)((char*)arg + CmiMsgHeaderSizeBytes + sizeof(int)));
  CmiAssert(hapiInvokeCallback);
  hapiInvokeCallback(wr->kernel_cb);

  // inform QD that the kernel is complete
  CmiAssert(hapiQdProcess);
  hapiQdProcess(1);

  return NULL;
}

// Frees device buffers and invokes user's device-to-host callback.
// Invoked regardless of the availability of the user's callback.
static void* deviceToHostCallback(void* arg) {
#ifdef HAPI_NVTX_PROFILE
  NVTXTracer nvtx_range("deviceToHostCallback", NVTXColor::Asbestos);
#endif
  hapiWorkRequest* wr = *((hapiWorkRequest**)((char*)arg + CmiMsgHeaderSizeBytes + sizeof(int)));

  // invoke user callback
  if (wr->device_to_host_cb) {
    CmiAssert(hapiInvokeCallback);
    hapiInvokeCallback(wr->device_to_host_cb);
  }

  hapiWorkRequestCleanup(wr);

  // inform QD that device-to-host transfer is complete
  CmiAssert(hapiQdProcess);
  hapiQdProcess(1);

  return NULL;
}

// Used by lightweight HAPI.
static void* lightCallback(void *arg) {
#ifdef HAPI_NVTX_PROFILE
  NVTXTracer nvtx_range("lightCallback", NVTXColor::Asbestos);
#endif

  char* conv_msg_tmp = (char*)arg + CmiMsgHeaderSizeBytes + sizeof(int);
  void* cb = *((void**)conv_msg_tmp);

  // invoke user callback
  if (cb) {
    CmiAssert(hapiInvokeCallback);
    hapiInvokeCallback(cb);
  }

  // notify process to QD
  CmiAssert(hapiQdProcess);
  hapiQdProcess(1);

  return NULL;
}
#endif // HAPI_CUDA_CALLBACK

// Register callback functions. All PEs need to call this.
void hapiRegisterCallbacks() {
#ifdef HAPI_CUDA_CALLBACK
  // FIXME: Potential race condition on assignments, but CmiAssignOnce
  // causes a hang at startup.
  CsvAccess(gpu_manager).host_to_device_cb_idx_
    = CmiRegisterHandler((CmiHandler)hostToDeviceCallback);
  CsvAccess(gpu_manager).kernel_cb_idx_
    = CmiRegisterHandler((CmiHandler)kernelCallback);
  CsvAccess(gpu_manager).device_to_host_cb_idx_
    = CmiRegisterHandler((CmiHandler)deviceToHostCallback);
  CsvAccess(gpu_manager).light_cb_idx_
    = CmiRegisterHandler((CmiHandler)lightCallback);
#endif
}

#ifdef HAPI_CUDA_CALLBACK
// Callback function invoked by the CUDA runtime certain parts of GPU work are
// complete. It sends a converse message to the original PE to free the relevant
// device memory and invoke the user's callback. The reason for this method is
// that a thread created by the CUDA runtime does not have access to any of the
// CpvDeclare'd variables as it is not one of the threads created by the Charm++
// runtime.
static void CUDART_CB CUDACallback(cudaStream_t stream, cudaError_t status,
                                   void *data) {
#ifdef HAPI_NVTX_PROFILE
  NVTXTracer nvtx_range("CUDACallback", NVTXColor::Silver);
#endif

  if (status == cudaSuccess) {
    // send message to the original PE
    char *conv_msg = (char*)data;
    int dstRank = *((int *)(conv_msg + CmiMsgHeaderSizeBytes));
    CmiPushPE(dstRank, conv_msg);
  }
  else {
    CmiAbort("[HAPI] error before CUDACallback");
  }
}

enum CallbackStage {
  AfterHostToDevice,
  AfterKernel,
  AfterDeviceToHost
};

static void addCallback(hapiWorkRequest *wr, CallbackStage stage) {
  // create converse message to be delivered to this PE after CUDA callback
  char *conv_msg = (char *)CmiAlloc(CmiMsgHeaderSizeBytes + sizeof(int) +
                                  sizeof(hapiWorkRequest *)); // FIXME memory leak?
  *((int *)(conv_msg + CmiMsgHeaderSizeBytes)) = CmiMyRank();
  *((hapiWorkRequest **)(conv_msg + CmiMsgHeaderSizeBytes + sizeof(int))) = wr;

  int handlerIdx;
  switch (stage) {
    case AfterHostToDevice:
      handlerIdx = CsvAccess(gpu_manager).host_to_device_cb_idx_;
      break;
    case AfterKernel:
      handlerIdx = CsvAccess(gpu_manager).kernel_cb_idx_;
      break;
    case AfterDeviceToHost:
      handlerIdx = CsvAccess(gpu_manager).device_to_host_cb_idx_;
      break;
    default: // wrong type
      CmiFree(conv_msg);
      return;
  }
  CmiSetHandler(conv_msg, handlerIdx);

  // add callback into CUDA stream
  hapiCheck(cudaStreamAddCallback(wr->stream, CUDACallback, (void*)conv_msg, 0));
}
#endif // HAPI_CUDA_CALLBACK

/******************** DEPRECATED ********************/
// User calls this function to offload work to the GPU.
void hapiEnqueue(hapiWorkRequest* wr) {
#ifdef HAPI_NVTX_PROFILE
  NVTXTracer nvtx_range("enqueue", NVTXColor::Pomegranate);
#endif

#if CMK_SMP
  CmiLock(CsvAccess(gpu_manager).progress_lock_);
#endif

  // allocate device memory
  CsvAccess(gpu_manager).allocateBuffers(wr);

  // transfer data to device
  CsvAccess(gpu_manager).hostToDeviceTransfer(wr);

  // add host-to-device transfer callback
  if (wr->host_to_device_cb) {
    // while there is an ongoing workrequest, quiescence should not be detected
    // even if all PEs seem idle
    CmiAssert(hapiQdCreate);
    hapiQdCreate(1);

#ifdef HAPI_CUDA_CALLBACK
    addCallback(wr, AfterHostToDevice);
#else
    recordEvent(wr->stream, wr->host_to_device_cb, NULL);
#endif
  }

  // run kernel
  CsvAccess(gpu_manager).runKernel(wr);

  // add kernel callback
  if (wr->kernel_cb) {
    CmiAssert(hapiQdCreate);
    hapiQdCreate(1);

#ifdef HAPI_CUDA_CALLBACK
    addCallback(wr, AfterKernel);
#else
    recordEvent(wr->stream, wr->kernel_cb, NULL);
#endif
  }

  // transfer data to host
  CsvAccess(gpu_manager).deviceToHostTransfer(wr);

  // add device-to-host transfer callback
  CmiAssert(hapiQdCreate);
  hapiQdCreate(1);
#ifdef HAPI_CUDA_CALLBACK
  // always invoked to free memory
  addCallback(wr, AfterDeviceToHost);
#else
  if (wr->device_to_host_cb) {
    recordEvent(wr->stream, wr->device_to_host_cb, NULL, wr);
  }
  else {
    recordEvent(wr->stream, NULL, NULL, wr);
  }
#endif

#if CMK_SMP
  CmiUnlock(CsvAccess(gpu_manager).progress_lock_);
#endif
}

/******************** DEPRECATED ********************/
// Creates a hapiWorkRequest object on the heap and returns it to the user.
hapiWorkRequest* hapiCreateWorkRequest() {
  return (new hapiWorkRequest);
}

hapiWorkRequest::hapiWorkRequest() :
    grid_dim(0), block_dim(0), shared_mem(0), host_to_device_cb(NULL),
    kernel_cb(NULL), device_to_host_cb(NULL), runKernel(NULL), state(0),
    user_data(NULL), free_user_data(false), free_host_to_device_cb(false),
    free_kernel_cb(false), free_device_to_host_cb(false)
  {
#ifdef HAPI_TRACE
    trace_name = "";
#endif
#ifdef HAPI_INSTRUMENT_WRS
    chare_index = -1;
#endif

#if CMK_SMP
    CmiLock(CsvAccess(gpu_manager).stream_lock_);
#endif

    // Create default per-PE streams if none exist
    if (CsvAccess(gpu_manager).getStream(0) == NULL) {
      CsvAccess(gpu_manager).createNStreams(CmiMyNodeSize());
    }

    stream = CsvAccess(gpu_manager).getStream(CmiMyRank() % CsvAccess(gpu_manager).getNStreams());

#if CMK_SMP
    CmiUnlock(CsvAccess(gpu_manager).stream_lock_);
#endif
  }

static void createPool(int *nbuffers, int n_slots, std::vector<BufferPool> &pools);
static void releasePool(std::vector<BufferPool> &pools);

// Initialization of HAPI functionalities.
void initHybridAPI() {
  // create and initialize GPU Manager object
  CsvInitialize(GPUManager, gpu_manager);
  CsvAccess(gpu_manager).init();
}

// Set up PE to GPU mapping, invoked from all PEs
// TODO: Support custom mappings
void initDeviceMapping(char** argv) {
  Mapping map_type = Mapping::Block; // Default is block mapping
  bool all_gpus = false; // If true, all GPUs are visible to all processes.
                         // Otherwise, only a subset are visible (e.g. with jsrun)
  char* gpumap = NULL;

  // Process +gpumap
  if (CmiGetArgStringDesc(argv, "+gpumap", &gpumap,
        "define pe to gpu device mapping")) {
    if (CmiMyPe() == 0) {
      CmiPrintf("HAPI> PE-GPU mapping: %s\n", gpumap);
    }

    if (strcmp(gpumap, "none") == 0) {
      map_type = Mapping::None;
    }
    else if (strcmp(gpumap, "block") == 0) {
      map_type = Mapping::Block;
    }
    else if (strcmp(gpumap, "roundrobin") == 0) {
      map_type = Mapping::RoundRobin;
    }
    else {
      CmiAbort("Unsupported mapping type!");
    }
  }

  // Process +allgpus
  if (CmiGetArgFlagDesc(argv, "+allgpus",
        "all GPUs are visible to all processes")) {
    all_gpus = true;
    if (CmiMyPe() == 0) {
      CmiPrintf("HAPI> All GPUs are visible to all processes\n");
    }
  }

  // No mapping specified, user assumes responsibility
  if (map_type == Mapping::None) {
    if (CmiMyPe() == 0) {
      CmiPrintf("HAPI> User should explicitly select devices for PEs/chares\n");
    }
    return;
  }

  CmiAssert(map_type != Mapping::None);

  // Get number of GPUs (visible to each process)
  int gpu_count;
  hapiCheck(cudaGetDeviceCount(&gpu_count));
  if (gpu_count <= 0) {
    CmiAbort("Unable to perform PE-GPU mapping, no GPUs found!");
  }

  // Perform mapping
  int my_gpu = 0;
  int pes_per_gpu = (all_gpus ? CmiNumPesOnPhysicalNode(CmiPhysicalNodeID(CmiMyPe())) :
      CmiNodeSize(CmiMyNode())) / gpu_count;

  switch (map_type) {
    case Mapping::Block:
      my_gpu = (all_gpus ? CmiPhysicalRank(CmiMyPe()) : CmiMyRank()) / pes_per_gpu;
      break;
    case Mapping::RoundRobin:
      my_gpu = (all_gpus ? CmiPhysicalRank(CmiMyPe()) : CmiMyRank()) % gpu_count;
      break;
    default:
      CmiAbort("Unsupported mapping type!");
  }

  hapiCheck(cudaSetDevice(my_gpu));
}

// Clean up and delete memory used by HAPI.
void exitHybridAPI() {
#if CMK_SMP
  // destroy mutex locks
  CmiDestroyLock(CsvAccess(gpu_manager).queue_lock_);
  CmiDestroyLock(CsvAccess(gpu_manager).progress_lock_);
  CmiDestroyLock(CsvAccess(gpu_manager).stream_lock_);
  CmiDestroyLock(CsvAccess(gpu_manager).mempool_lock_);
#endif

  // destroy streams (if they were created)
  CsvAccess(gpu_manager).destroyStreams();

  // release memory pool if it was used
  if (CsvAccess(gpu_manager).mempool_initialized_) {
    releasePool(CsvAccess(gpu_manager).mempool_free_bufs_);
  }

#ifdef HAPI_TRACE
  for (int i = 0; i < CsvAccess(gpu_manager).time_idx_; i++) {
    switch (CsvAccess(gpu_manager).gpu_events_[i].event_type) {
    case DataSetup:
      CmiPrintf("[HAPI] kernel %s data setup\n",
             CsvAccess(gpu_manager).gpu_events_[i].trace_name);
      break;
    case DataCleanup:
      CmiPrintf("[HAPI] kernel %s data cleanup\n",
             CsvAccess(gpu_manager).gpu_events_[i].trace_name);
      break;
    case KernelExecution:
      CmiPrintf("[HAPI] kernel %s execution\n",
             CsvAccess(gpu_manager).gpu_events_[i].trace_name);
      break;
    default:
      CmiPrintf("[HAPI] invalid timer identifier\n");
    }
    CmiPrintf("[HAPI] %.2f:%.2f\n",
           CsvAccess(gpu_manager).gpu_events_[i].cmi_start_time -
           CsvAccess(gpu_manager).gpu_events_[0].cmi_start_time,
           CsvAccess(gpu_manager).gpu_events_[i].cmi_end_time -
           CsvAccess(gpu_manager).gpu_events_[0].cmi_start_time);
  }
#endif
}

/******************** DEPRECATED ********************/
// Need to be updated with the Tracing API.
static inline void gpuEventStart(hapiWorkRequest* wr, int* index,
                                 WorkRequestStage event, ProfilingStage stage) {
#ifdef HAPI_TRACE
  gpuEventTimer* shared_gpu_events_ = CsvAccess(gpu_manager).gpu_events_;
  int shared_time_idx_ = CsvAccess(gpu_manager).time_idx_++;
  shared_gpu_events_[shared_time_idx_].cmi_start_time = CmiWallTimer();
  shared_gpu_events_[shared_time_idx_].event_type = event;
  shared_gpu_events_[shared_time_idx_].trace_name = wr->trace_name;
  *index = shared_time_idx_;
  shared_gpu_events_[shared_time_idx_].stage = stage;
#ifdef HAPI_DEBUG
  CmiPrintf("[HAPI] start event %d of WR %s, profiling stage %d\n",
         event, wr->trace_name, stage);
#endif
#endif // HAPI_TRACE
}

/******************** DEPRECATED ********************/
// Need to be updated with the Tracing API.
static inline void gpuEventEnd(int index) {
#ifdef HAPI_TRACE
  CsvAccess(gpu_manager).gpu_events_[index].cmi_end_time = CmiWallTimer();
  traceUserBracketEvent(CsvAccess(gpu_manager).gpu_events_[index].stage,
                        CsvAccess(gpu_manager).gpu_events_[index].cmi_start_time,
                        CsvAccess(gpu_manager).gpu_events_[index].cmi_end_time);
#ifdef HAPI_DEBUG
  Cmiprintf("[HAPI] end event %d of WR %s, profiling stage %d\n",
          CsvAccess(gpu_manager).gpu_events_[index].event_type,
          CsvAccess(gpu_manager).gpu_events_[index].trace_name,
          CsvAccess(gpu_manager).gpu_events_[index].stage);
#endif
#endif // HAPI_TRACE
}

static inline void hapiWorkRequestStartTime(hapiWorkRequest* wr) {
#ifdef HAPI_INSTRUMENT_WRS
  wr->phase_start_time = CmiWallTimer();
#endif
}

static inline void profileWorkRequestEvent(hapiWorkRequest* wr,
                                           WorkRequestStage event) {
#ifdef HAPI_INSTRUMENT_WRS
#if CMK_SMP
  CmiLock(CsvAccess(gpu_manager).inst_lock_);
#endif

  if (CsvAccess(gpu_manager).init_instr_) {
    double tt = CmiWallTimer() - (wr->phase_start_time);
    int index = wr->chare_index;
    char type = wr->comp_type;
    char phase = wr->comp_phase;

    std::vector<hapiRequestTimeInfo> &vec = CsvAccess(gpu_manager).avg_times_[index][type];
    if (vec.size() <= phase) {
      vec.resize(phase+1);
    }
    switch (event) {
      case DataSetup:
        vec[phase].transfer_time += tt;
        break;
      case KernelExecution:
        vec[phase].kernel_time += tt;
        break;
      case DataCleanup:
        vec[phase].cleanup_time += tt;
        vec[phase].n++;
        break;
      default:
        CmiPrintf("[HAPI] invalid event during profileWorkRequestEvent\n");
    }
  }
  else {
    CmiPrintf("[HAPI] instrumentation not initialized!\n");
  }

#if CMK_SMP
  CmiUnlock(CsvAccess(gpu_manager).inst_lock_);
#endif
#endif // HAPI_INSTRUMENT_WRS
}

// Create a pool with n_slots slots.
// There are n_buffers[i] buffers for each buffer size corresponding to entry i.
// TODO list the alignment/fragmentation issues with either of two allocation schemes:
// if single, large buffer is allocated for each subpool
// if multiple, smaller buffers are allocated for each subpool
static void createPool(int *n_buffers, int n_slots, std::vector<BufferPool> &pools){
  std::vector<size_t>& mempool_boundaries = CsvAccess(gpu_manager).mempool_boundaries_;

  // initialize pools
  pools.resize(n_slots);
  for (int i = 0; i < n_slots; i++) {
    pools[i].size = mempool_boundaries[i];
    pools[i].head = NULL;
  }

  // divide by # of PEs on physical node and multiply by # of PEs in logical node
  size_t available_memory = CsvAccess(gpu_manager).device_prop_.totalGlobalMem /
                           CmiNumPesOnPhysicalNode(CmiPhysicalNodeID(CmiMyPe()))
                           * CmiMyNodeSize() * HAPI_MEMPOOL_SCALE;

  // pre-calculate memory per size
  int max_buffers = *std::max_element(n_buffers, n_buffers + n_slots);
  int n_buffers_to_allocate[n_slots];
  memset(n_buffers_to_allocate, 0, sizeof(n_buffers_to_allocate));
  size_t buf_size;
  while (available_memory >= mempool_boundaries[0] + sizeof(BufferPoolHeader)) {
    for (int i = 0; i < max_buffers; i++) {
      for (int j = n_slots - 1; j >= 0; j--) {
        buf_size = mempool_boundaries[j] + sizeof(BufferPoolHeader);
        if (i < n_buffers[j] && buf_size <= available_memory) {
          n_buffers_to_allocate[j]++;
          available_memory -= buf_size;
        }
      }
    }
  }

  // pin the host memory
  for (int i = 0; i < n_slots; i++) {
    buf_size = mempool_boundaries[i] + sizeof(BufferPoolHeader);
    int num_buffers = n_buffers_to_allocate[i];

    BufferPoolHeader* hd;
    BufferPoolHeader* previous = NULL;

    // pin host memory in a contiguous block for a slot
    void* pinned_chunk;
    hapiCheck(cudaMallocHost(&pinned_chunk, buf_size * num_buffers));

    // initialize header structs
    for (int j = num_buffers - 1; j >= 0; j--) {
      hd = reinterpret_cast<BufferPoolHeader*>(reinterpret_cast<unsigned char*>(pinned_chunk)
                                     + buf_size * j);
      hd->slot = i;
      hd->next = previous;
      previous = hd;
    }

    pools[i].head = previous;
#ifdef HAPI_MEMPOOL_DEBUG
    pools[i].num = num_buffers;
#endif
  }
}

static void releasePool(std::vector<BufferPool> &pools){
  for (int i = 0; i < pools.size(); i++) {
    BufferPoolHeader* hdr = pools[i].head;
    if (hdr != NULL) {
      hapiCheck(cudaFreeHost((void*)hdr));
    }
  }
  pools.clear();
}

static int findPool(size_t size){
  int boundary_array_len = CsvAccess(gpu_manager).mempool_boundaries_.size();
  if (size <= CsvAccess(gpu_manager).mempool_boundaries_[0]) {
    return 0;
  }
  else if (size > CsvAccess(gpu_manager).mempool_boundaries_[boundary_array_len-1]) {
    // create new slot
    CsvAccess(gpu_manager).mempool_boundaries_.push_back(size);

    BufferPool newpool;
    hapiCheck(cudaMallocHost((void**)&newpool.head, size + sizeof(BufferPoolHeader)));
    if (newpool.head == NULL) {
      CmiPrintf("[HAPI (%d)] findPool: failed to allocate newpool %d head, size %zu\n",
             CmiMyPe(), boundary_array_len, size);
      CmiAbort("[HAPI] failed newpool allocation");
    }
    newpool.size = size;
#ifdef HAPI_MEMPOOL_DEBUG
    newpool.num = 1;
#endif
    CsvAccess(gpu_manager).mempool_free_bufs_.push_back(newpool);

    BufferPoolHeader* hd = newpool.head;
    hd->next = NULL;
    hd->slot = boundary_array_len;

    return boundary_array_len;
  }
  for (int i = 0; i < CsvAccess(gpu_manager).mempool_boundaries_.size()-1; i++) {
    if (CsvAccess(gpu_manager).mempool_boundaries_[i] < size &&
        size <= CsvAccess(gpu_manager).mempool_boundaries_[i+1]) {
      return (i + 1);
    }
  }
  return -1;
}

static void* getBufferFromPool(int pool, size_t size){
  BufferPoolHeader* ret;

  if (pool < 0 || pool >= CsvAccess(gpu_manager).mempool_free_bufs_.size()) {
    CmiPrintf("[HAPI (%d)] getBufferFromPool, pool: %d, size: %zu invalid pool\n",
           CmiMyPe(), pool, size);
#ifdef HAPI_MEMPOOL_DEBUG
    CmiPrintf("[HAPI (%d)] num: %d\n", CmiMyPe(),
           CsvAccess(gpu_manager).mempool_free_bufs_[pool].num);
#endif
    CmiAbort("[HAPI] exiting after invalid pool");
  }
  else if (CsvAccess(gpu_manager).mempool_free_bufs_[pool].head == NULL) {
    BufferPoolHeader* hd;
    hapiCheck(cudaMallocHost((void**)&hd, sizeof(BufferPoolHeader) +
                             CsvAccess(gpu_manager).mempool_free_bufs_[pool].size));
#ifdef HAPI_MEMPOOL_DEBUG
    CmiPrintf("[HAPI (%d)] getBufferFromPool, pool: %d, size: %zu expand by 1\n",
           CmiMyPe(), pool, size);
#endif
    if (hd == NULL) {
      CmiAbort("[HAPI] exiting after NULL hd from pool");
    }
    hd->slot = pool;
    return (void*)(hd + 1);
  }
  else {
    ret = CsvAccess(gpu_manager).mempool_free_bufs_[pool].head;
    CsvAccess(gpu_manager).mempool_free_bufs_[pool].head = ret->next;
#ifdef HAPI_MEMPOOL_DEBUG
    ret->size = size;
    CsvAccess(gpu_manager).mempool_free_bufs_[pool].num--;
#endif
    return (void*)(ret + 1);
  }
  return NULL;
}

static void returnBufferToPool(int pool, BufferPoolHeader* hd) {
  hd->next = CsvAccess(gpu_manager).mempool_free_bufs_[pool].head;
  CsvAccess(gpu_manager).mempool_free_bufs_[pool].head = hd;
#ifdef HAPI_MEMPOOL_DEBUG
  CsvAccess(gpu_manager).mempool_free_bufs_[pool].num++;
#endif
}

void* hapiPoolMalloc(size_t size) {
#if CMK_SMP
  CmiLock(CsvAccess(gpu_manager).mempool_lock_);
#endif

  if (!CsvAccess(gpu_manager).mempool_initialized_) {
    // create pool of page-locked memory
    int sizes[HAPI_MEMPOOL_NUM_SLOTS];
          /*256*/ sizes[0]  =  4;
          /*512*/ sizes[1]  =  2;
         /*1024*/ sizes[2]  =  2;
         /*2048*/ sizes[3]  =  4;
         /*4096*/ sizes[4]  =  2;
         /*8192*/ sizes[5]  =  6;
        /*16384*/ sizes[6]  =  5;
        /*32768*/ sizes[7]  =  2;
        /*65536*/ sizes[8]  =  1;
       /*131072*/ sizes[9]  =  1;
       /*262144*/ sizes[10] =  1;
       /*524288*/ sizes[11] =  1;
      /*1048576*/ sizes[12] =  1;
      /*2097152*/ sizes[13] =  2;
      /*4194304*/ sizes[14] =  2;
      /*8388608*/ sizes[15] =  2;
     /*16777216*/ sizes[16] =  2;
     /*33554432*/ sizes[17] =  1;
     /*67108864*/ sizes[18] =  1;
    /*134217728*/ sizes[19] =  7;
    createPool(sizes, HAPI_MEMPOOL_NUM_SLOTS, CsvAccess(gpu_manager).mempool_free_bufs_);
    CsvAccess(gpu_manager).mempool_initialized_ = true;

#ifdef HAPI_MEMPOOL_DEBUG
    CmiPrintf("[HAPI (%d)] done creating buffer pool\n", CmiMyPe());
#endif
  }

  int pool = findPool(size);
  void* buf = getBufferFromPool(pool, size);

#ifdef HAPI_MEMPOOL_DEBUG
  CmiPrintf("[HAPI (%d)] hapiPoolMalloc size %zu pool %d left %d\n",
         CmiMyPe(), size, pool,
         CsvAccess(gpu_manager).mempool_free_bufs_[pool].num);
#endif

#if CMK_SMP
  CmiUnlock(CsvAccess(gpu_manager).mempool_lock_);
#endif

  return buf;
}

void hapiPoolFree(void* ptr) {
  // check if mempool was initialized, just return if not
  if (!CsvAccess(gpu_manager).mempool_initialized_)
    return;

  BufferPoolHeader* hd = ((BufferPoolHeader*)ptr) - 1;
  int pool = hd->slot;

#ifdef HAPI_MEMPOOL_DEBUG
  size_t size = hd->size;
#endif

#if CMK_SMP
  CmiLock(CsvAccess(gpu_manager).mempool_lock_);
#endif

  returnBufferToPool(pool, hd);

#if CMK_SMP
  CmiUnlock(CsvAccess(gpu_manager).mempool_lock_);
#endif

#ifdef HAPI_MEMPOOL_DEBUG
  CmiPrintf("[HAPI (%d)] hapiPoolFree size %zu pool %d left %d\n",
         CmiMyPe(), size, pool,
         CsvAccess(gpu_manager).mempool_free_bufs_[pool].num);
#endif
}

#ifdef HAPI_INSTRUMENT_WRS
void hapiInitInstrument(int n_chares, int n_types) {
#if CMK_SMP
  CmiLock(CsvAccess(gpu_manager).inst_lock_);
#endif

  if (!CsvAccess(gpu_manager).init_instr_) {
    CsvAccess(gpu_manager).avg_times_.resize(n_chares);
    for (int i = 0; i < n_chares; i++) {
      CsvAccess(gpu_manager).avg_times_[i].resize(n_types);
    }
    CsvAccess(gpu_manager).init_instr_ = true;
  }

#if CMK_SMP
  CmiUnlock(CsvAccess(gpu_manager).inst_lock_);
#endif
}

hapiRequestTimeInfo* hapiQueryInstrument(int chare, char type, char phase) {
#if CMK_SMP
  CmiLock(CsvAccess(gpu_manager).inst_lock_);
#endif

  if (phase < CsvAccess(gpu_manager).avg_times_[chare][type].size()) {
    return &CsvAccess(gpu_manager).avg_times_[chare][type][phase];
  }
  else {
    return NULL;
  }

#if CMK_SMP
  CmiUnlock(CsvAccess(gpu_manager).inst_lock_);
#endif
}

void hapiClearInstrument() {
#if CMK_SMP
  CmiLock(CsvAccess(gpu_manager).inst_lock_);
#endif

  for (int chare = 0; chare < CsvAccess(gpu_manager).avg_times_.size(); chare++) {
    for (char type = 0; type < CsvAccess(gpu_manager).avg_times_[chare].size(); type++) {
      CsvAccess(gpu_manager).avg_times_[chare][type].clear();
    }
    CsvAccess(gpu_manager).avg_times_[chare].clear();
  }
  CsvAccess(gpu_manager).avg_times_.clear();
  CsvAccess(gpu_manager).init_instr_ = false;

#if CMK_SMP
  CmiUnlock(CsvAccess(gpu_manager).inst_lock_);
#endif
}
#endif // HAPI_INSTRUMENT_WRS

// Poll HAPI events stored in the PE's queue. Current strategy is to process
// all successive completed events in the queue starting from the front.
// TODO Maybe we should make one pass of all events in the queue instead,
// since there might be completed events later in the queue.
void hapiPollEvents() {
#ifndef HAPI_CUDA_CALLBACK
  std::queue<hapiEvent>& queue = CpvAccess(hapi_event_queue);
  while (!queue.empty()) {
    hapiEvent hev = queue.front();
    if (cudaEventQuery(hev.event) == cudaSuccess) {
      // invoke Charm++ callback if one was given
      if (hev.cb) {
        CmiAssert(hapiInvokeCallback);
        hapiInvokeCallback(hev.cb, hev.cb_msg);
      }

      // clean up hapiWorkRequest
      if (hev.wr) {
        hapiWorkRequestCleanup(hev.wr);
      }

      // Return CUDA Event back to pool or destroy it
      if (CsvAccess(use_event_pool)) {
        CpvAccess(cuda_event_pool).push(hev.event);
      }
      else {
        cudaEventDestroy(hev.event);
      }

      queue.pop();

      CpvAccess(n_hapi_events)--;

      // inform QD that an event was processed
      CmiAssert(hapiQdProcess);
      hapiQdProcess(1);
    }
    else {
      // stop going through the queue once we encounter a non-successful event
      break;
    }
  }
#endif
}

int hapiCreateStreams() {
#if CMK_SMP
  CmiLock(CsvAccess(gpu_manager).stream_lock_);
#endif

  int ret = CsvAccess(gpu_manager).createStreams();

#if CMK_SMP
  CmiUnlock(CsvAccess(gpu_manager).stream_lock_);
#endif

  return ret;
}

cudaStream_t hapiGetStream() {
#if CMK_SMP
  CmiLock(CsvAccess(gpu_manager).stream_lock_);
#endif

  cudaStream_t ret = CsvAccess(gpu_manager).getNextStream();

#if CMK_SMP
  CmiUnlock(CsvAccess(gpu_manager).stream_lock_);
#endif

  return ret;
}

// Lightweight HAPI, to be invoked after data transfer or kernel execution.
void hapiAddCallback(cudaStream_t stream, void* cb, void* cb_msg) {
#ifndef HAPI_CUDA_CALLBACK
  // record CUDA event
  recordEvent(stream, cb, cb_msg);
#else
  /* FIXME works for now (faster too), but CmiAlloc might not be thread-safe
#if CMK_SMP
  CmiLock(CsvAccess(gpu_manager).queue_lock_);
#endif
*/

  // create converse message to be delivered to this PE after CUDA callback
  char* conv_msg = (char*)CmiAlloc(CmiMsgHeaderSizeBytes + sizeof(int) +
                                 sizeof(void*)); // FIXME memory leak?
  char* conv_msg_tmp = conv_msg + CmiMsgHeaderSizeBytes;
  *((int*)conv_msg_tmp) = CmiMyRank();
  conv_msg_tmp += sizeof(int);
  *((void**)conv_msg_tmp) = cb;
  CmiSetHandler(conv_msg, CsvAccess(gpu_manager).light_cb_idx_);

  // push into CUDA stream
  hapiCheck(cudaStreamAddCallback(stream, CUDACallback, (void*)conv_msg, 0));

  /*
#if CMK_SMP
  CmiUnlock(CsvAccess(gpu_manager).queue_lock_);
#endif
*/
#endif

  // while there is an ongoing workrequest, quiescence should not be detected
  // even if all PEs seem idle
  CmiAssert(hapiQdCreate);
  hapiQdCreate(1);
}

cudaError_t hapiMalloc(void** devPtr, size_t size) {
  return cudaMalloc(devPtr, size);
}

cudaError_t hapiFree(void* devPtr) {
  return cudaFree(devPtr);
}

cudaError_t hapiMallocHost(void** ptr, size_t size) {
  return cudaMallocHost(ptr, size);
}

cudaError_t hapiMallocHostPool(void** ptr, size_t size) {
  void* tmp_ptr = hapiPoolMalloc(size);
  if (tmp_ptr) {
    *ptr = tmp_ptr;
    return cudaSuccess;
  }
  else return cudaErrorMemoryAllocation;
}

cudaError_t hapiFreeHost(void* ptr) {
  return cudaFreeHost(ptr);
}

cudaError_t hapiFreeHostPool(void *ptr) {
  hapiPoolFree(ptr);
  return cudaSuccess;
}

cudaError_t hapiMemcpyAsync(void* dst, const void* src, size_t count, cudaMemcpyKind kind, cudaStream_t stream = 0) {
  return cudaMemcpyAsync(dst, src, count, kind, stream);
}

void hapiErrorDie(cudaError_t retCode, const char* code, const char* file, int line) {
  if (retCode != cudaSuccess) {
    fprintf(stderr, "Fatal CUDA Error [%d] %s at %s:%d\n", retCode, cudaGetErrorString(retCode), file, line);
    CmiAbort("Exit due to CUDA error");
  }
}

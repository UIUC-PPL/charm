#ifndef _CONV_RDMADEVICE_H_
#define _CONV_RDMADEVICE_H_

#include "conv-header.h"
#include "cmirdmautils.h"
#include "pup.h"

#if CMK_CUDA
#include <cuda_runtime.h>

// Represents the mode of device-side zerocopy transfer
// MEMCPY indicates that the PEs are on the same logical node and cudaMemcpyDeviceToDevice can be used
// IPC indicates that the PEs are on different logical nodes within the same physical node and CUDA IPC can be used
// RDMA indicates that the PEs are on different physical nodes and requires GPUDirect RDMA
enum class CmiNcpyModeDevice : char { MEMCPY, IPC, RDMA };

// Status of Direct API (persistent) transfer
enum class CmiDeviceStatus : char { incomplete, complete };

class CmiDeviceBuffer {
public:
  // Pointer to and size of the buffer
  const void* ptr;
  size_t cnt;
  cudaStream_t cuda_stream;

#if !CMK_GPU_COMM
  // Source and destination PEs
  int src_pe;
  int dest_pe;

  // Used for CUDA IPC
  int device_idx;
  size_t comm_offset;
  int event_idx;

  // Store the actual data for host-staged inter-node messaging (no GPUDirect RDMA)
  bool data_stored;
  void* data;

  CmiDeviceBuffer() : ptr(NULL), cnt(0), src_pe(-1), dest_pe(-1) { init(); }

  explicit CmiDeviceBuffer(const void* ptr_, size_t cnt_) : ptr(ptr_), cnt(cnt_),
    src_pe(CmiMyPe()), dest_pe(-1) { init(); }

  void init() {
    device_idx = -1;
    comm_offset = 0;
    event_idx = -1;
    cuda_stream = cudaStreamPerThread;

    data_stored = false;
    data = NULL;
  }
#else
  uint64_t tag;

  CmiDeviceBuffer() : ptr(NULL), cnt(0) {}

  explicit CmiDeviceBuffer(const void* ptr_, size_t cnt_) : ptr(ptr_), cnt(cnt_) {}
#endif // CMK_GPU_COMM

  void pup(PUP::er &p) {
    p((char *)&ptr, sizeof(ptr));
    p|cnt;
#if !CMK_GPU_COMM
    p|src_pe;
    p|dest_pe;
    p|device_idx;
    p|comm_offset;
    p|event_idx;
    p|data_stored;
    if (data_stored) {
      if (p.isUnpacking()) {
        cudaMallocHost(&data, cnt);
      }
      PUParray(p, (char*)data, cnt);
    }
#else
    p|tag;
#endif // CMK_GPU_COMM
  }

  ~CmiDeviceBuffer() {
#if !CMK_GPU_COMM
    if (data) cudaFreeHost(data);
#endif
  }
};

CmiNcpyModeDevice findTransferModeDevice(int srcPe, int destPe);

#if CMK_GPU_COMM
typedef void (*RdmaAckCallerFn)(void *token);

void CmiSendDevice(int dest_pe, const void*& ptr, size_t size, uint64_t& tag);
void CmiRecvDevice(DeviceRdmaOp* op, DeviceRecvType type);
void CmiRdmaDeviceRecvInit(RdmaAckCallerFn fn);
void CmiInvokeRecvHandler(void* data);
#endif // CMK_GPU_COMM
#endif // CMK_CUDA

#endif // _CONV_RDMADEVICE_H_

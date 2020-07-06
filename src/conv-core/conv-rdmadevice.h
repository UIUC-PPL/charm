#ifndef _CONV_RDMADEVICE_H_
#define _CONV_RDMADEVICE_H_

#if CMK_CUDA

#include "cmirdmautils.h"
#include "pup.h"
#include <cuda_runtime.h>

// Represents the mode of device-side zerocopy transfer
// MEMCPY indicates that the PEs are on the same logical node and cudaMemcpyDeviceToDevice can be used
// IPC indicates that the PEs are on different logical nodes within the same physical node and CUDA IPC can be used
// RDMA indicates that the PEs are on different physical nodes and requires GPUDirect RDMA
enum class CmiNcpyModeDevice : char { MEMCPY, IPC, RDMA };

class CmiDeviceBuffer {
public:
  // Pointer to and size of the buffer
  const void* ptr;
  size_t cnt;

  // Home PE
  int pe;

  // Used for CUDA IPC
  int device_idx;
  size_t comm_offset;
  int event_idx;
  cudaStream_t cuda_stream;

  // Store the actual data for host-staged inter-node messaging (no GPUDirect RDMA)
  bool data_stored;
  void* data;

  CmiDeviceBuffer() : ptr(NULL), cnt(0), pe(-1) { init(); }

  explicit CmiDeviceBuffer(const void* ptr_, size_t cnt_) : ptr(ptr_), cnt(cnt_),
    pe(CmiMyPe()) { init(); }

  void init() {
    device_idx = -1;
    comm_offset = 0;
    event_idx = -1;
    cuda_stream = cudaStreamPerThread;

    data_stored = false;
    data = NULL;
  }

  void pup(PUP::er &p) {
    p((char *)&ptr, sizeof(ptr));
    p|cnt;
    p|pe;
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
  }

  ~CmiDeviceBuffer() {
    if (data) cudaFreeHost(data);
  }
};

CmiNcpyModeDevice findTransferModeDevice(int srcPe, int destPe);

#endif // CMK_CUDA

#endif // _CONV_RDMADEVICE_H_

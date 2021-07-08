#include "converse.h"
#include "conv-rdmadevice.h"

#if CMK_CUDA

CmiNcpyModeDevice findTransferModeDevice(int srcPe, int dstPe) {
  CmiEnforce((srcPe >= 0) && (srcPe <= CmiNumPes()));
  CmiEnforce((dstPe >= 0) && (dstPe <= CmiNumPes()));

  if (CmiNodeOf(srcPe) == CmiNodeOf(dstPe)) {
    // Same logical node
    return CmiNcpyModeDevice::MEMCPY;
  }
  else if (CmiPeOnSamePhysicalNode(srcPe, dstPe)) {
    // Different logical nodes, same physical node
    return CmiNcpyModeDevice::IPC;
  }
  else {
    // Different physical nodes, requires GPUDirect RDMA
    return CmiNcpyModeDevice::RDMA;
  }
}

#if CMK_GPU_COMM
#include "machine-rdma.h"

void CmiSendDevice(int dest_pe, const void*& ptr, size_t size, uint64_t& tag) {
  LrtsSendDevice(dest_pe, ptr, size, tag);
}

void CmiRecvDevice(DeviceRdmaOp* op, DeviceRecvType type) {
  LrtsRecvDevice(op, type);
}

RdmaAckHandlerFn rdmaDeviceRecvHandlerFn;

void CmiRdmaDeviceRecvInit(RdmaAckHandlerFn fn) {
  // Set handler function that gets invoked when data transfer is complete (on receiver)
  rdmaDeviceRecvHandlerFn = fn;
}

void CmiInvokeRecvHandler(void* data) {
  rdmaDeviceRecvHandlerFn(data);
}
#endif // CMK_GPU_COMM
#endif // CMK_CUDA

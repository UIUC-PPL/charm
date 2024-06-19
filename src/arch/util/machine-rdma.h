#ifndef _MACHINE_RDMA_H_
#define _MACHINE_RDMA_H_

/* Support for Nocopy Direct API */
void LrtsSetRdmaBufferInfo(void *info, const void *ptr, int size, unsigned short int mode);
void LrtsIssueRget(NcpyOperationInfo *ncpyOpInfo);

void LrtsIssueRput(NcpyOperationInfo *ncpyOpInfo);

void LrtsDeregisterMem(const void *ptr, void *info, int pe, unsigned short int mode);

#if CMK_REG_REQUIRED
void LrtsInvokeRemoteDeregAckHandler(int pe, NcpyOperationInfo *ncpyOpInfo);
#endif

void CmiInvokeNcpyAck(void *ack);

// Function pointer to acknowledgement handler
typedef void (*RdmaAckHandlerFn)(void *token);

#if CMK_GPU_COMM
#if CMK_CUDA
void LrtsSendDevice(int dest_pe, const void*& ptr, size_t size, uint64_t& tag);
void LrtsRecvDevice(DeviceRdmaOp* op, CommType type);

void CmiDeviceRecvHandler(void* data);
#endif // CMK_CUDA

void LrtsChannelSend(int dest_pe, int id, const void*& ptr, size_t size, void* meta, uint64_t tag);
void LrtsChannelRecv(int id, const void*& ptr, size_t size, void* meta, uint64_t tag);

void CmiChannelHandler(void* data);
#endif // CMK_GPU_COMM

int CmiGetRdmaCommonInfoSize();
#endif

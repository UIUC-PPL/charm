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

#if CMK_CUDA
// Function pointer to acknowledgement handler
typedef void (*RdmaAckHandlerFn)(void *token);

void LrtsSendDevice(int dest_pe, const void*& ptr, size_t size, uint64_t& tag);
void LrtsRecvDevice(DeviceRdmaOp* op);

void CmiInvokeRecvHandler(void* data);
#endif

int CmiGetRdmaCommonInfoSize();
#endif

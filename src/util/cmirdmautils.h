#ifndef _CKRDMAUTILS_H
#define _CKRDMAUTILS_H

#include "conv-header.h"
#include <stdio.h>
#include <stddef.h>

// Structure that can be used across layers
typedef struct ncpystruct{

  // Used in the MPI layer
#if CMK_CONVERSE_MPI
  char core[CmiMsgHeaderSizeBytes];
  int tag;
#endif

  const void *srcPtr;
  char *srcLayerInfo;
  char *srcAck;
  const void *srcRef;
  int srcPe;
  size_t srcSize;
  short int srcLayerSize;
  short int srcAckSize;
  unsigned char srcRegMode;
  unsigned char srcDeregMode;
  unsigned char isSrcRegistered;

  const void *destPtr;
  char *destLayerInfo;
  char *destAck;
  const void *destRef;
  int destPe;
  size_t destSize;
  short int destAckSize;
  short int destLayerSize;
  unsigned char destRegMode;
  unsigned char destDeregMode;
  unsigned char isDestRegistered;

  unsigned char opMode; // CMK_DIRECT_API for p2p direct api
                        // CMK_DIRECT_API_REVERSE for p2p direct api with inverse operation
                        // CMK_EM_API for p2p entry method api
                        // CMK_EM_API_REVERSE for p2p entry method api with inverse operation

  // Variables used for ack handling
  unsigned char ackMode; // CMK_SRC_DEST_ACK for call both src and dest acks
                         // CMK_SRC_ACK for call just src ack
                         // CMK_DEST_ACK for call just dest ack

  unsigned char freeMe;  // CMK_FREE_NCPYOPINFO in order to free NcpyOperationInfo
                         // CMK_DONT_FREE_NCPYOPINFO in order to not free NcpyOperationInfo

  short int ncpyOpInfoSize;

  int rootNode; // used only for Broadcast, -1 for p2p operations

  void *refPtr;

}NcpyOperationInfo;

enum CommType {
  COMM_TYPE_CHARM,
  COMM_TYPE_AMPI,
  COMM_TYPE_CHARM4PY
};

#if CMK_CUDA
typedef struct DeviceRdmaInfo_ {
  int n_ops; // Number of RDMA operations, i.e. number of buffers being sent
  int counter; // Used to track the number of completed RDMA operations
  void* msg; // Charm++ message to be (re-)enqueued after all operations complete
} DeviceRdmaInfo;

typedef struct DeviceRdmaOp_ {
  int dest_pe;
  const void* dest_ptr;
  size_t size;
  DeviceRdmaInfo* info;
  void* src_cb;
  void* dst_cb;
  uint64_t tag;
} DeviceRdmaOp;

typedef struct DeviceRdmaOpMsg_ {
  char header[CmiMsgHeaderSizeBytes];
  DeviceRdmaOp op;
} DeviceRdmaOpMsg;
#endif // CMK_CUDA

#ifdef __cplusplus
extern "C" {
#endif

int getNcpyOpInfoTotalSize(
  int srcLayerSize,
  int srcAckSize,
  int destLayerSize,
  int destAckSize);

void setNcpyOpInfo(
  const void *srcPtr,
  char *srcLayerInfo,
  int srcLayerSize,
  char *srcAck,
  int srcAckSize,
  size_t srcSize,
  unsigned short int srcRegMode,
  unsigned short int srcDeregMode,
  unsigned short int isSrcRegistered,
  int srcPe,
  const void *srcRef,
  const void *destPtr,
  char *destLayerInfo,
  int destLayerSize,
  char *destAck,
  int destAckSize,
  size_t destSize,
  unsigned short int destRegMode,
  unsigned short int destDeregMode,
  unsigned short int isdestRegistered,
  int destPe,
  const void *destRef,
  int rootNode,
  NcpyOperationInfo *ncpyOpInfo);

void resetNcpyOpInfoPointers(NcpyOperationInfo *ncpyOpInfo);

void setReverseModeForNcpyOpInfo(NcpyOperationInfo *ncpyOpInfo);

#ifdef __cplusplus
}
#endif

#endif

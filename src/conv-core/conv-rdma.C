/* Support for Direct Nocopy API (Generic Implementation)
 * Specific implementations are in arch/layer/machine-onesided.{h,c}
 */
#include "converse.h"
#include "conv-rdma.h"
#include <algorithm>
#include <vector>

bool useCMAForZC;
CpvExtern(std::vector<NcpyOperationInfo *>, newZCPupGets);
static int zc_pup_handler_idx;

// Methods required to keep the Nocopy Direct API functional on non-LRTS layers
#if !CMK_USE_LRTS
void CmiSetRdmaCommonInfo(void *info, const void *ptr, int size) {
}

int CmiGetRdmaCommonInfoSize() {
  return 0;
}

void CmiSetNcpyAckSize(int ackSize) {}
#endif

#if !CMK_ONESIDED_IMPL
/****************************** Zerocopy Direct API For non-RDMA layers *****************************/
/* Support for generic implementation */

// Function Pointer to Acknowledement handler function for the Direct API
RdmaAckCallerFn ncpyDirectAckHandlerFn;

// An Rget initiator PE sends this message to the target PE that will be the source of the data
typedef struct _converseRdmaMsg {
  char cmicore[CmiMsgHeaderSizeBytes];
} ConverseRdmaMsg;

static int get_request_handler_idx;
static int put_data_handler_idx;

// Invoked when this PE has to send a large array for an Rget
static void getRequestHandler(ConverseRdmaMsg *getReqMsg){

  NcpyOperationInfo *ncpyOpInfo = (NcpyOperationInfo *)((char *)(getReqMsg) + sizeof(ConverseRdmaMsg));

  resetNcpyOpInfoPointers(ncpyOpInfo);

  ncpyOpInfo->freeMe = CMK_DONT_FREE_NCPYOPINFO;

  // Get is implemented internally using a call to Put
  CmiIssueRput(ncpyOpInfo);
}

// Invoked when this PE receives a large array as the target of an Rput or the initiator of an Rget
static void putDataHandler(ConverseRdmaMsg *payloadMsg) {

  NcpyOperationInfo *ncpyOpInfo = (NcpyOperationInfo *)((char *)payloadMsg + sizeof(ConverseRdmaMsg));

  resetNcpyOpInfoPointers(ncpyOpInfo);

  // copy the received messsage into the user's destination address
  memcpy((char *)ncpyOpInfo->destPtr,
         (char *)payloadMsg + sizeof(ConverseRdmaMsg) + ncpyOpInfo->ncpyOpInfoSize,
         std::min(ncpyOpInfo->srcSize, ncpyOpInfo->destSize));

  // Invoke the destination ack
  ncpyOpInfo->ackMode = CMK_DEST_ACK; // Only invoke the destination ack
  ncpyOpInfo->freeMe  = CMK_DONT_FREE_NCPYOPINFO;
  ncpyDirectAckHandlerFn(ncpyOpInfo);

  CmiFree(payloadMsg);
}

void CmiSetDirectNcpyAckHandler(RdmaAckCallerFn fn) {
  ncpyDirectAckHandlerFn = fn;
}

void CmiIssueRget(NcpyOperationInfo *ncpyOpInfo) {

  int ncpyOpInfoSize = ncpyOpInfo->ncpyOpInfoSize;

  // Send a ConverseRdmaMsg to other PE requesting it to send the array
  ConverseRdmaMsg *getReqMsg = (ConverseRdmaMsg *)CmiAlloc(sizeof(ConverseRdmaMsg) + ncpyOpInfoSize);

  // copy the additional Info into the getReqMsg
  memcpy((char *)getReqMsg + sizeof(ConverseRdmaMsg),
         (char *)ncpyOpInfo,
         ncpyOpInfoSize);

  CmiSetHandler(getReqMsg, get_request_handler_idx);
  CmiSyncSendAndFree(ncpyOpInfo->srcPe, sizeof(ConverseRdmaMsg) + ncpyOpInfoSize, getReqMsg);

  // free original ncpyOpinfo
  CmiFree(ncpyOpInfo);
}

void CmiIssueRput(NcpyOperationInfo *ncpyOpInfo) {

  int ncpyOpInfoSize = ncpyOpInfo->ncpyOpInfoSize;
  size_t size = ncpyOpInfo->srcSize;
  int destPe = ncpyOpInfo->destPe;

  // Send a ConverseRdmaMsg to the other PE sending the array
  ConverseRdmaMsg *payloadMsg = (ConverseRdmaMsg *)CmiAlloc(sizeof(ConverseRdmaMsg) + ncpyOpInfoSize + size);

  // copy the ncpyOpInfo into the recvMsg
  memcpy((char *)payloadMsg + sizeof(ConverseRdmaMsg),
         (char *)ncpyOpInfo,
         ncpyOpInfoSize);

  // copy the large array into the recvMsg
  memcpy((char *)payloadMsg + sizeof(ConverseRdmaMsg) + ncpyOpInfoSize,
         ncpyOpInfo->srcPtr,
         size);

  // Invoke the source ack
  ncpyOpInfo->ackMode = CMK_SRC_ACK; // only invoke the source ack

  ncpyDirectAckHandlerFn(ncpyOpInfo);

  CmiSetHandler(payloadMsg, put_data_handler_idx);
  CmiSyncSendAndFree(destPe,
                     sizeof(ConverseRdmaMsg) + ncpyOpInfoSize + size,
                     payloadMsg);
}

void CmiSetRdmaBufferInfo(void *info, const void *ptr, int size, unsigned short int mode) {}

void CmiDeregisterMem(const void *ptr, void *info, int pe, unsigned short int mode) {}

#endif

// Rget/Rput operations are implemented as normal converse messages
// This method is invoked during converse initialization to initialize these message handlers
void CmiOnesidedDirectInit(void) {
#if !CMK_ONESIDED_IMPL
  get_request_handler_idx = CmiRegisterHandler((CmiHandler)getRequestHandler);
  put_data_handler_idx = CmiRegisterHandler((CmiHandler)putDataHandler);
#endif
  zc_pup_handler_idx = CmiRegisterHandler((CmiHandler)zcPupHandler);
}

/****************************** Zerocopy Direct API *****************************/

// Get Methods
void CmiNcpyBuffer::memcpyGet(CmiNcpyBuffer &source) {
  // memcpy the data from the source buffer into the destination buffer
  memcpy((void *)ptr, source.ptr, std::min(cnt, source.cnt));
}

#if CMK_USE_CMA
void CmiNcpyBuffer::cmaGet(CmiNcpyBuffer &source) {
  CmiIssueRgetUsingCMA(source.ptr,
         source.layerInfo,
         source.pe,
         ptr,
         layerInfo,
         pe,
         std::min(cnt, source.cnt));
}
#endif


void CmiNcpyBuffer::rdmaGet(CmiNcpyBuffer &source, int ackSize, char *srcAck, char *destAck) {

  //int ackSize = sizeof(CmiCallback);

  if(regMode == CMK_BUFFER_UNREG) {
    // register it because it is required for RGET
    CmiSetRdmaBufferInfo(layerInfo + CmiGetRdmaCommonInfoSize(), ptr, cnt, regMode);

    isRegistered = true;
  }

  int rootNode = -1; // -1 is the rootNode for p2p operations

  NcpyOperationInfo *ncpyOpInfo = createNcpyOpInfo(source, *this, ackSize, srcAck, destAck, rootNode, CMK_DIRECT_API, (void *)ref);

  CmiIssueRget(ncpyOpInfo);
}

NcpyOperationInfo *CmiNcpyBuffer::createNcpyOpInfo(CmiNcpyBuffer &source, CmiNcpyBuffer &destination, int ackSize, char *srcAck, char *destAck, int rootNode, int opMode, void *refPtr) {

  int layerInfoSize = CMK_COMMON_NOCOPY_DIRECT_BYTES + CMK_NOCOPY_DIRECT_BYTES;

  // Create a general object that can be used across layers and can store the state of the CmiNcpyBuffer objects
  int ncpyObjSize = getNcpyOpInfoTotalSize(
                      layerInfoSize,
                      ackSize,
                      layerInfoSize,
                      ackSize);

  NcpyOperationInfo *ncpyOpInfo = (NcpyOperationInfo *)CmiAlloc(ncpyObjSize);

  setNcpyOpInfo(source.ptr,
                (char *)(source.layerInfo),
                layerInfoSize,
                srcAck,
                ackSize,
                source.cnt,
                source.regMode,
                source.deregMode,
                source.isRegistered,
                source.pe,
                source.ref,
                destination.ptr,
                (char *)(destination.layerInfo),
                layerInfoSize,
                destAck,
                ackSize,
                destination.cnt,
                destination.regMode,
                destination.deregMode,
                destination.isRegistered,
                destination.pe,
                destination.ref,
                rootNode,
                ncpyOpInfo);

  ncpyOpInfo->opMode = opMode;
  ncpyOpInfo->refPtr = refPtr;

  return ncpyOpInfo;
}

// Put Methods
void CmiNcpyBuffer::memcpyPut(CmiNcpyBuffer &destination) {
  // memcpy the data from the source buffer into the destination buffer
  memcpy((void *)destination.ptr, ptr, std::min(cnt, destination.cnt));
}

#if CMK_USE_CMA
void CmiNcpyBuffer::cmaPut(CmiNcpyBuffer &destination) {
  CmiIssueRputUsingCMA(destination.ptr,
                       destination.layerInfo,
                       destination.pe,
                       ptr,
                       layerInfo,
                       pe,
                       std::min(cnt, destination.cnt));
}
#endif

void CmiNcpyBuffer::rdmaPut(CmiNcpyBuffer &destination, int ackSize, char *srcAck, char *destAck) {

  int layerInfoSize = CMK_COMMON_NOCOPY_DIRECT_BYTES + CMK_NOCOPY_DIRECT_BYTES;

  if(regMode == CMK_BUFFER_UNREG) {
    // register it because it is required for RPUT
    CmiSetRdmaBufferInfo(layerInfo + CmiGetRdmaCommonInfoSize(), ptr, cnt, regMode);

    isRegistered = true;
  }

  int rootNode = -1; // -1 is the rootNode for p2p operations

  NcpyOperationInfo *ncpyOpInfo = createNcpyOpInfo(*this, destination, ackSize, srcAck, destAck, rootNode, CMK_DIRECT_API, (void *)ref);

  CmiIssueRput(ncpyOpInfo);
}

// Returns CmiNcpyMode::MEMCPY if both the PEs are the same and memcpy can be used
// Returns CmiNcpyMode::CMA if both the PEs are in the same physical node and CMA can be used
// Returns CmiNcpyMode::RDMA if RDMA needs to be used
CmiNcpyMode findTransferMode(int srcPe, int destPe) {
  if(CmiNodeOf(srcPe)==CmiNodeOf(destPe))
    return CmiNcpyMode::MEMCPY;
#if CMK_USE_CMA
  else if(useCMAForZC && CmiDoesCMAWork() && CmiPeOnSamePhysicalNode(srcPe, destPe))
    return CmiNcpyMode::CMA;
#endif
  else
    return CmiNcpyMode::RDMA;
}

CmiNcpyMode findTransferModeWithNodes(int srcNode, int destNode) {
  if(srcNode==destNode)
    return CmiNcpyMode::MEMCPY;
#if CMK_USE_CMA
  else if(useCMAForZC && CmiDoesCMAWork() && CmiPeOnSamePhysicalNode(CmiNodeFirst(srcNode), CmiNodeFirst(destNode)))
    return CmiNcpyMode::CMA;
#endif
  else
    return CmiNcpyMode::RDMA;
}

zcPupSourceInfo *zcPupAddSource(CmiNcpyBuffer &src) {
  zcPupSourceInfo *srcInfo = new zcPupSourceInfo();
  srcInfo->src = src;
  srcInfo->deallocate = free;
  return srcInfo;
}

zcPupSourceInfo *zcPupAddSource(CmiNcpyBuffer &src, std::function<void (void *)> deallocate) {
  zcPupSourceInfo *srcInfo = new zcPupSourceInfo();
  srcInfo->src = src;
  srcInfo->deallocate = deallocate;
  return srcInfo;
}

void zcPupDone(void *ref) {
  zcPupSourceInfo *srcInfo = (zcPupSourceInfo *)(ref);
#if CMK_REG_REQUIRED
  deregisterBuffer(srcInfo->src);
#endif

  srcInfo->deallocate((void *)srcInfo->src.ptr);
  delete srcInfo;
}

void zcPupHandler(ncpyHandlerMsg *msg) {
  zcPupDone(msg->ref);
}

void invokeZCPupHandler(void *ref, int pe) {
  ncpyHandlerMsg *msg = (ncpyHandlerMsg *)CmiAlloc(sizeof(ncpyHandlerMsg));
  msg->ref = (void *)ref;

  CmiSetHandler(msg, zc_pup_handler_idx);
  CmiSyncSendAndFree(pe, sizeof(ncpyHandlerMsg), (char *)msg);
}

void zcPupGet(CmiNcpyBuffer &src, CmiNcpyBuffer &dest) {
  CmiNcpyMode transferMode = findTransferMode(src.pe, dest.pe);
  if(transferMode == CmiNcpyMode::MEMCPY) {
    CmiAbort("zcPupGet: memcpyGet should not happen\n");
  }
#if CMK_USE_CMA
  else if(transferMode == CmiNcpyMode::CMA) {
    dest.cmaGet(src);

#if CMK_REG_REQUIRED
    // De-register destination buffer
    deregisterBuffer(dest);
#endif

    if(src.ref)
      invokeZCPupHandler((void *)src.ref, src.pe);
    else
      CmiAbort("zcPupGet - src.ref is NULL\n");
  }
#endif
  else {
    int ackSize = 0;
    int rootNode = -1; // -1 is the rootNode for p2p operations
    NcpyOperationInfo *ncpyOpInfo = dest.createNcpyOpInfo(src, dest, ackSize, NULL, NULL, rootNode, CMK_ZC_PUP, NULL);
    CpvAccess(newZCPupGets).push_back(ncpyOpInfo);
  }
}


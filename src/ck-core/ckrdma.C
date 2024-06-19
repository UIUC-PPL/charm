/*
 * Charm Onesided API Utility Functions
 */

#include "charm++.h"
#include "ck.h"
#include "converse.h"
#include "cmirdmautils.h"
#include "spanningTree.h"
#include <algorithm>
#include "ckarray.h"
#include "cklocation.h"
#include "conv-rdma.h"

void CmiFreeBroadcastAllExceptMeFn(int size, char *msg);

#if CMK_SMP
/*readonly*/ extern CProxy_ckcallback_group _ckcallbackgroup;
static int zcpy_pup_complete_handler_idx;
#endif

// Integer used to store the ncpy ack handler idx
static int ncpy_handler_idx;

CkpvExtern(ReqTagPostMap, ncpyPostedReqMap);
CkpvExtern(ReqTagBufferMap, ncpyPostedBufferMap);
CpvExtern(std::vector<NcpyOperationInfo *>, newZCPupGets);
CksvExtern(ObjNumRdmaOpsMap, pendingZCOps);
CksvExtern(CmiNodeLock, _nodeZCPendingLock);

CksvExtern(ReqTagPostMap, ncpyPostedReqNodeMap);
CksvExtern(ReqTagBufferMap, ncpyPostedBufferNodeMap);
CksvExtern(CmiNodeLock, _nodeZCPostReqLock);
CksvExtern(CmiNodeLock, _nodeZCBufferReqLock);

/*********************************** Zerocopy Direct API **********************************/

// Perform a nocopy get operation into this destination using the passed source
CkNcpyStatus CkNcpyBuffer::get(CkNcpyBuffer &source){
  if(regMode == CK_BUFFER_NOREG || source.regMode == CK_BUFFER_NOREG) {
    CkAbort("Cannot perform RDMA operations in CK_BUFFER_NOREG mode\n");
  }

  // Check that the source buffer fits into the destination buffer
  if(cnt < source.cnt)
    CkAbort("CkNcpyBuffer::get (destination.cnt < source.cnt) Destination buffer is smaller than the source buffer\n");

  // Check that this object is local when get is called
  CkAssert(CkNodeOf(pe) == CkMyNode());

  CkNcpyMode transferMode = findTransferMode(source.pe, pe);

  //Check if it is a within-process sending
  if(transferMode == CkNcpyMode::MEMCPY) {
    memcpyGet(source);

#if CMK_REG_REQUIRED
    // De-register source
    if(source.isRegistered && source.deregMode == CMK_BUFFER_DEREG)
      deregisterBuffer(source);
#endif

    //Invoke the source callback
    source.cb.send(sizeof(CkNcpyBuffer), &source);

#if CMK_REG_REQUIRED
    // De-register destination
    if(isRegistered && deregMode == CMK_BUFFER_DEREG)
      deregisterBuffer(*this);
#endif

    //Invoke the destination callback
    cb.send(sizeof(CkNcpyBuffer), this);

    // rdma data transfer complete
    return CkNcpyStatus::complete;

#if CMK_USE_CMA
  } else if(transferMode == CkNcpyMode::CMA) {

    cmaGet(source);

#if CMK_REG_REQUIRED
    // De-register source and invoke cb
    if(source.isRegistered && source.deregMode == CMK_BUFFER_DEREG)
      invokeCmaDirectRemoteDeregAckHandler(source, ncpyHandlerIdx::CMA_DEREG_ACK_DIRECT); // Send a message to de-register source buffer and invoke callback
    else
#endif
      source.cb.send(sizeof(CkNcpyBuffer), &source); //Invoke the source callback

#if CMK_REG_REQUIRED
    // De-register destination
    if(isRegistered && deregMode == CMK_BUFFER_DEREG)
      deregisterBuffer(*this);
#endif

    //Invoke the destination callback
    cb.send(sizeof(CkNcpyBuffer), this);

    // rdma data transfer complete
    return CkNcpyStatus::complete;

#endif // end of CMK_USE_CMA
  } else if (transferMode == CkNcpyMode::RDMA) {

    zcQdIncrement();

    rdmaGet(source, sizeof(CkCallback), (char *)&source.cb, (char *)&cb);

    // rdma data transfer incomplete
    return CkNcpyStatus::incomplete;

  } else {
    CkAbort("CkNcpyBuffer::get : Invalid CkNcpyMode");
  }
}

// Perform a nocopy put operation into the passed destination using this source
CkNcpyStatus CkNcpyBuffer::put(CkNcpyBuffer &destination){
  if(regMode == CK_BUFFER_NOREG || destination.regMode == CK_BUFFER_NOREG) {
    CkAbort("Cannot perform RDMA operations in CK_BUFFER_NOREG mode\n");
  }
  // Check that the source buffer fits into the destination buffer
  if(destination.cnt < cnt)
    CkAbort("CkNcpyBuffer::put (destination.cnt < source.cnt) Destination buffer is smaller than the source buffer\n");

  // Check that this object is local when put is called
  CkAssert(CkNodeOf(pe) == CkMyNode());

  CkNcpyMode transferMode = findTransferMode(pe, destination.pe);

  //Check if it is a within-process sending
  if(transferMode == CkNcpyMode::MEMCPY) {
    memcpyPut(destination);

#if CMK_REG_REQUIRED
    // De-register destination
    if(destination.isRegistered && destination.deregMode == CMK_BUFFER_DEREG)
      deregisterBuffer(destination);
#endif

    //Invoke the destination callback
    destination.cb.send(sizeof(CkNcpyBuffer), &destination);

#if CMK_REG_REQUIRED
    // De-register source
    if(isRegistered && deregMode == CMK_BUFFER_DEREG)
      deregisterBuffer(*this);
#endif

    //Invoke the source callback
    cb.send(sizeof(CkNcpyBuffer), this);

    // rdma data transfer complete
    return CkNcpyStatus::complete;

#if CMK_USE_CMA
  } else if(transferMode == CkNcpyMode::CMA) {
    cmaPut(destination);

#if CMK_REG_REQUIRED
    // De-register destination invoke cb
    if(destination.isRegistered && destination.deregMode == CMK_BUFFER_DEREG)
      invokeCmaDirectRemoteDeregAckHandler(destination, ncpyHandlerIdx::CMA_DEREG_ACK_DIRECT); // Send a message to de-register dest buffer and invoke callback
    else
#endif
      destination.cb.send(sizeof(CkNcpyBuffer), &destination); //Invoke the destination callback

#if CMK_REG_REQUIRED
    // De-register source
    if(isRegistered && deregMode == CMK_BUFFER_DEREG)
      deregisterBuffer(*this);
#endif

    //Invoke the source callback
    cb.send(sizeof(CkNcpyBuffer), this);

    // rdma data transfer complete
    return CkNcpyStatus::complete;

#endif
  } else if (transferMode == CkNcpyMode::RDMA) {

    zcQdIncrement();

    rdmaPut(destination, sizeof(CkCallback), (char *)&cb, (char *)&destination.cb);

    // rdma data transfer incomplete
    return CkNcpyStatus::incomplete;

  } else {
    CkAbort("CkNcpyBuffer::put : Invalid CkNcpyMode");
  }
}

// reconstruct the CkNcpyBuffer object for the source
void constructSourceBufferObject(NcpyOperationInfo *info, CkNcpyBuffer &src) {
  src.ptr = info->srcPtr;
  src.pe  = info->srcPe;
  src.cnt = info->srcSize;
  src.ref = info->srcRef;
  src.regMode = info->srcRegMode;
  src.deregMode = info->srcDeregMode;
  src.isRegistered = info->isSrcRegistered;
  memcpy((char *)(&src.cb), info->srcAck, info->srcAckSize); // initialize cb
  memcpy((char *)(src.layerInfo), info->srcLayerInfo, info->srcLayerSize); // initialize layerInfo
}

// reconstruct the CkNcpyBuffer object for the destination
void constructDestinationBufferObject(NcpyOperationInfo *info, CkNcpyBuffer &dest) {
  dest.ptr = info->destPtr;
  dest.pe  = info->destPe;
  dest.cnt = info->destSize;
  dest.ref = info->destRef;
  dest.regMode = info->destRegMode;
  dest.deregMode = info->destDeregMode;
  dest.isRegistered = info->isDestRegistered;
  memcpy((char *)(&dest.cb), info->destAck, info->destAckSize); // initialize cb
  memcpy((char *)(dest.layerInfo), info->destLayerInfo, info->destLayerSize); // initialize layerInfo
}

void invokeSourceCallback(NcpyOperationInfo *info) {
  CkCallback *srcCb = (CkCallback *)(info->srcAck);
  if(srcCb->requiresMsgConstruction()) {
    if(info->ackMode == CMK_SRC_DEST_ACK || info->ackMode == CMK_SRC_ACK) {
      CkNcpyBuffer src;
      constructSourceBufferObject(info, src);
      //Invoke the sender's callback
      invokeCallback(info->srcAck, info->srcPe, src);
    }
  }
}

void invokeDestinationCallback(NcpyOperationInfo *info) {
  CkCallback *destCb = (CkCallback *)(info->destAck);
  if(destCb->requiresMsgConstruction()) {
    if(info->ackMode == CMK_SRC_DEST_ACK || info->ackMode == CMK_DEST_ACK) {
      CkNcpyBuffer dest;
      constructDestinationBufferObject(info, dest);
      //Invoke the receiver's callback
      invokeCallback(info->destAck, info->destPe, dest);
    }
  }
}

void handleDirectApiCompletion(NcpyOperationInfo *info) {

  int freeMe = info->freeMe;

  if(CmiMyNode() == CmiNodeOf(info->destPe)) {
#if CMK_REG_REQUIRED
    if(info->isDestRegistered == 1 && info->destDeregMode == CK_BUFFER_DEREG)
      deregisterDestBuffer(info);
#endif

    // Invoke the destination callback
    invokeDestinationCallback(info);

#if CMK_REG_REQUIRED
    // send a message to the source to de-register and invoke callback
    if(info->isSrcRegistered == 1 && info->srcDeregMode == CK_BUFFER_DEREG) {
      freeMe = CMK_DONT_FREE_NCPYOPINFO; // don't free info here, it'll be freed by the machine layer
      QdCreate(1); // Matching QdProcess in CkRdmaDirectAckHandler
      CmiInvokeRemoteDeregAckHandler(info->srcPe, info);
    } else
#endif
      invokeSourceCallback(info);
  }

  if(CmiMyNode() == CmiNodeOf(info->srcPe)) {
#if CMK_REG_REQUIRED
    if(info->isSrcRegistered == 1 && info->srcDeregMode == CK_BUFFER_DEREG)
      deregisterSrcBuffer(info);
#endif

    // Invoke the source callback
    invokeSourceCallback(info);

#if CMK_REG_REQUIRED
    // send a message to the destination to de-register and invoke callback
    if(info->isDestRegistered == 1 && info->destDeregMode == CK_BUFFER_DEREG) {
      freeMe = CMK_DONT_FREE_NCPYOPINFO; // don't free info here, it'll be freed by the machine layer
      QdCreate(1); // Matching QdProcess in CkRdmaDirectAckHandler
      CmiInvokeRemoteDeregAckHandler(info->destPe, info);
    } else
#endif
      invokeDestinationCallback(info);
  }

  if(freeMe == CMK_FREE_NCPYOPINFO)
    CmiFree(info);
}

// Ack handler function which invokes the callback
void CkRdmaDirectAckHandler(void *ack) {

  // Process QD to mark completion of the outstanding RDMA operation
  QdProcess(1);

  NcpyOperationInfo *info = (NcpyOperationInfo *)(ack);

  switch(info->opMode) {
    case CMK_DIRECT_API             : handleDirectApiCompletion(info); // Ncpy Direct API
                                      break;

    case CMK_EM_API                 : handleEntryMethodApiCompletion(info); // Ncpy EM API invoked through a GET
                                      break;

    case CMK_EM_API_SRC_ACK_INVOKE  : invokeSourceCallback(info);
                                      break;

    case CMK_EM_API_DEST_ACK_INVOKE : invokeDestinationCallback(info);
                                      break;

    case CMK_EM_API_REVERSE         : handleReverseEntryMethodApiCompletion(info); // Ncpy EM API invoked through a PUT
                                      break;

    case CMK_BCAST_EM_API           : handleBcastEntryMethodApiCompletion(info); // Ncpy EM Bcast API
                                      break;

    case CMK_BCAST_EM_API_REVERSE   : handleBcastReverseEntryMethodApiCompletion(info); // Ncpy EM Bcast API invoked through a PUT
                                      break;

    case CMK_READONLY_BCAST         : readonlyGetCompleted(info);
                                      break;

    case CMK_ZC_PUP                 : zcPupGetCompleted(info);
                                      break;

    default                         : CkAbort("CkRdmaDirectAckHandler: Unknown ncpyOpInfo->opMode");
                                      break;
  }
}

// Helper methods
void invokeCallback(CkNcpyBuffer &buff) {
#if CMK_SMP
    //call to callbackgroup to call the callback when calling from comm thread
    //this adds one more trip through the scheduler
    _ckcallbackgroup[buff.pe].call(buff.cb, sizeof(CkNcpyBuffer), (const char *)(&buff));
#else
    //Invoke the callback
    buff.cb.send(sizeof(CkNcpyBuffer), &buff);
#endif
}

void invokeCallback(void *cb, int pe, CkNcpyBuffer &buff) {
#if CMK_SMP
    //call to callbackgroup to call the callback when calling from comm thread
    //this adds one more trip through the scheduler
    _ckcallbackgroup[pe].call(*(CkCallback *)(cb), sizeof(CkNcpyBuffer), (const char *)(&buff));
#else
    //Invoke the destination callback
    ((CkCallback *)(cb))->send(sizeof(CkNcpyBuffer), &buff);
#endif
}

void enqueueNcpyMessage(int destPe, void *msg){
  // invoke the charm message handler to enqueue the messsage
#if CMK_SMP && !CMK_ENABLE_ASYNC_PROGRES
  if(destPe == CkMyPe()) // invoked from the same worker thread, call message handler directly
    CmiHandleMessage(msg);
  else                   // invoked from the comm thread, so send message to the worker thread
    CmiPushPE(CmiRankOf(destPe), msg);
#else
  // invoked from the same logical node (process), call message handler directly
  // or invoked from the same worker thread, call message handler directly
  CmiHandleMessage(msg);
#endif
}

inline void zcQdIncrement() {
    int outstandingRdmaOps = 1; // used by true-RDMA layers

#if CMK_ONESIDED_IMPL
#if CMK_CONVERSE_MPI
    outstandingRdmaOps += 1; // MPI layer invokes CmiDirectAckHandler twice as sender and receiver post separately
#endif
#else
    outstandingRdmaOps += 1; // non-RDMA layers invoke CmiDirectAckHandler twice using regular messages
#endif

    // Create QD to ensure that outstanding rdmaGet call is accounted for
    QdCreate(outstandingRdmaOps);
}


/*********************************** Zerocopy Entry Method API ****************************/

/************************* Zerocopy Entry Method API - Utility functions ******************/

void performRget(NcpyEmInfo *ref, int index, int extraSize) {
  // Launch rget
  NcpyEmBufferInfo *ncpyEmBufferInfo = (NcpyEmBufferInfo *)((char *)ref + sizeof(NcpyEmInfo) + index *(sizeof(NcpyEmBufferInfo) + extraSize));
  NcpyOperationInfo *ncpyOpInfo = &(ncpyEmBufferInfo->ncpyOpInfo);
  zcQdIncrement();
  CmiIssueRget(ncpyOpInfo);
}

void performRgets(NcpyEmInfo *ref, int numops, int extraSize) {
  // Launch rgets
  for(int i=0; i<numops; i++){
    performRget(ref, i, extraSize);
  }
}

CmiSpanningTreeInfo* getSpanningTreeInfo(int startNode) {
  return (startNode == 0) ? _topoTree : ST_RecursivePartition_getTreeInfo(startNode);
}

inline bool isDeregReady(CkNcpyBuffer &buffInfo) {
#if CMK_REG_REQUIRED
  return (buffInfo.regMode != CK_BUFFER_UNREG && buffInfo.deregMode != CK_BUFFER_NODEREG);
#endif
  return false;
}

inline void deregisterDestBuffer(NcpyOperationInfo *ncpyOpInfo) {
  CmiDeregisterMem(ncpyOpInfo->destPtr, ncpyOpInfo->destLayerInfo + CmiGetRdmaCommonInfoSize(), ncpyOpInfo->destPe, ncpyOpInfo->destRegMode);
  ncpyOpInfo->isDestRegistered = 0;
}

inline void deregisterSrcBuffer(NcpyOperationInfo *ncpyOpInfo) {
  CmiDeregisterMem(ncpyOpInfo->srcPtr, ncpyOpInfo->srcLayerInfo + CmiGetRdmaCommonInfoSize(), ncpyOpInfo->srcPe, ncpyOpInfo->srcRegMode);
  ncpyOpInfo->isSrcRegistered = 0;
}

// Method called on completion of an Zcpy EM API (Send or Recv, P2P or BCAST)
void CkRdmaEMAckHandler(int destPe, void *ack) {

  if(_topoTree == NULL) CkAbort("CkRdmaIssueRgets:: topo tree has not been calculated \n");

  NcpyEmBufferInfo *emBuffInfo = (NcpyEmBufferInfo *)(ack);

  char *ref = (char *)(emBuffInfo);

  int layerInfoSize = CMK_COMMON_NOCOPY_DIRECT_BYTES + CMK_NOCOPY_DIRECT_BYTES;
  int ncpyObjSize = getNcpyOpInfoTotalSize(
                    layerInfoSize,
                    sizeof(CkCallback),
                    layerInfoSize,
                    0);


  NcpyEmInfo *ncpyEmInfo = (NcpyEmInfo *)(ref - (emBuffInfo->index) * (sizeof(NcpyEmBufferInfo) + ncpyObjSize - sizeof(NcpyOperationInfo)) - sizeof(NcpyEmInfo));
  ncpyEmInfo->counter++; // Operation completed, update counter

#if CMK_REG_REQUIRED
  CmiSpanningTreeInfo *t = NULL;
  if(ncpyEmInfo->mode == ncpyEmApiMode::BCAST_SEND || ncpyEmInfo->mode == ncpyEmApiMode::BCAST_RECV)
    t = getSpanningTreeInfo(getRootNode((envelope *)ncpyEmInfo->msg));

  NcpyOperationInfo *ncpyOpInfo = &(emBuffInfo->ncpyOpInfo);

  if(ncpyEmInfo->mode == ncpyEmApiMode::P2P_SEND ||
     (ncpyEmInfo->mode == ncpyEmApiMode::BCAST_SEND && t->child_count == 0)) {  // EM P2P Send API or EM BCAST Send API

    // De-register the destination buffer
    deregisterDestBuffer(ncpyOpInfo);

  } else if(ncpyEmInfo->mode == ncpyEmApiMode::P2P_RECV ||
           (ncpyEmInfo->mode == ncpyEmApiMode::BCAST_RECV && t->child_count == 0)) {  // EM P2P Post API or EM BCAST Post API

    // De-register only if destDeregMode is CK_BUFFER_DEREG
    if(ncpyOpInfo->destDeregMode == CK_BUFFER_DEREG) {
      deregisterDestBuffer(ncpyOpInfo);
    }
  }
#endif

  if(ncpyEmInfo->counter == ncpyEmInfo->numOps) {
    // All operations have been completed

    switch(ncpyEmInfo->mode) {
      case ncpyEmApiMode::P2P_SEND    : enqueueNcpyMessage(destPe, ncpyEmInfo->msg);
                                        break;

      case ncpyEmApiMode::P2P_RECV    : // Since P2P_RECV messages are enqueued twice (first with Post EM
                                        // and the next time with Regular EM), hence QdCounter should be added
                                        QdCreate(1);
                                        CMI_ZC_MSGTYPE(ncpyEmInfo->msg) = CMK_REG_NO_ZC_MSG;
                                        enqueueNcpyMessage(destPe, ncpyEmInfo->msg);
                                        CmiFree(ncpyEmInfo);
                                        break;

      case ncpyEmApiMode::BCAST_SEND  : processBcastSendEmApiCompletion(ncpyEmInfo, destPe);
                                        break;

      case ncpyEmApiMode::BCAST_RECV  : processBcastRecvEmApiCompletion(ncpyEmInfo, destPe);
                                        break;

      default                         : CmiAbort("Invalid operation mode");
                                        break;
    }
  }
}

void performEmApiMemcpy(CkNcpyBuffer &source, CkNcpyBuffer &dest, ncpyEmApiMode emMode) {
  dest.memcpyGet(source);

  if(emMode == ncpyEmApiMode::P2P_SEND || emMode == ncpyEmApiMode::P2P_RECV) {

    // De-register source
    if(isDeregReady(source))
      deregisterBuffer(source);

    // De-register destination for p2p Post API
    if(emMode == ncpyEmApiMode::P2P_RECV && isDeregReady(dest))
      deregisterBuffer(dest);

    // Invoke source callback
    source.cb.send(sizeof(CkNcpyBuffer), &source);

  } // send a message to the parent to indicate completion
  else if (emMode == ncpyEmApiMode::BCAST_SEND || emMode == ncpyEmApiMode::BCAST_RECV) {
    // De-register dest if it has been registered
    if(emMode == ncpyEmApiMode::BCAST_RECV && isDeregReady(dest))
      deregisterBuffer(dest);
  }
}

#if CMK_USE_CMA
void performEmApiCmaTransfer(CkNcpyBuffer &source, CkNcpyBuffer &dest, CmiSpanningTreeInfo *t, ncpyEmApiMode emMode) {
  dest.cmaGet(source);

  if(emMode == ncpyEmApiMode::P2P_SEND || emMode == ncpyEmApiMode::P2P_RECV) {

    // De-register destination for p2p Post API
    if(emMode == ncpyEmApiMode::P2P_RECV && isDeregReady(dest))
      deregisterBuffer(dest);

    if(source.refAckInfo == NULL) { // Not a part of a de-registration group
      // Invoke source callback
      source.cb.send(sizeof(CkNcpyBuffer), &source);
    }
  }
  else if (emMode == ncpyEmApiMode::BCAST_SEND || emMode == ncpyEmApiMode::BCAST_RECV) {
    if(t->child_count != 0) {
      if(dest.regMode == CK_BUFFER_UNREG) {
        // register it because it is required for RGET performed by child nodes
        CmiSetRdmaBufferInfo(dest.layerInfo + CmiGetRdmaCommonInfoSize(), dest.ptr, dest.cnt, dest.regMode);
        dest.isRegistered = true;
      }
      // Buffers on intermediate nodes are left registered to have their child nodes rget from them
    } else {
      // De-register dest on child nodes if it has been registered
      if(emMode == ncpyEmApiMode::BCAST_RECV && isDeregReady(dest))
        deregisterBuffer(dest);
    }
  }
}
#endif

void performEmApiRget(CkNcpyBuffer &source, CkNcpyBuffer &dest, int opIndex, NcpyEmInfo *ref, int extraSize, int rootNode, ncpyEmApiMode emMode) {

  int layerInfoSize = CMK_COMMON_NOCOPY_DIRECT_BYTES + CMK_NOCOPY_DIRECT_BYTES;
  if(dest.regMode == CK_BUFFER_UNREG) {
    // register it because it is required for RGET
    CmiSetRdmaBufferInfo(dest.layerInfo + CmiGetRdmaCommonInfoSize(), dest.ptr, dest.cnt, dest.regMode);

    dest.isRegistered = true;
  }

  NcpyEmBufferInfo *ncpyEmBufferInfo = (NcpyEmBufferInfo *)((char *)ref + sizeof(NcpyEmInfo) + opIndex *(sizeof(NcpyEmBufferInfo) + extraSize));
  ncpyEmBufferInfo->index = opIndex;

  NcpyOperationInfo *ncpyOpInfo = &(ncpyEmBufferInfo->ncpyOpInfo);
  setNcpyOpInfo(source.ptr,
                (char *)(source.layerInfo),
                layerInfoSize,
                (char *)(&source.cb),
                sizeof(CkCallback),
                source.cnt,
                source.regMode,
                source.deregMode,
                source.isRegistered,
                source.pe,
                source.ref,
                dest.ptr,
                (char *)(dest.layerInfo),
                layerInfoSize,
                NULL,
                0,
                dest.cnt,
                dest.regMode,
                dest.deregMode,
                dest.isRegistered,
                dest.pe,
                (char *)(ncpyEmBufferInfo), // destRef
                rootNode,
                ncpyOpInfo);

  // set opMode
  if(emMode == ncpyEmApiMode::BCAST_SEND || emMode == ncpyEmApiMode::BCAST_RECV)
    ncpyOpInfo->opMode = CMK_BCAST_EM_API;  // mode for bcast
  else if(emMode == ncpyEmApiMode::P2P_SEND || emMode == ncpyEmApiMode::P2P_RECV)
    ncpyOpInfo->opMode = CMK_EM_API;  // mode for p2p
  else
    CmiAbort("Invalid Mode\n");

  ncpyOpInfo->freeMe = CMK_DONT_FREE_NCPYOPINFO; // Since ncpyOpInfo is a part of the charm message, don't explicitly free it
                                                 // It'll be freed when the message is freed by the RTS after the execution of the entry method
  ncpyOpInfo->refPtr = ncpyEmBufferInfo;

  // Do not launch Rgets here as they could potentially cause a race condition in the SMP mode
  // The race condition is caused when an RGET completes and invokes the CkRdmaDirectAckHandler
  // on the comm. thread as the message is being inside this for loop on the worker thread
}

void performEmApiNcpyTransfer(CkNcpyBuffer &source, CkNcpyBuffer &dest, int opIndex, CmiSpanningTreeInfo *t, NcpyEmInfo *ref, int extraSize, CkNcpyMode ncpyMode, int rootNode, ncpyEmApiMode emMode){

  switch(ncpyMode) {
    case CkNcpyMode::MEMCPY: performEmApiMemcpy(source, dest, emMode);
                                   break;
#if CMK_USE_CMA
    case CkNcpyMode::CMA   : performEmApiCmaTransfer(source, dest, t, emMode);
                                   break;
#endif
    case CkNcpyMode::RDMA  : performEmApiRget(source, dest, opIndex, ref, extraSize, rootNode, emMode);
                                   break;

    default                      : CkAbort("Invalid Mode");
                                   break;
  }
}


void preprocessRdmaCaseForRgets(int &layerInfoSize, int &ncpyObjSize, int &extraSize, int &totalMsgSize, int &numops) {
    layerInfoSize = CMK_COMMON_NOCOPY_DIRECT_BYTES + CMK_NOCOPY_DIRECT_BYTES;

    ncpyObjSize = getNcpyOpInfoTotalSize(
                  layerInfoSize,
                  sizeof(CkCallback),
                  layerInfoSize,
                  0);

    extraSize = ncpyObjSize - sizeof(NcpyOperationInfo);

    totalMsgSize += sizeof(NcpyEmInfo) + numops*(sizeof(NcpyEmBufferInfo) + extraSize);
}

void setNcpyEmInfo(NcpyEmInfo *ncpyEmInfo, envelope *env, int &numops, void *forwardMsg, ncpyEmApiMode emMode) {

    ncpyEmInfo->numOps = numops;
    ncpyEmInfo->counter = 0;
    ncpyEmInfo->msg = env;

    ncpyEmInfo->forwardMsg = forwardMsg; // useful only for Send Bcast, NULL for others
    ncpyEmInfo->pe = CkMyPe();
    ncpyEmInfo->mode = emMode; // P2P or BCAST

    ncpyEmInfo->tagArray = nullptr;
    ncpyEmInfo->peerAckInfo = nullptr;
}

/* Zerocopy Entry Method API Functions */
// Method called to unpack rdma pointers
void CkPackRdmaPtrs(char *msgBuf){
  PUP::toMem p((void *)msgBuf);
  PUP::fromMem up((void *)msgBuf);
  int numops, rootNode;
  up|numops;
  up|rootNode;
  p|numops;
  p|rootNode;

  // Pack ncpy pointers in CkNcpyBuffer
  for(int i=0; i<numops; i++){
    CkNcpyBuffer w;
    up|w;
    w.ptr = (void *)((char *)w.ptr - (char *)msgBuf);
    p|w;
  }
}

// Method called to unpack rdma pointers
void CkUnpackRdmaPtrs(char *msgBuf){
  PUP::toMem p((void *)msgBuf);
  PUP::fromMem up((void *)msgBuf);
  int numops, rootNode;
  up|numops;
  up|rootNode;
  p|numops;
  p|rootNode;

  // Unpack ncpy pointers in CkNcpyBuffer
  for(int i=0; i<numops; i++){
    CkNcpyBuffer w;
    up|w;
    w.ptr = (void *)((char *)msgBuf + (size_t)w.ptr);
    p|w;
  }
}


// Determine the number of ncpy ops and the sum of the ncpy buffer sizes
// from the metadata message
void getRdmaNumopsAndBufsize(envelope *env, int &numops, int &bufsize, int &rootNode) {
  numops = 0;
  bufsize = 0;
  PUP::fromMem up((void *)((CkMarshallMsg *)EnvToUsr(env))->msgBuf);
  up|numops;
  up|rootNode;
  for(int i=0; i<numops; i++){
    CkNcpyBuffer w;
    up|w;
    bufsize += CK_ALIGN(w.cnt, 16);
  }
}

int getSrcPe(envelope *env) {
  int numops, rootNode;
  PUP::fromMem up((void *)((CkMarshallMsg *)EnvToUsr(env))->msgBuf);
  up|numops;
  up|rootNode;
  CkEnforce(numops > 0);
  CkNcpyBuffer w;
  up|w;
  return w.pe;
}

int getRootNode(envelope *env) {
  int numops;
  int rootNode;
  PUP::fromMem up((void *)((CkMarshallMsg *)EnvToUsr(env))->msgBuf);
  up|numops;
  up|rootNode;
  return rootNode;
}

void handleEntryMethodApiCompletion(NcpyOperationInfo *info) {

#if CMK_REG_REQUIRED
  // send a message to the source to de-register and invoke callback
  if(info->srcDeregMode == CK_BUFFER_DEREG) {
    QdCreate(1); // Matching QdProcess in CkRdmaDirectAckHandler
    CmiInvokeRemoteDeregAckHandler(info->srcPe, info);
  } else // Do not de-register source when srcDeregMode != CK_BUFFER_DEREG
#endif
    invokeSourceCallback(info);

  if(info->ackMode == CMK_SRC_DEST_ACK || info->ackMode == CMK_DEST_ACK) {
    // Invoke the ackhandler function to update the counter
    CkRdmaEMAckHandler(info->destPe, info->refPtr);
  }
}

void handleReverseEntryMethodApiCompletion(NcpyOperationInfo *info) {

  if(info->ackMode == CMK_SRC_DEST_ACK || info->ackMode == CMK_DEST_ACK) {
    // Send a message to the receiver to invoke CkRdmaEMAckHandler to update the counter
    invokeRemoteNcpyAckHandler(info->destPe, info->refPtr, ncpyHandlerIdx::EM_ACK);
  }

#if CMK_REG_REQUIRED
  // De-register source only when srcDeregMode == CK_BUFFER_DEREG
  if(info->srcDeregMode == CK_BUFFER_DEREG) {
    deregisterSrcBuffer(info);
  }
#endif

  invokeSourceCallback(info);

  if(info->freeMe == CMK_FREE_NCPYOPINFO)
    CmiFree(info);
}

/******************************* Zerocopy P2P EM SEND API Functions ***********************/

/*
 * Extract ncpy buffer information from the metadata message, used passed buffers
 * and issue ncpy calls (either memcpy or cma read or rdma get). Main method called on
 * the destination to perform zerocopy operations as a part of the Zerocopy Entry Method
 * API
 */
envelope* CkRdmaIssueRgets(envelope *env, ncpyEmApiMode emMode, void *forwardMsg){

  int numops=0, bufsize=0, msgsize=0, rootNode = -1;

  CkUnpackMessage(&env); // Unpack message to access msgBuf inside getRdmaNumopsAndBufsize
  getRdmaNumopsAndBufsize(env, numops, bufsize, rootNode);
  CkPackMessage(&env); // Pack message to ensure corret copying into copyenv

  msgsize = env->getTotalsize();
  int totalMsgSize = CK_ALIGN(msgsize, 16) + bufsize;
  NcpyEmInfo *ncpyEmInfo;
  int layerInfoSize, ncpyObjSize, extraSize;

  CmiSpanningTreeInfo *t = NULL;

  CkNcpyMode ncpyMode = findTransferMode(env->getSrcPe(), CkMyPe());
  if(_topoTree == NULL) CkAbort("CkRdmaIssueRgets:: topo tree has not been calculated \n");
  if(emMode == ncpyEmApiMode::BCAST_SEND || emMode == ncpyEmApiMode::BCAST_RECV)
    t = getSpanningTreeInfo(rootNode);

  layerInfoSize = CMK_COMMON_NOCOPY_DIRECT_BYTES + CMK_NOCOPY_DIRECT_BYTES;

  if(ncpyMode == CkNcpyMode::RDMA) {
    preprocessRdmaCaseForRgets(layerInfoSize, ncpyObjSize, extraSize, totalMsgSize, numops);
  }

  // Allocate the new message which stores the receiver buffers
  envelope *copyenv = (envelope *)CmiAlloc(totalMsgSize);

  //Copy the metadata message(without the machine specific info) into the buffer
  memcpy(copyenv, env, msgsize);

  /* Set the total size of the message excluding the receiver's machine specific info
   * which is not required when the receiver's entry method executes
   */
  copyenv->setTotalsize(totalMsgSize);

  if(ncpyMode == CkNcpyMode::RDMA) {
    ncpyEmInfo = (NcpyEmInfo *)((char *)copyenv + CK_ALIGN(msgsize, 16) + bufsize);
    setNcpyEmInfo(ncpyEmInfo, copyenv, numops, forwardMsg, emMode);
  }

  char *buf = (char *)copyenv + CK_ALIGN(msgsize, 16);

  CkUnpackMessage(&copyenv);

  // Mark the message to be a SEND_DONE message to prevent message handler on the receiver
  // from intercepting it and pack pointers when forwarded to peers/children
  CMI_ZC_MSGTYPE(copyenv) = CMK_ZC_SEND_DONE_MSG;

  PUP::toMem p((void *)(((CkMarshallMsg *)EnvToUsr(copyenv))->msgBuf));
  PUP::fromMem up((void *)((CkMarshallMsg *)EnvToUsr(copyenv))->msgBuf);
  up|numops;
  up|rootNode;
  p|numops;
  p|rootNode;

  // source buffer
  CkNcpyBuffer source;

  bool sendBackToSourceForDereg = false; // used for CMA transfers

  for(int i=0; i<numops; i++){
    up|source;

#if CMK_USE_CMA && CMK_REG_REQUIRED
    if(!sendBackToSourceForDereg && ncpyMode == CkNcpyMode::CMA && source.refAckInfo != NULL)
      sendBackToSourceForDereg = true;
#endif

    // destination buffer
    CkNcpyBuffer dest((const void *)buf, source.cnt, CK_BUFFER_UNREG);

    performEmApiNcpyTransfer(source, dest, i, t, ncpyEmInfo, extraSize, ncpyMode, rootNode, emMode);

    //Update the CkRdmaWrapper pointer of the new message
    source.ptr = buf;

    source.isRegistered = dest.isRegistered;

    source.regMode = dest.regMode;

    source.deregMode = dest.deregMode;

    memcpy(source.layerInfo, dest.layerInfo, layerInfoSize);

    //Update the pointer
    buf += CK_ALIGN(source.cnt, 16);
    p|source;
  }


  if(emMode == ncpyEmApiMode::P2P_SEND) {
    switch(ncpyMode) {
      case CkNcpyMode::MEMCPY:  return copyenv;
                                break;
      case CkNcpyMode::CMA   :  if(sendBackToSourceForDereg) {
                                  // Send back to source process to de-register
                                  invokeRemoteNcpyAckHandler(source.pe, (void *)source.refAckInfo, ncpyHandlerIdx::CMA_DEREG_ACK);
                                }
                                return copyenv;
                                break;

      case CkNcpyMode::RDMA  :  performRgets(ncpyEmInfo, numops, extraSize);
                                break;

      default                :  CmiAbort("Invalid transfer mode\n");
                                break;
    }
  } else if(emMode == ncpyEmApiMode::BCAST_SEND) {
    switch(ncpyMode) {
      case CkNcpyMode::MEMCPY:  // Invoke the bcast Ack Handler after 'numops' memcpy operations are complete
                                CkAssert(source.refAckInfo != NULL);
                                CkRdmaEMBcastAckHandler((void *)source.refAckInfo);
                                forwardMessageToPeerNodes(copyenv, copyenv->getMsgtype());
                                return copyenv;
                                break;

      case CkNcpyMode::CMA   :  CkPackMessage(&copyenv);
                                handleMsgUsingCMAPostCompletionForSendBcast(copyenv, env, source);
                                break;

      case CkNcpyMode::RDMA  :  performRgets(ncpyEmInfo, numops, extraSize);
                                break;

      default                :  CmiAbort("Invalid transfer mode\n");
                                break;
    }
  } else {
    CmiAbort("Invalid operation mode\n");
  }
  return NULL;
}


/******************************* Zerocopy P2P EM RECV API Functions ***********************/
/*
 * Extract ncpy buffer information from the metadata message
 * and issue ncpy calls (either memcpy or cma read or rdma get). Main method called on
 * the destination to perform zerocopy operations as a part of the Zerocopy Entry Method
 * API
 */
void CkRdmaIssueRgets(envelope *env, void **arrPtrs, int *arrSizes, int arrayIndex, CkNcpyBufferPost *postStructs){

  int numops, rootNode;
  int refSize = 0;
  NcpyEmInfo *ncpyEmInfo = postStructs[0].ncpyEmInfo;
  int layerInfoSize, ncpyObjSize, extraSize;

  ncpyEmApiMode emMode = (CMI_ZC_MSGTYPE(env) == CMK_ZC_BCAST_RECV_MSG) ? ncpyEmApiMode::BCAST_RECV : ncpyEmApiMode::P2P_RECV;

  CkNcpyMode ncpyMode = findTransferMode(getSrcPe(env), CkMyPe());
  CmiSpanningTreeInfo *t = NULL;

  layerInfoSize = CMK_COMMON_NOCOPY_DIRECT_BYTES + CMK_NOCOPY_DIRECT_BYTES;

  PUP::toMem p((void *)(((CkMarshallMsg *)EnvToUsr(env))->msgBuf));
  PUP::fromMem up((void *)((CkMarshallMsg *)EnvToUsr(env))->msgBuf);
  up|numops;
  up|rootNode;
  p|numops;
  p|rootNode;

  if(emMode == ncpyEmApiMode::BCAST_SEND || emMode == ncpyEmApiMode::BCAST_RECV) {
    if(_topoTree == NULL) CkAbort("CkRdmaIssueRgets:: topo tree has not been calculated \n");
    t = getSpanningTreeInfo(rootNode);
  }

  if(ncpyMode == CkNcpyMode::RDMA) {
    preprocessRdmaCaseForRgets(layerInfoSize, ncpyObjSize, extraSize, refSize, numops);
  }

  // source buffer
  CkNcpyBuffer source;

  bool sendBackToSourceForDereg = false;

  for(int i=0; i<numops; i++){
    up|source;

    if(source.cnt < arrSizes[i])
      CkAbort("CkRdmaIssueRgets: Size of the posted buffer > Size of the source buffer\n");

#if CMK_USE_CMA && CMK_REG_REQUIRED
    if(!sendBackToSourceForDereg && ncpyMode == CkNcpyMode::CMA && source.refAckInfo != NULL)
      sendBackToSourceForDereg = true;
#endif

    // destination buffer
    CkNcpyBuffer dest((const void *)arrPtrs[i], arrSizes[i], postStructs[i].regMode, postStructs[i].deregMode);

    performEmApiNcpyTransfer(source, dest, i, t, ncpyEmInfo, extraSize, ncpyMode, rootNode, emMode);

    //Update the CkRdmaWrapper pointer of the new message
    source.ptr = arrPtrs[i];

    source.isRegistered = dest.isRegistered;

    source.regMode = dest.regMode;

    source.deregMode = dest.deregMode;

    source.ncpyEmInfo = postStructs[i].ncpyEmInfo;

    memcpy(source.layerInfo, dest.layerInfo, layerInfoSize);

    p|source;
  }


  if(emMode == ncpyEmApiMode::P2P_RECV) {
    // Make the message a regular message to prevent message handler on the receiver
    // from intercepting it
    CMI_ZC_MSGTYPE(env) = CMK_REG_NO_ZC_MSG;

    switch(ncpyMode) {
      case CkNcpyMode::MEMCPY:  QdCreate(1);
                                enqueueNcpyMessage(CkMyPe(), env);
                                CmiFree(ncpyEmInfo);
                                break;
      case CkNcpyMode::CMA   :  if(sendBackToSourceForDereg) {
                                  // Send back to source process to de-register
                                  invokeRemoteNcpyAckHandler(source.pe, (void *)source.refAckInfo, ncpyHandlerIdx::CMA_DEREG_ACK);
                                }
                                QdCreate(1);
                                enqueueNcpyMessage(CkMyPe(), env);
                                CmiFree(ncpyEmInfo);
                                break;

      case CkNcpyMode::RDMA  :  performRgets(ncpyEmInfo, numops, extraSize);
                                break;

      default                :  CmiAbort("Invalid transfer mode\n");
                                break;
    }
  } else if(emMode == ncpyEmApiMode::BCAST_RECV) {
    switch(ncpyMode) {
      case CkNcpyMode::MEMCPY:  // Invoke the bcast Ack Handler after 'numops' memcpy operations are complete
                                CkAssert(source.refAckInfo != NULL);
                                CkRdmaEMBcastAckHandler((void *)source.refAckInfo);
                                handleMsgOnChildPostCompletionForRecvBcast(env, nullptr);
                                break;

      case CkNcpyMode::CMA   :  // Invoke the Ack handler on the parent node to signal completion
                                sendAckMsgToParent(env);
                                if(t->child_count == 0) {
                                  handleMsgOnChildPostCompletionForRecvBcast(env, nullptr);
                                } else {
                                  // Allocate a structure NcpyBcastInterimAckInfo to maintain state for ack handling
                                  NcpyBcastInterimAckInfo *bcastAckInfo = allocateInterimNodeAckObj(env, NULL, CkMyPe());
                                  handleMsgOnInterimPostCompletionForRecvBcast(env, bcastAckInfo, CkMyPe());
                                }
                                break;

      case CkNcpyMode::RDMA  :  performRgets(ncpyEmInfo, numops, extraSize);
                                break;

      default                :  CmiAbort("Invalid transfer mode\n");
                                break;
    }
  } else {
    CmiAbort("Invalid operation mode\n");
  }
}

/***************************** Zerocopy Bcast Entry Method API ****************************/

/********************** Zerocopy Bcast Entry Method API - Utility functions ***************/

void CkRdmaPrepareZCMsg(envelope *env, int node) {
  if(CMI_IS_ZC_BCAST(env)) {
    CkRdmaPrepareBcastMsg(env);

// De-registration in this case is not applicable for non-CMA supported layers or layers not requiring registration
#if CMK_USE_CMA && CMK_REG_REQUIRED
  } else if(CMI_IS_ZC_P2P(env) && env->getSrcPe() == CkMyPe()) {
    // The condition env->getSrcPe() == CkMyPe() was added because messages for chare arrays
    // are forwarded after migration and there is a possibility of routing messages through
    // the chare's previous home PE. In such cases, we don't want to do anything, i.e. we prepare
    // the P2P message only on the source buffer's PE (when env->getSrcPe() == CkMyPe())
    CkNcpyMode transferMode = findTransferModeWithNodes(CkMyNode(), node);
    if(transferMode == CkNcpyMode::CMA)
      CkRdmaPrepareP2PMsg(env);
#endif
  }
}

#if CMK_USE_CMA && CMK_REG_REQUIRED
void CkRdmaEMDeregAndAckDirectHandler(void *ack) {

  CkNcpyBuffer buffInfo;
  PUP::fromMem implP(ack);

  implP|buffInfo;

  // De-register source buffer
  deregisterBuffer(buffInfo);

  // Invoke Callback
  invokeCallback(buffInfo);
}

void CkRdmaEMDeregAndAckHandler(void *ack) {

  NcpyP2PAckInfo *p2pAckInfo = (NcpyP2PAckInfo *)ack;

  CkEnforce(p2pAckInfo->numOps > 0);

  for(int i = 0; i < p2pAckInfo->numOps; i++) {
    CkNcpyBuffer &source = p2pAckInfo->src[i];

    // De-register source buffer
    deregisterBuffer(source);

    // Invoke Callback
    invokeCallback(source);
  }
}

void CkRdmaPrepareP2PMsg(envelope *env) {
  int numops, rootNode;
  CkUnpackMessage(&env);
  PUP::toMem p((void *)(((CkMarshallMsg *)EnvToUsr(env))->msgBuf));
  PUP::fromMem up((void *)((CkMarshallMsg *)EnvToUsr(env))->msgBuf);

  up|numops;
  up|rootNode;

  int numToBeDeregOps = 0;

  // Determine number of zc ops for which de-reg is required
  for(int i=0; i<numops; i++) {
    CkNcpyBuffer source;
    up|source;
    if(isDeregReady(source))
      numToBeDeregOps++;
  }


  if(numToBeDeregOps > 0) { // Allocate structure only if numToBeDeregOps > 0
    up.reset(); // Reset PUP::fromMem to the original buffer
    up|numops;
    up|rootNode;
    p|numops;
    p|rootNode;

    // Allocate a structure to de-register after completion and invoke acks
    NcpyP2PAckInfo *p2pAckInfo = (NcpyP2PAckInfo *)CmiAlloc(sizeof(NcpyP2PAckInfo) + numops * sizeof(CkNcpyBuffer));
    p2pAckInfo->numOps  = numops;

    for(int i=0; i<numops; i++) {
      CkNcpyBuffer source;
      up|source;
      source.refAckInfo = p2pAckInfo; // Update refAckInfo with p2pAckInfo
      p2pAckInfo->src[i] = source;      // Store the source into the allocated structure
      p|source;
    }
  }
  CkPackMessage(&env);
}
#endif

// Method called on the bcast source to store some information for ack handling
void CkRdmaPrepareBcastMsg(envelope *env) {

  int numops, rootNode;
  CkUnpackMessage(&env);
  PUP::toMem p((void *)(((CkMarshallMsg *)EnvToUsr(env))->msgBuf));
  PUP::fromMem up((void *)((CkMarshallMsg *)EnvToUsr(env))->msgBuf);

  up|numops;
  up|rootNode;
  p|numops;
  p|rootNode;

  NcpyBcastRootAckInfo *bcastAckInfo = (NcpyBcastRootAckInfo *)CmiAlloc(sizeof(NcpyBcastRootAckInfo) + numops * sizeof(CkNcpyBuffer));

  CmiSpanningTreeInfo &t = *(getSpanningTreeInfo(CkMyNode()));
  bcastAckInfo->numChildren = t.child_count + 1;
  bcastAckInfo->setCounter(0);
  bcastAckInfo->isRoot  = true;
  bcastAckInfo->numops  = numops;
  bcastAckInfo->pe = CkMyPe();

  for(int i=0; i<numops; i++) {
    CkNcpyBuffer source;
    up|source;

    bcastAckInfo->src[i] = source;

    source.refAckInfo = bcastAckInfo;

    p|source;
  }
  CkPackMessage(&env);
}

// Method called to extract the parent bcastAckInfo from the received message for ack handling
// Requires message to be unpacked
const void *getParentBcastAckInfo(void *msg, int &srcPe) {
  int numops, rootNode;
  CkNcpyBuffer source;
  envelope *env = (envelope *)msg;
  PUP::toMem p((void *)(((CkMarshallMsg *)EnvToUsr(env))->msgBuf));
  PUP::fromMem up((void *)((CkMarshallMsg *)EnvToUsr(env))->msgBuf);

  up|numops;
  up|rootNode;
  p|numops;
  p|rootNode;

  CkAssert(numops >= 1);

  up|source;
  p|source;

  srcPe = source.pe;
  return source.refAckInfo;
}

// Called only on intermediate nodes
// Allocate a NcpyBcastInterimAckInfo and return it
NcpyBcastInterimAckInfo *allocateInterimNodeAckObj(envelope *myEnv, envelope *myChildEnv, int pe) {

  CkUnpackMessage(&myEnv);
  CmiSpanningTreeInfo &t = *(getSpanningTreeInfo(getRootNode(myEnv)));
  CkPackMessage(&myEnv);

  // Allocate a NcpyBcastInterimAckInfo object
  NcpyBcastInterimAckInfo *bcastAckInfo = (NcpyBcastInterimAckInfo *)CmiAlloc(sizeof(NcpyBcastInterimAckInfo));

  // Initialize fields of bcastAckInfo
  bcastAckInfo->numChildren = t.child_count;
  bcastAckInfo->counter = 0;
  bcastAckInfo->isRoot = false;
  bcastAckInfo->pe = pe;

  // Recv Bcast API uses myEnv as myChildEnv (and myChildEnv is NULL)
  bcastAckInfo->isRecv = (myChildEnv == NULL);
  bcastAckInfo->isArray = (myEnv->getMsgtype() == ArrayBcastFwdMsg);

  // initialize derived calss NcpyBcastInterimAckInfo fields
  bcastAckInfo->msg = myEnv; // this message will be enqueued after the completion of all operations

  bcastAckInfo->ncpyEmInfo = nullptr;

  return bcastAckInfo;
}

// Method called on the root node and other intermediate parent nodes on completion of RGET through ZC Bcast
void CkRdmaEMBcastAckHandler(void *ack) {
  NcpyBcastAckInfo *bcastAckInfo = (NcpyBcastAckInfo *)ack;

  // Increment counter to indicate that another child was completed
  // Since incCounter() is equivalent to counter++, it returns the value of 'counter' before incrementing it by 1.
  // For that reason, the comparison is performed with bcastAckInfo->incCounter() + 1 to compare with the updated value
  if(bcastAckInfo->incCounter() + 1 == bcastAckInfo->numChildren) {
    // All child nodes have completed RGETs

    // TODO: replace with a swtich with 3 cases
    if(bcastAckInfo->isRoot) {

      NcpyBcastRootAckInfo *bcastRootAckInfo = (NcpyBcastRootAckInfo *)(bcastAckInfo);
      // invoke the callback with the pointer
      for(int i=0; i<bcastRootAckInfo->numops; i++) {
#if CMK_REG_REQUIRED
        // Deregister source buffer respecting source buffer's dereg mode
        if(isDeregReady(bcastRootAckInfo->src[i]))
          deregisterBuffer(bcastRootAckInfo->src[i]);
#endif

        invokeCallback(&(bcastRootAckInfo->src[i].cb),
                       bcastRootAckInfo->pe,
                       bcastRootAckInfo->src[i]);
      }

      CmiFree(bcastRootAckInfo);

    } else {

      NcpyBcastInterimAckInfo *bcastInterimAckInfo = (NcpyBcastInterimAckInfo *)(bcastAckInfo);
      // This node should send a message to its parent
      envelope *myMsg = (envelope *)(bcastInterimAckInfo->msg);

      CkUnpackMessage(&myMsg);
      CkPackMessage(&myMsg);

      if(bcastInterimAckInfo->isRecv)  { // bcast post api
        // deregister using the message
#if CMK_REG_REQUIRED
        deregisterMemFromMsg(myMsg, true);
#endif
        CMI_ZC_MSGTYPE(myMsg) = CMK_ZC_BCAST_RECV_DONE_MSG;

        CkUnpackMessage(&myMsg); // DO NOT REMOVE THIS

        if(bcastInterimAckInfo->isArray) {
          handleArrayMsgOnChildPostCompletionForRecvBcast(myMsg, bcastInterimAckInfo->ncpyEmInfo);
        } else if(myMsg->getMsgtype() == ForBocMsg) {
          handleGroupMsgOnChildPostCompletionForRecvBcast(myMsg, bcastInterimAckInfo->ncpyEmInfo);
        } else if(myMsg->getMsgtype() == ForNodeBocMsg) {
          handleNGMsgOnChildPostCompletionForRecvBcast(myMsg, bcastInterimAckInfo->ncpyEmInfo);
        }

        CmiFree(bcastInterimAckInfo);

      } else { // bcast send api

        // deregister using the message
#if CMK_REG_REQUIRED
        deregisterMemFromMsg(myMsg, false);
#endif
        forwardMessageToPeerNodes(myMsg, myMsg->getMsgtype());

        // enquque message to execute EM on the intermediate node
        enqueueNcpyMessage(bcastAckInfo->pe, bcastInterimAckInfo->msg);

        CmiFree(bcastInterimAckInfo);
      }
    }
  }
}

// Called only on intermediate nodes
// Method forwards a message to all the children
void forwardMessageToChildNodes(envelope *myChildrenMsg, UChar msgType) {
#if CMK_SMP && CMK_NODE_QUEUE_AVAILABLE
  // Node level forwarding for nodegroup bcasts
  CmiForwardNodeBcastMsg(myChildrenMsg->getTotalsize(), (char *)myChildrenMsg);
#else
  // Proc level forwarding
  CmiForwardProcBcastMsg(myChildrenMsg->getTotalsize(), (char *)myChildrenMsg);
#endif
}

// Method forwards a message to all the peer nodes
void forwardMessageToPeerNodes(envelope *myMsg, UChar msgType) {
#if CMK_SMP
#if CMK_NODE_QUEUE_AVAILABLE
  if(msgType == ForBocMsg) {
#endif // CMK_NODE_QUEUE_AVAILABLE
    if(CMI_ZC_MSGTYPE(myMsg) == CMK_ZC_SEND_DONE_MSG)
      CkPackMessage(&myMsg);
    CmiForwardMsgToPeers(myMsg->getTotalsize(), (char *)myMsg);
#if CMK_NODE_QUEUE_AVAILABLE
  }
#endif // CMK_NODE_QUEUE_AVAILABLE
#endif
}

void handleBcastEntryMethodApiCompletion(NcpyOperationInfo *info){

// The following CMK_REG_REQUIRED is only used by the UCX layer. For CMK_REG_REQUIRED
// layers (GNI, OFI and Verbs), the code simply returns from LrtsInvokeRemoteDeregAckHandler.
#if CMK_REG_REQUIRED

  NcpyEmBufferInfo *emBuffInfo = (NcpyEmBufferInfo *)(info->refPtr);

  char *ref = (char *)(emBuffInfo);

  int layerInfoSize = CMK_COMMON_NOCOPY_DIRECT_BYTES + CMK_NOCOPY_DIRECT_BYTES;
  int ncpyObjSize = getNcpyOpInfoTotalSize(
                    layerInfoSize,
                    sizeof(CkCallback),
                    layerInfoSize,
                    0);


  NcpyEmInfo *ncpyEmInfo = (NcpyEmInfo *)(ref - (emBuffInfo->index) * (sizeof(NcpyEmBufferInfo) + ncpyObjSize - sizeof(NcpyOperationInfo)) - sizeof(NcpyEmInfo));

  int rootNode = getRootNode((envelope *)ncpyEmInfo->msg);
  CmiSpanningTreeInfo *t = getSpanningTreeInfo(rootNode);

  // De-register source for reverse operations when regMode == UNREG and deregMode == DEREG
  // on the first level of intermediate nodes i.e. t->parent == rootNode.
  // This is only required for the UCX layer since the UCX layer (unlike GNI, OFI and Verbs) doesn't
  // implement the GET for an UNREG source with a REG and PUT operation, since PUT doesn't guarantee
  // remote destination data transfer completion. In the UCX layer, since a GET for an UNREG source
  // is implemented with a remote REG and send-back mechanism, followed by a GET, it is important
  // to de-register the root's source buffer now.
  if(t->parent == rootNode &&
     info->isSrcRegistered == 1 &&
     info->srcRegMode == CK_BUFFER_UNREG &&
     info->srcDeregMode == CK_BUFFER_DEREG) {
    CmiInvokeRemoteDeregAckHandler(info->srcPe, info);
  }
#endif

  if(info->ackMode == CMK_SRC_DEST_ACK || info->ackMode == CMK_DEST_ACK) {
    // invoking the entry method
    // Invoke the ackhandler function to update the counter
    CkRdmaEMAckHandler(info->destPe, info->refPtr);
  }
}

void handleBcastReverseEntryMethodApiCompletion(NcpyOperationInfo *info) {
  if(info->ackMode == CMK_SRC_DEST_ACK || info->ackMode == CMK_DEST_ACK) {
    // Invoke the remote ackhandler function
    invokeRemoteNcpyAckHandler(info->destPe, info->refPtr, ncpyHandlerIdx::EM_ACK);
  }
#if CMK_REG_REQUIRED
  // De-register source for reverse operations when regMode == UNREG and deregMode == DEREG on the root node
  if(info->rootNode == CkMyNode())
    if(info->isSrcRegistered == 1 && info->srcRegMode == CK_BUFFER_UNREG && info->srcDeregMode == CK_BUFFER_DEREG)
      deregisterSrcBuffer(info);
#endif

  if(info->freeMe == CMK_FREE_NCPYOPINFO)
    CmiFree(info);
}

void deregisterMemFromMsg(envelope *env, bool isRecv) {
  CkUnpackMessage(&env);
  PUP::toMem p((void *)(((CkMarshallMsg *)EnvToUsr(env))->msgBuf));
  PUP::fromMem up((void *)((CkMarshallMsg *)EnvToUsr(env))->msgBuf);
  int numops, rootNode;
  up|numops;
  up|rootNode;
  p|numops;
  p|rootNode;

  CkNcpyBuffer dest;

  for(int i=0; i<numops; i++){
    up|dest;

    // De-register the destination buffer when isRecv is false (i.e. using ZC Bcast Send API) or
    // when isRecv is true, respect deregMode and de-register
    if( (!isRecv) || (isRecv && dest.deregMode == CMK_BUFFER_DEREG) )
      deregisterBuffer(dest);

    p|dest;
  }
  CkPackMessage(&env);
}

/****************************** Zerocopy BCAST EM SEND API Functions ***********************/

void handleMsgUsingCMAPostCompletionForSendBcast(envelope *copyenv, envelope *env, CkNcpyBuffer &source) {

  CkUnpackMessage(&env);
  CmiSpanningTreeInfo &t = *(getSpanningTreeInfo(getRootNode(env)));
  CkPackMessage(&env);

  // Send an ack message to the parent node to signal completion
  invokeRemoteNcpyAckHandler(source.pe, (void *)source.refAckInfo, ncpyHandlerIdx::BCAST_ACK);

  if(t.child_count == 0) { // child node

    // Only forwarding is to peer PEs
    forwardMessageToPeerNodes(copyenv, copyenv->getMsgtype());

    // enqueue message to execute EM on the child node
    enqueueNcpyMessage(CkMyPe(), copyenv);

  } else { // intermediate node

    // Allocate a structure NcpyBcastInterimAckInfo to maintain state for ack handling
    NcpyBcastInterimAckInfo *bcastAckInfo = allocateInterimNodeAckObj(copyenv, env, CkMyPe());

    //// Replace parent pointers with my pointers for my children
    CkReplaceSourcePtrsInBcastMsg(env, copyenv, bcastAckInfo, CkMyPe());

    // Send message to children for them to Rget from me
    forwardMessageToChildNodes(env, copyenv->getMsgtype());
  }
}

void processBcastSendEmApiCompletion(NcpyEmInfo *ncpyEmInfo, int destPe) {
  envelope *myEnv = (envelope *)(ncpyEmInfo->msg);
  CmiSpanningTreeInfo &t = *(getSpanningTreeInfo(getRootNode(myEnv)));

  CkUnpackMessage(&myEnv); // Unpack message before sending it to sendAckMsgToParent
  sendAckMsgToParent(myEnv); // Send a ack message to the parent node to signal completion
  CkPackMessage(&myEnv);

  if(t.child_count == 0) { // Child Node

    // Since I am a child node, no more forwarding to any more childing
    // Only forwarding is to peer PEs
    forwardMessageToPeerNodes(myEnv, myEnv->getMsgtype());

    // enquque message to execute EM on the child node
    enqueueNcpyMessage(destPe, myEnv);

  } else { // Intermediate Node

    envelope *myChildEnv = (envelope *)(ncpyEmInfo->forwardMsg);

    // Allocate a structure NcpyBcastInterimAckInfo to maintain state for ack handling
    NcpyBcastInterimAckInfo *bcastAckInfo = allocateInterimNodeAckObj(myEnv, myChildEnv, ncpyEmInfo->pe);

    // Replace parent pointers with my pointers for my children
    CkReplaceSourcePtrsInBcastMsg(myChildEnv, myEnv, bcastAckInfo, ncpyEmInfo->pe);

    // Send message to children for them to Rget from me
    forwardMessageToChildNodes(myChildEnv, myEnv->getMsgtype());
  }
}

// Method called on intermediate nodes after RGET to switch old source pointers with my pointers
void CkReplaceSourcePtrsInBcastMsg(envelope *prevEnv, envelope *env, void *bcastAckInfo, int origPe) {

  int numops, rootNode;

  CkUnpackMessage(&prevEnv);
  PUP::toMem p_prev((void *)(((CkMarshallMsg *)EnvToUsr(prevEnv))->msgBuf));
  PUP::fromMem up_prev((void *)((CkMarshallMsg *)EnvToUsr(prevEnv))->msgBuf);

  CkUnpackMessage(&env);
  PUP::toMem p((void *)(((CkMarshallMsg *)EnvToUsr(env))->msgBuf));
  PUP::fromMem up((void *)((CkMarshallMsg *)EnvToUsr(env))->msgBuf);

  up_prev|numops;
  up|numops;

  up_prev|rootNode;
  up|rootNode;

  p|numops;
  p_prev|numops;

  p|rootNode;
  p_prev|rootNode;

  for(int i=0; i<numops; i++){
    // source buffer
    CkNcpyBuffer prev_source, source;

    // unpack from previous message
    up_prev|prev_source;

    // unpack from current message
    up|source;

    const void *bcastAckInfoTemp = source.refAckInfo;
    int orig_source_pe = source.pe;

    source.refAckInfo = bcastAckInfo;
    source.pe = origPe;

    // pack updated CkNcpyBuffer into previous message
    p_prev|source;

    source.refAckInfo = bcastAckInfoTemp;
    source.pe = orig_source_pe;

    // pack back CkNcpyBuffer into current message
    p|source;
  }

  CkPackMessage(&prevEnv);

  CkPackMessage(&env);
}

/****************************** Zerocopy BCAST EM POST API Functions ***********************/

void processBcastRecvEmApiCompletion(NcpyEmInfo *ncpyEmInfo, int destPe) {
  // Send message to all peer elements on this PE
  // Send a message to the worker thread
#if CMK_SMP
  invokeRemoteNcpyAckHandler(destPe, ncpyEmInfo, ncpyHandlerIdx::BCAST_POST_ACK);
#else
  CkRdmaEMBcastPostAckHandler(ncpyEmInfo);
#endif
}

void CkRdmaEMBcastPostAckHandler(void *msg) {
  NcpyEmInfo *ncpyEmInfo = (NcpyEmInfo *)(msg);
  envelope *env = (envelope *)(ncpyEmInfo->msg);

  CmiSpanningTreeInfo &t = *(getSpanningTreeInfo(getRootNode(env)));

  // send an ack message to your parent node
  sendAckMsgToParent(env);

  if(t.child_count == 0) {

    // Send message to all peer elements on this PE
    handleMsgOnChildPostCompletionForRecvBcast(env, ncpyEmInfo);

  } else if(t.child_count !=0 && t.parent != -1) {

    // Allocate a structure NcpyBcastInterimAckInfo to maintain state for ack handling
    NcpyBcastInterimAckInfo *bcastAckInfo = allocateInterimNodeAckObj(env, NULL, ncpyEmInfo->pe);
    bcastAckInfo->ncpyEmInfo = ncpyEmInfo;
    handleMsgOnInterimPostCompletionForRecvBcast(env, bcastAckInfo, ncpyEmInfo->pe);

  } else {
    CmiAbort("parent node reaching CkRdmaEMBcastPostAckHandler\n");
  }

}

void CkReplaceSourcePtrsInBcastMsg(envelope *env, NcpyBcastInterimAckInfo *bcastAckInfo, int origPe) {

  int numops, rootNode;
  CkUnpackMessage(&env);
  //CkUnpackRdmaPtrs((((CkMarshallMsg *)EnvToUsr(env))->msgBuf));
  PUP::toMem p((void *)(((CkMarshallMsg *)EnvToUsr(env))->msgBuf));
  PUP::fromMem up((void *)((CkMarshallMsg *)EnvToUsr(env))->msgBuf);

  up|numops;
  up|rootNode;
  p|numops;
  p|rootNode;

  // source buffer
  CkNcpyBuffer source;

  for(int i=0; i<numops; i++){
    // unpack from current message
    up|source;

    source.refAckInfo = bcastAckInfo;
    source.pe = origPe;

    // pack back CkNcpyBuffer into current message
    p|source;
  }

  // CkPackRdmaPtrs((((CkMarshallMsg *)EnvToUsr(env))->msgBuf));
  CkPackMessage(&env);

#if !CMK_SMP || !CMK_NODE_QUEUE_AVAILABLE
  CMI_SET_BROADCAST_ROOT(env, rootNode+1);
#endif
}


#if CMK_SMP
void sendRecvDoneMsgToPeers(envelope *env, CkArray *mgr) {

  CmiForwardMsgToPeers(env->getTotalsize(), (char *)env);
}
#endif

// Send a message to the parent node to signal completion
void sendAckMsgToParent(envelope *env)  {
  int srcPe;

  // srcPe is passed by reference and written inside the method
  char *ref = (char *)getParentBcastAckInfo(env,srcPe);

  // Invoke BcastAckHandler on the parent node to notify completion
  invokeRemoteNcpyAckHandler(srcPe, ref, ncpyHandlerIdx::BCAST_ACK);
}

CkArray* getArrayMgrFromMsg(envelope *env) {

  CkArray *mgr = NULL;
  CkGroupID gId = env->getGroupNum();
  IrrGroup *obj = _getCkLocalBranchFromGroupID(gId);
  CkAssert(obj!=NULL);
  mgr = (CkArray *)obj;
  return mgr;
}

void freeNcpyEmInfo(NcpyEmInfo *ncpyEmInfo) {
  if(ncpyEmInfo) {
    if(ncpyEmInfo->peerAckInfo)
      delete ncpyEmInfo->peerAckInfo;

    if(ncpyEmInfo->tagArray)
      delete [] ncpyEmInfo->tagArray;

    CmiFree(ncpyEmInfo);
  }
}

void handleArrayMsgOnChildPostCompletionForRecvBcast(envelope *env, NcpyEmInfo *ncpyEmInfo) {

  CMI_ZC_MSGTYPE(env) = CMK_ZC_BCAST_RECV_DONE_MSG;
  CkArray *mgr = getArrayMgrFromMsg(env);
  mgr->forwardZCMsgToOtherElems(env);

#if CMK_SMP
  if(CmiMyNodeSize() > 1) {
    sendRecvDoneMsgToPeers(env, mgr);
  } else
#endif
  {
    if(mgr->getNumLocalElems() == 1) { // this is the only element
      CMI_ZC_MSGTYPE(env) = CMK_ZC_BCAST_RECV_ALL_DONE_MSG;
      QdCreate(1);
      CmiHandleMessage(env);
      freeNcpyEmInfo(ncpyEmInfo);
    }
  }
  //TODO: Equeue the basic message if there are no elements
}

void handleGroupMsgOnChildPostCompletionForRecvBcast(envelope *env, NcpyEmInfo *ncpyEmInfo) {
  CMI_ZC_MSGTYPE(env) = CMK_ZC_BCAST_RECV_DONE_MSG;
#if CMK_SMP
  if(CmiMyNodeSize() > 1) {
    sendRecvDoneMsgToPeers(env, NULL);
  } else
#endif
  {
    CMI_ZC_MSGTYPE(env) = CMK_ZC_BCAST_RECV_ALL_DONE_MSG;
    QdCreate(1);
    CmiHandleMessage(env);
    freeNcpyEmInfo(ncpyEmInfo);
  }
}

void handleNGMsgOnChildPostCompletionForRecvBcast(envelope *env, NcpyEmInfo *ncpyEmInfo) {
  CMI_ZC_MSGTYPE(env) = CMK_ZC_BCAST_RECV_ALL_DONE_MSG;
  QdCreate(1);
  CmiHandleMessage(env);
  freeNcpyEmInfo(ncpyEmInfo);
}

void handleMsgOnChildPostCompletionForRecvBcast(envelope *env, NcpyEmInfo *ncpyEmInfo) {
  switch(env->getMsgtype()) {

    case ArrayBcastFwdMsg : handleArrayMsgOnChildPostCompletionForRecvBcast(env, ncpyEmInfo);
                            break;
    case ForBocMsg        : handleGroupMsgOnChildPostCompletionForRecvBcast(env, ncpyEmInfo);
                            break;
    case ForNodeBocMsg    : handleNGMsgOnChildPostCompletionForRecvBcast(env, ncpyEmInfo);
                            break;
    default               : CmiAbort("Type of message currently not supported\n");
                            break;
  }
}

void handleMsgOnInterimPostCompletionForRecvBcast(envelope *env, NcpyBcastInterimAckInfo *bcastAckInfo, int pe) {
  // Replace parent pointers with my pointers for my children
  CkReplaceSourcePtrsInBcastMsg(env, bcastAckInfo, pe);

  CMI_ZC_MSGTYPE(env) = CMK_ZC_BCAST_RECV_MSG;


  // Send message to children for them to Rget from me
  forwardMessageToChildNodes(env, env->getMsgtype());
}


/***************************** Zerocopy Readonly Bcast Support ****************************/

extern int _roRdmaDoneHandlerIdx,_initHandlerIdx;
CksvExtern(int, _numPendingRORdmaTransfers);
#if CMK_SMP
extern std::atomic<UInt> numZerocopyROops;
#else
extern UInt  numZerocopyROops;
#endif
extern UInt curROIndex;
extern bool usedCMAForROBcastTransfer;
extern NcpyROBcastAckInfo *roBcastAckInfo;

void readonlyUpdateNumops() {
  //update numZerocopyROops
  numZerocopyROops++;
}

// Method to allocate an object on the source for de-registration after bcast completes
void readonlyAllocateOnSource() {

  if(_topoTree == NULL) CkAbort("CkRdmaIssueRgets:: topo tree has not been calculated \n");
  CmiSpanningTreeInfo &t = *_topoTree;

  // allocate the buffer to keep track of the completed operations
  roBcastAckInfo = (NcpyROBcastAckInfo *)CmiAlloc(sizeof(NcpyROBcastAckInfo) + numZerocopyROops * sizeof(NcpyROBcastBuffAckInfo));

  roBcastAckInfo->counter = 0;
  roBcastAckInfo->isRoot = (t.parent == -1);
  roBcastAckInfo->numChildren = t.child_count;
  roBcastAckInfo->numops = numZerocopyROops;
}

// Method to initialize the allocated object with each source buffer's information
void readonlyCreateOnSource(CkNcpyBuffer &src) {
  src.refAckInfo = roBcastAckInfo;

  NcpyROBcastBuffAckInfo *buffAckInfo = &(roBcastAckInfo->buffAckInfo[curROIndex]);

  buffAckInfo->ptr = src.ptr;
  buffAckInfo->regMode = src.regMode;
  buffAckInfo->pe = src.pe;

  // store the source layer information for de-registration
  memcpy(buffAckInfo->layerInfo, src.layerInfo, CMK_COMMON_NOCOPY_DIRECT_BYTES + CMK_NOCOPY_DIRECT_BYTES);

  curROIndex++;
}

// Method to perform an get for Readonly transfer
// In the case of SMP mode, readonlyGet is invoked on the worker thread (CmiMyRank == 0)
// In the case of Non-SMP mode, readonlyGet is invoked on the same process which receives RODataMsg
void readonlyGet(CkNcpyBuffer &src, CkNcpyBuffer &dest, void *refPtr) {

  CkAssert(CkMyRank() == 0);

  CmiSpanningTreeInfo &t = *_topoTree;

  CkNcpyMode transferMode = findTransferMode(src.pe, dest.pe);
  if(transferMode == CkNcpyMode::MEMCPY) {
    CmiAbort("memcpy: should not happen\n");
  }
#if CMK_USE_CMA
  else if(transferMode == CkNcpyMode::CMA) {
    dest.cmaGet(src);

    // Decrement _numPendingRORdmaTransfers after completion of an Get operation
    CksvAccess(_numPendingRORdmaTransfers)--;

    // Initialize previously allocated structure for ack tracking on intermediate nodes
    if(t.child_count != 0)  // Intermediate Node
      readonlyCreateOnSource(dest);
    else // Child Node - deregister dest buffer
      deregisterBuffer(dest);

    // When all pending RO Rdma transfers are complete
    if(CksvAccess(_numPendingRORdmaTransfers) == 0) {

      if(t.child_count != 0) {  // Intermediate Node

        // Send a message to my child nodes
        envelope *env = (envelope *)(refPtr);
        CmiForwardProcBcastMsg(env->getTotalsize(), (char *)env);

      } else { // Child Node

        // Send a message to the parent to signal completion in order to deregister
        QdCreate(1);
        envelope *compEnv = _allocEnv(ROChildCompletionMsg);
        compEnv->setSrcPe(CkMyPe());
        CmiSetHandler(compEnv, _roRdmaDoneHandlerIdx);
        CmiSyncSendAndFree(src.pe, compEnv->getTotalsize(), (char *)compEnv);
      }

      // mark this variable as true in order to invoke checkForInitDone later
      usedCMAForROBcastTransfer = true;
    }
  }
#endif
  else {
    // Initialize previously allocated structure for ack tracking on intermediate nodes
    if(t.child_count != 0)
      readonlyCreateOnSource(dest);

    int ackSize = 0;
    int rootNode = 0;// Root Node is always 0 for Readonly bcast (the spanning tree is rooted at Node 0)

    NcpyOperationInfo *ncpyOpInfo = dest.createNcpyOpInfo(src, dest, ackSize, NULL, NULL, rootNode, CMK_READONLY_BCAST, refPtr);

    zcQdIncrement();

    CmiIssueRget(ncpyOpInfo);
  }
}

// Method invoked when an RO RDMA operation is complete
// In the case of SMP mode, readonlyGetCompleted is invoked on the comm. thread
// In the case of Non-SMP mode, readonlyGet is invoked on the same process which receives the RODataMsg
void readonlyGetCompleted(NcpyOperationInfo *ncpyOpInfo) {

  if(_topoTree == NULL) CkAbort("CkRdmaIssueRgets:: topo tree has not been calculated \n");
  CmiSpanningTreeInfo &t = *_topoTree;

  // Lock not needed for SMP mode as no other thread decrements _numPendingRORdmaTransfers
  CksvAccess(_numPendingRORdmaTransfers)--;

  if(t.child_count == 0) // deregister dest buffer on the child node
    deregisterDestBuffer(ncpyOpInfo);

  // When all pending RO Rdma transfers are complete
  if(CksvAccess(_numPendingRORdmaTransfers) == 0) {

    if(t.child_count != 0) {  // Intermediate Node

      envelope *env = (envelope *)(ncpyOpInfo->refPtr);

      // Send a message to my child nodes
      CmiForwardProcBcastMsg(env->getTotalsize(), (char *)env);

    } else {

      // Send a message to the parent to signal completion in order to deregister
      QdCreate(1);
      envelope *compEnv = _allocEnv(ROChildCompletionMsg);
      compEnv->setSrcPe(CkMyPe());
      CmiSetHandler(compEnv, _roRdmaDoneHandlerIdx);
      CmiSyncSendAndFree(ncpyOpInfo->srcPe, compEnv->getTotalsize(), (char *)compEnv);
    }

#if CMK_SMP
    // Send a message to my first node to signal completion
    QdCreate(1);
    envelope *sigEnv = _allocEnv(ROPeerCompletionMsg);
    sigEnv->setSrcPe(CkMyPe());
    CmiSetHandler(sigEnv, _roRdmaDoneHandlerIdx);
    CmiSyncSendAndFree(CmiNodeFirst(CmiMyNode()), sigEnv->getTotalsize(), (char *)sigEnv);
#else
    // Directly call checkInitDone to notify RO Rdma completion
    checkForInitDone(true);
#endif

  }

  // Free ncpyOpInfo allocated inside readonlyGet
  if(ncpyOpInfo->freeMe == CMK_FREE_NCPYOPINFO)
    CmiFree(ncpyOpInfo);
}
/***************************** End of Zerocopy Readonly Bcast Support ****************************/

inline void invokeCmaDirectRemoteDeregAckHandler(CkNcpyBuffer &buffInfo, ncpyHandlerIdx opMode) {
  PUP::sizer implSizer;
  implSizer|buffInfo;

  ncpyHandlerMsg *msg = (ncpyHandlerMsg *)CmiAlloc(sizeof(ncpyHandlerMsg) + implSizer.size());

  PUP::toMem implP((void *)((char *)msg + sizeof(ncpyHandlerMsg)));
  implP|buffInfo;

  msg->opMode = opMode;
  CmiSetHandler(msg, ncpy_handler_idx);
  QdCreate(1); // Matching QdProcess in _ncpyAckHandler
  CmiSyncSendAndFree(buffInfo.pe, sizeof(ncpyHandlerMsg) + implSizer.size(), (char *)msg);
}


// Register converse handler for invoking ncpy ack
void initEMNcpyAckHandler(void) {
  ncpy_handler_idx = CmiRegisterHandler((CmiHandler)_ncpyAckHandler);
#if CMK_SMP
  zcpy_pup_complete_handler_idx = CmiRegisterHandler((CmiHandler)_zcpyPupCompleteHandler);
#endif
}

inline void invokeRemoteNcpyAckHandler(int pe, void *ref, ncpyHandlerIdx opMode) {
  ncpyHandlerMsg *msg = (ncpyHandlerMsg *)CmiAlloc(sizeof(ncpyHandlerMsg));
  msg->ref = ref;
  msg->opMode = opMode;

  CmiSetHandler(msg, ncpy_handler_idx);
  QdCreate(1); // Matching QdProcess in _ncpyAckHandler
  CmiSyncSendAndFree(pe, sizeof(ncpyHandlerMsg), (char *)msg);
}

inline void _ncpyAckHandler(ncpyHandlerMsg *msg) {
  QdProcess(1);

  switch(msg->opMode) {
    case ncpyHandlerIdx::EM_ACK                : CkRdmaEMAckHandler(CmiMyPe(), msg->ref);
                                                 break;
    case ncpyHandlerIdx::BCAST_ACK             : CkRdmaEMBcastAckHandler(msg->ref);
                                                 break;
    case ncpyHandlerIdx::BCAST_POST_ACK        : CkRdmaEMBcastPostAckHandler(msg->ref);
                                                 break;
#if CMK_USE_CMA && CMK_REG_REQUIRED
    case ncpyHandlerIdx::CMA_DEREG_ACK         : CkRdmaEMDeregAndAckHandler(msg->ref);
                                                 break;
    case ncpyHandlerIdx::CMA_DEREG_ACK_DIRECT  : CkRdmaEMDeregAndAckDirectHandler((char *)msg + sizeof(ncpyHandlerMsg));
                                                 break;
#endif
    default                                    : CmiAbort("_ncpyAckHandler: Invalid OpMode\n");
                                                 break;
  }

  CmiFree(msg); // Allocated in invokeRemoteNcpyAckHandler
}


/***************************** Zerocopy PUP Support ****************************/

#if CMK_SMP
// Executed on the worker thread to enqueue all buffered messages after Rgets complete
void _zcpyPupCompleteHandler(zcPupPendingRgetsMsg *msg) {
  CProxy_CkLocMgr(msg->locMgrId).ckLocalBranch()->processAfterActiveRgetsCompleted(msg->id);
  CmiFree(msg);
}
#endif

// Executed on the comm. thread in SMP mode
void zcPupGetCompleted(NcpyOperationInfo *info) {
  if(info->ackMode == CMK_SRC_DEST_ACK || info->ackMode == CMK_DEST_ACK) {
    zcPupPendingRgetsMsg *ref = (zcPupPendingRgetsMsg *)(info->destRef);

    auto iter = CksvAccess(pendingZCOps).find(ref->id);
    if(iter != CksvAccess(pendingZCOps).end()) { // Entry found in pendingZCOps

      CmiLock(CksvAccess(_nodeZCPendingLock));
      int counter = --iter->second;
      CmiUnlock(CksvAccess(_nodeZCPendingLock));

      if(counter == 0) { // All Rgets have completed

        CmiLock(CksvAccess(_nodeZCPendingLock));
        // remove this entry from the map
        CksvAccess(pendingZCOps).erase(iter);
        CmiUnlock(CksvAccess(_nodeZCPendingLock));

#if CMK_SMP
        // Send a message to all PEs on this node to handle all buffered messages
        CmiSetHandler(ref, zcpy_pup_complete_handler_idx);
        CmiSyncSend(ref->pe,
                    sizeof(zcPupPendingRgetsMsg),
                    (char *)ref);
#else
        // On worker thread, handle all buffered messages
        CProxy_CkLocMgr(ref->locMgrId).ckLocalBranch()->processAfterActiveRgetsCompleted(ref->id);
#endif
        CmiFree(ref);
      }
    } else {
      CmiAbort("zcPupGetCompleted: object not found\n");
    }
    if(info->ackMode == CMK_SRC_DEST_ACK) {
      invokeZCPupHandler((void *)info->srcRef, info->srcPe);

#if CMK_REG_REQUIRED
      // De-register destination buffer
      deregisterDestBuffer(info);
#endif
    }
  } else {
    zcPupDone((void *)info->srcRef);
  }
  if(info->freeMe == CMK_FREE_NCPYOPINFO)
    CmiFree(info);
}

// Issue Rgets for ZC Pup using NcpyOperationInfo stored in newZCPupGets
void zcPupIssueRgets(CmiUInt8 id, CkLocMgr *locMgr) {

  // Allocate a zcPupPendingRgetsMsg that is used for ack handling
  zcPupPendingRgetsMsg *ref = (zcPupPendingRgetsMsg *)CmiAlloc(sizeof(zcPupPendingRgetsMsg));
  ref->id = id;
  ref->numops = CpvAccess(newZCPupGets).size();
  ref->locMgrId = locMgr->getGroupID();
#if CMK_SMP
  ref->pe = CmiMyPe();
#endif

  for(std::vector<NcpyOperationInfo *>::iterator it = CpvAccess(newZCPupGets).begin();
        it != CpvAccess(newZCPupGets).end(); ++it) {
    (*it)->destRef = (char *)ref;
    zcQdIncrement();
    CmiIssueRget(*it); // Issue the Rget
  }

  // Create an entry for the unordered map with idx as the index and the vector size as the value
  CmiLock(CksvAccess(_nodeZCPendingLock));
  CksvAccess(pendingZCOps).emplace(id, (CmiUInt1)CpvAccess(newZCPupGets).size());
  CmiUnlock(CksvAccess(_nodeZCPendingLock));

  // Create an entry for the unordered map with idx as the index and vector of messages as the value
  locMgr->bufferedActiveRgetMsgs.emplace(id, std::vector<CkArrayMessage *>()); // does not require locking as it is owned by locMgr
}
/***************************** End of Zerocopy PUP Support ****************************/


/****************************** Channel API ******************************/

#if CMK_GPU_COMM
CkChannel::CkChannel(int id_, const CProxyElement_ArrayBase &proxy) : CkChannel(id_) {
  peer_pe = proxy.ckLocalBranch()->lastKnown(proxy.ckGetIndex());
}

CkChannel::CkChannel(int id_, const CProxyElement_Group &proxy) : CkChannel(id_) {
  peer_pe = proxy.ckGetGroupPe();
}

CkChannel::CkChannel(int id_, const CProxyElement_NodeGroup &proxy) : CkChannel(id_) {
  peer_pe = proxy.ckGetGroupPe();
}

void CkChannel::send(const void* ptr, size_t size, CkFuture* fut) {
  CkChannelMetadata* metadata = new CkChannelMetadata();
  metadata->fut = fut;
  CmiChannelSend(peer_pe, id, ptr, size, metadata, send_counter++);
}

void CkChannel::send(const void* ptr, size_t size, bool cb_self, const CkCallback& cb, void* msg) {
  CkChannelMetadata* metadata = new CkChannelMetadata();
  metadata->use_cb = true;
  metadata->cb = cb;
  metadata->cb_pe = cb_self ? CkMyPe() : peer_pe;
  metadata->msg = msg;
  CmiChannelSend(peer_pe, id, ptr, size, metadata, send_counter++);
}

void CkChannel::recv(const void* ptr, size_t size, CkFuture* fut) {
  CkChannelMetadata* metadata = new CkChannelMetadata();
  metadata->fut = fut;
  CmiChannelRecv(id, ptr, size, metadata, recv_counter++);
}

void CkChannel::recv(const void* ptr, size_t size, bool cb_self, const CkCallback& cb, void* msg) {
  CkChannelMetadata* metadata = new CkChannelMetadata();
  metadata->use_cb = true;
  metadata->cb = cb;
  metadata->cb_pe = cb_self ? CkMyPe() : peer_pe;
  metadata->msg = msg;
  CmiChannelRecv(id, ptr, size, metadata, recv_counter++);
}

void CkChannelHandler(void* data)
{
  CkChannelMetadata* metadata = static_cast<CkChannelMetadata*>(data);

  // Invoke Charm++ callback
  if (metadata->use_cb) {
#if CMK_SMP
    if (metadata->cb.type == CkCallback::resumeThread) {
      metadata->cb.send(metadata->msg);
    } else {
      _ckcallbackgroup[metadata->cb_pe].call(metadata->cb, (CkMessage*)metadata->msg);
    }
#else
    metadata->cb.send(metadata->msg);
#endif
  }

  // Flag request as complete
  if (metadata->fut) {
    CkMarshallMsg* msg = CkAllocateMarshallMsg(sizeof(bool), NULL);
    PUP::toMem p((void*)msg->msgBuf);
    int value = true;
    p | value;
    CkSendToFuture(*metadata->fut, msg);
  }

  // Delete metadata object
  delete metadata;
}
#endif // CMK_GPU_COMM

/****************************** End of Channel API ******************************/

/**************************** Zerocopy Post API Async Support **************************/

// PE-level Post method
void CkPostBufferInternal(void *destBuffer, size_t destSize, int tag) {

  // check if tag exists in posted req table
  auto iter = CkpvAccess(ncpyPostedReqMap).find(tag);

  if(iter == CkpvAccess(ncpyPostedReqMap).end()) {

    auto iter2 = CkpvAccess(ncpyPostedBufferMap).find(tag);

    if(iter2 == CkpvAccess(ncpyPostedBufferMap).end()) { // not found, insert into ncpyPostedBufferMap
      CkPostedBuffer postedBuff;
      postedBuff.buffer = destBuffer;
      postedBuff.bufferSize = destSize;
      CkpvAccess(ncpyPostedBufferMap).emplace(tag, postedBuff);
    } else {
      CkAbort("CkPostBufferInternal: tag %d already exists, use another tag!\n", tag);
    }

  } else { // found, perform rget

    CkNcpyBufferPost post = iter->second;
    if(CkPerformRget(post, destBuffer, destSize))  {
      CkpvAccess(ncpyPostedReqMap).erase(iter);
    }

  }
}

// Node-level Post method
void CkPostNodeBufferInternal(void *destBuffer, size_t destSize, int tag) {

  // check if tag exists in posted req node table
  CmiLock(CksvAccess(_nodeZCPostReqLock));
  auto iter = CksvAccess(ncpyPostedReqNodeMap).find(tag);
  CmiUnlock(CksvAccess(_nodeZCPostReqLock));

  if(iter == CksvAccess(ncpyPostedReqNodeMap).end()) {

    CmiLock(CksvAccess(_nodeZCBufferReqLock));
    auto iter2 = CksvAccess(ncpyPostedBufferNodeMap).find(tag);
    CmiUnlock(CksvAccess(_nodeZCBufferReqLock));

    if(iter2 == CksvAccess(ncpyPostedBufferNodeMap).end()) { // not found, insert into ncpyPostedBufferNodeMap
      CkPostedBuffer postedBuff;
      postedBuff.buffer = destBuffer;
      postedBuff.bufferSize = destSize;

      CmiLock(CksvAccess(_nodeZCBufferReqLock));
      CksvAccess(ncpyPostedBufferNodeMap).emplace(tag, postedBuff);
      CmiUnlock(CksvAccess(_nodeZCBufferReqLock));
    } else {
      CkAbort("CkPostNodeBufferInternal: tag %d already exists, use another tag!\n", tag);
    }

  } else { // found, perform rget

    CkNcpyBufferPost post = iter->second;

    if(CkPerformRget(post, destBuffer, destSize)) {
      CmiLock(CksvAccess(_nodeZCPostReqLock));
      CksvAccess(ncpyPostedReqNodeMap).erase(iter);
      CmiUnlock(CksvAccess(_nodeZCPostReqLock));
    }
  }
}

// PE-level Match method
void CkMatchBuffer(CkNcpyBufferPost *post, int index, int tag) {

  post[index].postAsync = true;

  // check if tag exists in posted buffer table
  auto iter = CkpvAccess(ncpyPostedBufferMap).find(tag);

  if(iter == CkpvAccess(ncpyPostedBufferMap).end()) {

    auto iter2 = CkpvAccess(ncpyPostedReqMap).find(tag);

    if(iter2 == CkpvAccess(ncpyPostedReqMap).end()) { // not found, insert into ncpyPostedReqMap
      post[index].tag = tag;
      CkpvAccess(ncpyPostedReqMap).emplace(post[index].tag, post[index]);
    } else {
      CkAbort("CkMatchBuffer: tag %d already exists, use another tag!\n", tag);
    }

  } else { // found, perform rget

    CkPostedBuffer *buff = &(iter->second);
    post[index].tag = tag;

    if(CkPerformRget((post[index]), buff->buffer, buff->bufferSize)) {
      CkpvAccess(ncpyPostedBufferMap).erase(iter);
    }
  }
}

// Node-level Match method
void CkMatchNodeBuffer(CkNcpyBufferPost *post, int index, int tag) {

  post[index].postAsync = true;

  // check if tag exists posted buffer node table
  auto iter = CksvAccess(ncpyPostedBufferNodeMap).find(tag);

  if(iter == CksvAccess(ncpyPostedBufferNodeMap).end()) {

    auto iter2 = CksvAccess(ncpyPostedReqNodeMap).find(tag);

    if(iter2 == CksvAccess(ncpyPostedReqNodeMap).end()) { // not found, insert into ncpyPostedReqNodeMap
      post[index].tag = tag;
      CmiLock(CksvAccess(_nodeZCPostReqLock));
      CksvAccess(ncpyPostedReqNodeMap).emplace(post[index].tag, post[index]);
      CmiUnlock(CksvAccess(_nodeZCPostReqLock));
    } else {
      CkAbort("CkMatchNodeBuffer: tag %d already exists, use another tag!\n", tag);
    }

  } else { // found, perform rget

    CkPostedBuffer *buff = &(iter->second);
    post[index].tag = tag;

    if(CkPerformRget((post[index]), buff->buffer, buff->bufferSize)) {
      CmiLock(CksvAccess(_nodeZCBufferReqLock));
      CksvAccess(ncpyPostedBufferNodeMap).erase(iter);
      CmiUnlock(CksvAccess(_nodeZCBufferReqLock));
    }
  }
}

void initPostStruct(CkNcpyBufferPost *ncpyPost, int index) {
  CkNcpyBufferPost &ncpyPostElem = ncpyPost[index];
  ncpyPostElem.regMode = CK_BUFFER_REG;
  ncpyPostElem.deregMode = CK_BUFFER_DEREG;
  ncpyPostElem.index = index;
  ncpyPostElem.postAsync = false;
}

void setPostStruct(CkNcpyBufferPost *ncpyPost, int index, CkNcpyBuffer &buffObj, CmiUInt8 elemIndex) {
  CkNcpyBufferPost &ncpyPostElem = ncpyPost[index];
  ncpyPostElem.srcBuffer = (void *)buffObj.ptr;
  ncpyPostElem.srcSize = buffObj.cnt;
  ncpyPostElem.ncpyEmInfo->tagArray = buffObj.ncpyEmInfo->tagArray;
  ncpyPostElem.opIndex = index;
  ncpyPostElem.arrayIndex = elemIndex;
}

void updateTagArray(envelope *env, int localElems) {
  int numops = 0;
  int rootNode;
  PUP::fromMem up((void *)((CkMarshallMsg *)EnvToUsr(env))->msgBuf);
  up|numops;
  up|rootNode;
  for(int i=0; i<numops; i++){
    CkNcpyBuffer w;
    up|w;
    w.ncpyEmInfo->tagArray[CmiMyRank()].resize(static_cast<size_t>(localElems) * numops);
    std::fill(w.ncpyEmInfo->tagArray[CmiMyRank()].begin(), w.ncpyEmInfo->tagArray[CmiMyRank()].end(), -1);
    w.ncpyEmInfo->peerAckInfo->incNumElems(localElems);
    w.ncpyEmInfo->peerAckInfo->decNumPeers();
    break;
  }
}

void updatePeerCounter(NcpyEmInfo *ncpyEmInfo) {
  NcpyBcastRecvPeerAckInfo *peerAckInfo = ncpyEmInfo->peerAckInfo;
  if(peerAckInfo->decNumElems() - 1 == 0 && peerAckInfo->getNumPeers() == 0) {
    envelope *env = (envelope *)peerAckInfo->msg;
    CMI_ZC_MSGTYPE(env) = CMK_ZC_BCAST_RECV_ALL_DONE_MSG;
    if(env->getMsgtype() == ArrayBcastFwdMsg) {
      QdCreate(1);
      enqueueNcpyMessage(peerAckInfo->peerParentPe, env);
    } else if(env->getMsgtype() == ForBocMsg) {
      CmiPushPE(CmiRankOf(peerAckInfo->peerParentPe), env);
    }
    freeNcpyEmInfo(ncpyEmInfo);
  }
}

void setPosted(std::vector<int> *tagArray, envelope *env, CmiUInt8 elemIndex, int numops, int opIndex) {
  int localIndex = -1;
  if(env->getMsgtype() == ArrayBcastFwdMsg) {
    CkArray *mgr = getArrayMgrFromMsg(env);
    localIndex = mgr->getEltLocalIndex(elemIndex);
    tagArray[CmiMyRank()][localIndex * numops + opIndex] = 0;
  } else {
    localIndex = CmiMyRank();
    tagArray[CmiMyRank()][opIndex] = 0;
  }
}

bool isUnposted(std::vector<int> *tagArray, envelope *env, CmiUInt8 elemIndex, int numops) {
  int opIndex = 0;
  int localIndex = -1;
  if(env->getMsgtype() == ArrayBcastFwdMsg) {
    CkArray *mgr = getArrayMgrFromMsg(env);
    localIndex = mgr->getEltLocalIndex(elemIndex);
    return (tagArray[CmiMyRank()][localIndex * numops + opIndex] == -1);
  } else {
    localIndex = CmiMyRank();
    return (tagArray[CmiMyRank()][opIndex] == -1);
  }
}

void *extractStoredBuffer(std::vector<int> *tagArray, envelope *env, CmiUInt8 elemIndex, int numops, int opIndex) {
  int tag = 0;
  int localIndex = -1;
  void *ptr = nullptr;

  // Retrieve tag
  if(env->getMsgtype() == ArrayBcastFwdMsg) {
    CkArray *mgr = getArrayMgrFromMsg(env);
    localIndex = mgr->getEltLocalIndex(elemIndex);
    tag = tagArray[CmiMyRank()][localIndex * numops + opIndex];

  } else if(env->getMsgtype() == ForBocMsg) {

    localIndex = CmiMyRank();
    tag = tagArray[CmiMyRank()][opIndex];
  }

  // Find buffer pointer using tag
  auto iter = CkpvAccess(ncpyPostedReqMap).find(tag);

  if(iter == CkpvAccess(ncpyPostedReqMap).end()) { // Entry not found in ncpyPostedReqMap
    auto iter2 = CkpvAccess(ncpyPostedBufferMap).find(tag);

    if(iter2 == CkpvAccess(ncpyPostedBufferMap).end()) {
      CkAbort("extractStoredBuffer: Tag:%d not found on Pe:%d\n", tag, CmiMyPe());
    } else {
      CkPostedBuffer buff = (iter2->second);
      ptr = buff.buffer; // set ptr
      CkpvAccess(ncpyPostedBufferMap).erase(iter2);
    }
  } else {
    CkNcpyBufferPost *post = &(iter->second);
    ptr = post->srcBuffer; // set ptr
    CkpvAccess(ncpyPostedReqMap).erase(iter);
  }
  return ptr;
}

// Preprocess method executed for primary element (that performs the zcpy operation)
void CkRdmaAsyncPostPreprocess(envelope *env, int numops, CkNcpyBufferPost *post) {

  int refSize = 0;
  NcpyEmInfo *ncpyEmInfo = nullptr;
  int layerInfoSize, ncpyObjSize, extraSize;

  ncpyEmApiMode emMode = (CMI_ZC_MSGTYPE(env) == CMK_ZC_BCAST_RECV_MSG) ? ncpyEmApiMode::BCAST_RECV : ncpyEmApiMode::P2P_RECV;

  CkNcpyMode ncpyMode = findTransferMode(getSrcPe(env), CkMyPe());

  layerInfoSize = CMK_COMMON_NOCOPY_DIRECT_BYTES + CMK_NOCOPY_DIRECT_BYTES;

  if(ncpyMode == CkNcpyMode::RDMA) {
    preprocessRdmaCaseForRgets(layerInfoSize, ncpyObjSize, extraSize, refSize, numops);
    ncpyEmInfo = (NcpyEmInfo *)CmiAlloc(refSize);
    setNcpyEmInfo(ncpyEmInfo, env, numops, NULL, emMode);

  } else {
    ncpyEmInfo = (NcpyEmInfo *)CmiAlloc(sizeof(NcpyEmInfo));
    refSize = sizeof(NcpyEmInfo);
    setNcpyEmInfo(ncpyEmInfo, env, numops, NULL, emMode);
  }

  std::vector<int> *tagArray = NULL;
  NcpyBcastRecvPeerAckInfo *peerAckInfo = NULL;

  if(emMode == ncpyEmApiMode::BCAST_RECV) {
    if(env->getMsgtype() == ArrayBcastFwdMsg) {
      CkArray *mgr = getArrayMgrFromMsg(env);
      int numElems = mgr->getNumLocalElems();

      tagArray = new std::vector<int>[CmiMyNodeSize()];
      tagArray[CmiMyRank()].resize(static_cast<size_t>(numElems) * numops);
      std::fill(tagArray[CmiMyRank()].begin(), tagArray[CmiMyRank()].end(), -1);

      peerAckInfo = new NcpyBcastRecvPeerAckInfo();
      peerAckInfo->init(numElems - 1, CmiMyNodeSize() - 1, env, CmiMyPe());
    }
#if CMK_SMP
    else if(env->getMsgtype() == ForBocMsg) {

      int numElems = CmiMyNodeSize();

      tagArray = new std::vector<int>[CmiMyNodeSize()];
      for(int i=0; i < CmiMyNodeSize(); i++) {
        tagArray[i].resize(numops);
        std::fill(tagArray[i].begin(), tagArray[i].end(), -1);
      }

      peerAckInfo = new NcpyBcastRecvPeerAckInfo();
      peerAckInfo->init(numElems - 1, 0, env, CmiMyPe());
    }
#endif
  }

  ncpyEmInfo->tagArray = tagArray;
  ncpyEmInfo->peerAckInfo = peerAckInfo;

  for(int i=0; i<numops; i++) {
    post[i].ncpyEmInfo = ncpyEmInfo;
  }
}

// Preprocess method executed for the secondary element
void CkRdmaAsyncPostPreprocess(envelope *env, int numops, CkNcpyBufferPost *post, CmiUInt8 arrayIndex, void *peerAckInfo) {

  ncpyEmApiMode emMode = (CMI_ZC_MSGTYPE(env) == CMK_ZC_BCAST_RECV_MSG) ? ncpyEmApiMode::BCAST_RECV : ncpyEmApiMode::P2P_RECV;

  NcpyEmInfo *ncpyEmInfo = (NcpyEmInfo *)CmiAlloc(sizeof(NcpyEmInfo));
  setNcpyEmInfo(ncpyEmInfo, env, numops, NULL, emMode);

  for(int i=0; i<numops; i++) {
    post[i].ncpyEmInfo = ncpyEmInfo;
  }
}

int CkPerformRget(CkNcpyBufferPost &post, void *destBuffer, int destSize) {

  envelope *env = (envelope *)post.ncpyEmInfo->msg;
  int numops = post.ncpyEmInfo->numOps;
  ncpyEmApiMode emMode = post.ncpyEmInfo->mode;

  if(CMI_IS_ZC_RECV(env)) {
    int destIndex = post.index;
    int refSize = 0;
    NcpyEmInfo *ncpyEmInfo;
    int layerInfoSize, ncpyObjSize, extraSize;
    int rootNode;

    layerInfoSize = CMK_COMMON_NOCOPY_DIRECT_BYTES + CMK_NOCOPY_DIRECT_BYTES;

    CkNcpyMode ncpyMode = findTransferMode(getSrcPe(env), CkMyPe());

    CmiSpanningTreeInfo *t = NULL;
    ncpyEmInfo = post.ncpyEmInfo;

    PUP::toMem p((void *)(((CkMarshallMsg *)EnvToUsr(env))->msgBuf));
    PUP::fromMem up((void *)((CkMarshallMsg *)EnvToUsr(env))->msgBuf);
    up|numops;
    up|rootNode;
    p|numops;
    p|rootNode;

    if(emMode == ncpyEmApiMode::BCAST_RECV) {
      if(_topoTree == NULL) CkAbort("CkPostBufferInternal: topo tree has not been calculated \n");
      t = getSpanningTreeInfo(rootNode);
    }

    if(ncpyMode == CkNcpyMode::RDMA) {
      preprocessRdmaCaseForRgets(layerInfoSize, ncpyObjSize, extraSize, refSize, numops);
    }

    // source buffer
    CkNcpyBuffer source;

    bool sendBackToSourceForDereg = false;
    for(int i=0; i<numops; i++){
      up|source;

      if(i == destIndex) {

        if(source.cnt < destSize)
          CkAbort("CkRdmaIssueRgets: Size of the posted buffer > Size of the source buffer\n");

#if CMK_USE_CMA && CMK_REG_REQUIRED
        if(!sendBackToSourceForDereg && ncpyMode == CkNcpyMode::CMA && source.refAckInfo != NULL)
          sendBackToSourceForDereg = true;
#endif

        // destination buffer
        CkNcpyBuffer dest((const void *)destBuffer, destSize, post.regMode, post.deregMode);

        performEmApiNcpyTransfer(source, dest, i, t, ncpyEmInfo, extraSize, ncpyMode, rootNode, emMode);

        //Update the CkRdmaWrapper pointer of the new message
        source.ptr = destBuffer;

        source.isRegistered = dest.isRegistered;

        source.regMode = dest.regMode;

        source.deregMode = dest.deregMode;

        source.ncpyEmInfo = post.ncpyEmInfo;

        memcpy(source.layerInfo, dest.layerInfo, layerInfoSize);
      }
      p|source;
    }

    bool allOpsComplete = false;

    // check for completion
    if(ncpyMode != CkNcpyMode::RDMA) { //operation is complete

      if(numops == 1) // complete
        allOpsComplete = true;

      else { // update counter
        ncpyEmInfo->counter++;
        if(ncpyEmInfo->counter == ncpyEmInfo->numOps) {
          allOpsComplete = true;
        }
      }
    } else {
      performRget(ncpyEmInfo, destIndex, extraSize);
    }

    if(!allOpsComplete) // wait for all ops to be complete
      return true;

    if(emMode == ncpyEmApiMode::P2P_RECV) {

      CMI_ZC_MSGTYPE(env) = CMK_REG_NO_ZC_MSG;
      CmiFree(ncpyEmInfo);

      switch(ncpyMode) {
        case CkNcpyMode::MEMCPY:  QdCreate(1);
                                  enqueueNcpyMessage(CkMyPe(), env);
                                  break;
        case CkNcpyMode::CMA   :  if(sendBackToSourceForDereg) {
                                    // Send back to source process to de-register
                                    invokeRemoteNcpyAckHandler(source.pe, (void *)source.refAckInfo, ncpyHandlerIdx::CMA_DEREG_ACK);
                                  }
                                  QdCreate(1);
                                  enqueueNcpyMessage(CkMyPe(), env);
                                  break;

        default                :  CmiAbort("Invalid transfer mode\n");
                                  break;
      }
    } else if(emMode == ncpyEmApiMode::BCAST_RECV) {

      switch(ncpyMode) {
        case CkNcpyMode::MEMCPY:  // Invoke the bcast Ack Handler after 'numops' memcpy operations are complete
                                  CkAssert(source.refAckInfo != NULL);
                                  CkRdmaEMBcastAckHandler((void *)source.refAckInfo);
                                  handleMsgOnChildPostCompletionForRecvBcast(env, ncpyEmInfo);
                                  break;

        case CkNcpyMode::CMA   :  // Invoke the Ack handler on the parent node to signal completion
                                  sendAckMsgToParent(env);
                                  if(t->child_count == 0) {
                                    handleMsgOnChildPostCompletionForRecvBcast(env, ncpyEmInfo);
                                  } else {
                                    // Allocate a structure NcpyBcastInterimAckInfo to maintain state for ack handling
                                    NcpyBcastInterimAckInfo *bcastAckInfo = allocateInterimNodeAckObj(env, NULL, CkMyPe());
                                    bcastAckInfo->ncpyEmInfo = ncpyEmInfo;
                                    handleMsgOnInterimPostCompletionForRecvBcast(env, bcastAckInfo, CkMyPe());
                                  }
                                  break;

        default                :  CmiAbort("Invalid transfer mode\n");
                                  break;
      }
    } else {
      CmiAbort("Invalid operation mode\n");
    }
    return true;

  } else if(CMI_ZC_MSGTYPE(env) == CMK_ZC_BCAST_RECV_DONE_MSG) {

    memcpy(destBuffer, post.srcBuffer, post.srcSize);

    post.srcBuffer = destBuffer;
    post.srcSize = destSize;

    post.ncpyEmInfo->counter++;

    if(env->getMsgtype() == ArrayBcastFwdMsg) {

      CkArray *mgr = getArrayMgrFromMsg(env);
      CkMigratable *elem = mgr->getEltFromArrMgr(post.arrayIndex);
      int localIndex = mgr->getEltLocalIndex(post.arrayIndex);
      (post.ncpyEmInfo->tagArray)[CmiMyRank()][localIndex * numops + post.opIndex] = post.tag;

      if(post.ncpyEmInfo->counter == numops) {
        mgr->forwardZCMsgToSpecificElem(env, elem);
        CmiFree(post.ncpyEmInfo);
      }

    } else if(env->getMsgtype() == ForBocMsg) {
      (post.ncpyEmInfo->tagArray)[CmiMyRank()][post.opIndex] = post.tag;

      if(post.ncpyEmInfo->counter == numops) {
        CmiHandleMessage(env);
        CmiFree(post.ncpyEmInfo);
      }
    }
    return false;

  } else {

    CkAbort("CkPerformRget: Incorrect message type\n");
    return false;
  }
}
/************************* End of Zerocopy Post API Async Support **********************/

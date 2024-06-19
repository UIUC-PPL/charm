/*
 * Charm Onesided API Utility Functions
 */

#ifndef _CKRDMA_H_
#define _CKRDMA_H_

#include "envelope.h"
#include "ckcallback.h"
#include "conv-rdma.h"
#include <vector>

/*********************************** Zerocopy Direct API **********************************/

#define CK_BUFFER_REG     CMK_BUFFER_REG
#define CK_BUFFER_UNREG   CMK_BUFFER_UNREG
#define CK_BUFFER_PREREG  CMK_BUFFER_PREREG
#define CK_BUFFER_NOREG   CMK_BUFFER_NOREG

#define CK_BUFFER_DEREG     CMK_BUFFER_DEREG
#define CK_BUFFER_NODEREG   CMK_BUFFER_NODEREG

#define CkRdmaAlloc CmiRdmaAlloc
#define CkRdmaFree  CmiRdmaFree

#define CkNcpyStatus CmiNcpyStatus
#define CkNcpyMode CmiNcpyMode


struct NcpyBcastRecvPeerAckInfo;

// P2P_SEND mode is used for EM P2P Send API
// BCAST_SEND mode is used for EM BCAST Send API
// P2P_RECV mode is used for EM P2P Recv API
// BCAST_RECV mode is used for EM BCAST Send API
enum class ncpyEmApiMode : char { P2P_SEND, BCAST_SEND, P2P_RECV, BCAST_RECV };

struct CkNcpyBufferPost;

struct NcpyBcastRecvPeerAckInfo{
  envelope *msg;
  int peerParentPe;

#if CMK_SMP
  std::atomic<int> numPeers;
#else
  int numPeers;
#endif

#if CMK_SMP
  std::atomic<int> numElems;
#else
  int numElems;
#endif

#if CMK_SMP
    int getNumPeers() const {
       return numPeers.load(std::memory_order_acquire);
    }
    void setNumPeers(int r) {
       return numPeers.store(r, std::memory_order_release);
    }
    int incNumPeers() {
        return numPeers.fetch_add(1, std::memory_order_release);
    }
    int decNumPeers() {
         return numPeers.fetch_sub(1, std::memory_order_release);
    }
#else
    int getNumPeers() const { return numPeers; }
    void setNumPeers(int r) { numPeers = r; }
    int incNumPeers() { return numPeers++; }
    int decNumPeers() { return numPeers--; }
#endif

#if CMK_SMP
    int getNumElems() const {
       return numElems.load(std::memory_order_acquire);
    }
    void setNumElems(int r) {
       return numElems.store(r, std::memory_order_release);
    }
    int incNumElems() {
        return numElems.fetch_add(1, std::memory_order_release);
    }
    int incNumElems(int r) {
        return numElems.fetch_add(r, std::memory_order_release);
    }
    int decNumElems() {
         return numElems.fetch_sub(1, std::memory_order_release);
    }
#else
    int getNumElems() const { return numElems; }
    void setNumElems(int r) { numElems = r; }
    int incNumElems() { return numElems++; }
    int incNumElems(int r) { numElems += r; return numElems; }
    int decNumElems() { return numElems--; }
#endif

    void init(int nElems, int nPeers, envelope *myMsg, int ppPe) {
      setNumElems(nElems);
      setNumPeers(nPeers);
      msg = myMsg;
      peerParentPe = ppPe;
    }
};

// NOTE: Inside CkRdmaIssueRgets, a large message allocation is made consisting of space
// for the destination or receiver buffers and some additional information required for processing
// and acknowledgment handling. The space for additional information is typically equal to
// sizeof(NcpyEmInfo) + numops * sizeof(NcpyEmBufferInfo)

// This structure is used to store zerocopy information associated with an entry method
// invocation which uses the RDMA mode of transfer in Zerocopy Entry Method API.
// A variable of the structure stores the information in order to access it after the
// completion of the Rget operation (which is an asynchronous call) in order to invoke
// the entry method
struct NcpyEmInfo{
  int numOps; // number of zerocopy operations i.e number of buffers sent using CkSendBuffer
  int counter; // used for tracking the number of completed RDMA operations
  int pe;
  ncpyEmApiMode mode; // used to distinguish between p2p and bcast
  void *msg; // pointer to the Charm++ message which will be enqueued after completion of all Rgets
  void *forwardMsg; // used for the ncpy broadcast api

  std::vector<int> *tagArray;

  NcpyBcastRecvPeerAckInfo *peerAckInfo;

};

// Class to represent an Zerocopy buffer
// CkSendBuffer(....) passed by the user internally translates to a CkNcpyBuffer
class CkNcpyBuffer : public CmiNcpyBuffer {
  public:

  NcpyEmInfo *ncpyEmInfo;

  // callback to be invoked on the sender/receiver
  CkCallback cb;

  CkNcpyBuffer() : CmiNcpyBuffer() {}

  explicit CkNcpyBuffer(const void *ptr_, size_t cnt_, unsigned short int regMode_=CK_BUFFER_REG, unsigned short int deregMode_=CK_BUFFER_DEREG) {
    cb = CkCallback(CkCallback::ignore);
    CmiNcpyBuffer::init(ptr_, cnt_, regMode_, deregMode_);
  }

  explicit CkNcpyBuffer(const void *ptr_, size_t cnt_, CkCallback &cb_, unsigned short int regMode_=CK_BUFFER_REG, unsigned short int deregMode_=CK_BUFFER_DEREG) {
    init(ptr_, cnt_, cb_, regMode_, deregMode_);
  }

  void print() {
    CkPrintf("[%d][%d][%d] CkNcpyBuffer print: ptr:%p, size:%zu, pe:%d, regMode=%d, deregMode=%d, ref:%p, refAckInfo:%p\n", CmiMyPe(), CmiMyNode(), CmiMyRank(), ptr, cnt, pe, regMode, deregMode, ref, refAckInfo);
  }

  void init(const void *ptr_, size_t cnt_, CkCallback &cb_, unsigned short int regMode_=CK_BUFFER_REG, unsigned short int deregMode_=CK_BUFFER_DEREG) {
    cb   = cb_;
    CmiNcpyBuffer::init(ptr_, cnt_, regMode_, deregMode_);
  }

  CkNcpyStatus get(CkNcpyBuffer &source);
  CkNcpyStatus put(CkNcpyBuffer &destination);

  void pup(PUP::er &p) {
    CmiNcpyBuffer::pup(p);
    p|cb;
    p((char *)&ncpyEmInfo, sizeof(NcpyEmInfo));
  }

  friend void CkRdmaDirectAckHandler(void *ack);

  friend void CkRdmaEMBcastAckHandler(void *ack);

  friend void constructSourceBufferObject(NcpyOperationInfo *info, CkNcpyBuffer &src);
  friend void constructDestinationBufferObject(NcpyOperationInfo *info, CkNcpyBuffer &dest);

  friend envelope* CkRdmaIssueRgets(envelope *env, ncpyEmApiMode emMode, void *forwardMsg);
  friend void CkRdmaIssueRgets(envelope *env, void **arrPtrs, int *arrSizes, int localIndex, CkNcpyBufferPost *postStructs);

  friend void readonlyGet(CkNcpyBuffer &src, CkNcpyBuffer &dest, void *refPtr);
  friend void readonlyCreateOnSource(CkNcpyBuffer &src);


  friend void performEmApiNcpyTransfer(CkNcpyBuffer &source, CkNcpyBuffer &dest, int opIndex, CmiSpanningTreeInfo *t, NcpyEmInfo *ref, int extraSize, CkNcpyMode ncpyMode, int rootNode, ncpyEmApiMode emMode);

  friend void performEmApiRget(CkNcpyBuffer &source, CkNcpyBuffer &dest, int opIndex, NcpyEmInfo *ref, int extraSize, int rootNode, ncpyEmApiMode emMode);

  friend void performEmApiCmaTransfer(CkNcpyBuffer &source, CkNcpyBuffer &dest, CmiSpanningTreeInfo *t, ncpyEmApiMode emMode);

  friend void performEmApiMemcpy(CkNcpyBuffer &source, CkNcpyBuffer &dest, ncpyEmApiMode emMode);

  friend void deregisterMemFromMsg(envelope *env, bool isRecv);
  friend void CkRdmaEMDeregAndAckHandler(void *ack);
};

// Ack handler for the Zerocopy Direct API
// Invoked on the completion of any RDMA operation calling using the Direct API
void CkRdmaDirectAckHandler(void *ack);

// Method to invoke a callback on a particular pe with a CkNcpyBuffer being passed
// as a part of a CkDataMsg. This method is used to invoke callbacks on specific pes
// after the completion of the Zerocopy Direct API operation
void invokeCallback(void *cb, int pe, CkNcpyBuffer &buff);

// Returns CkNcpyMode::MEMCPY if both the PEs are the same and memcpy can be used
// Returns CkNcpyMode::CMA if both the PEs are in the same physical node and CMA can be used
// Returns CkNcpyMode::RDMA if RDMA needs to be used
CkNcpyMode findTransferMode(int srcPe, int destPe);

void invokeSourceCallback(NcpyOperationInfo *info);

void invokeDestinationCallback(NcpyOperationInfo *info);

// Method to enqueue a message after the completion of an payload transfer
void enqueueNcpyMessage(int destPe, void *msg);

// Method to increment Qd counter
inline void zcQdIncrement();

/*********************************** Zerocopy Entry Method API ****************************/
static inline CkNcpyBuffer CkSendBuffer(const void *ptr_, CkCallback &cb_, unsigned short int regMode_=CK_BUFFER_REG, unsigned short int deregMode_=CK_BUFFER_DEREG) {
  return CkNcpyBuffer(ptr_, 0, cb_, regMode_, deregMode_);
}

static inline CkNcpyBuffer CkSendBuffer(const void *ptr_, unsigned short int regMode_=CK_BUFFER_REG, unsigned short int deregMode_=CK_BUFFER_DEREG) {
  return CkNcpyBuffer(ptr_, 0, regMode_, deregMode_);
}

void CkRdmaAsyncPostPreprocess(envelope *env, int numops, CkNcpyBufferPost *postStructs, CmiUInt8 arrayIndex, void *ackInfo);

void CkRdmaAsyncPostPreprocess(envelope *env, int numops, CkNcpyBufferPost *post);

// This structure is used to store the buffer information specific to each buffer being sent
// using the Zerocopy Entry Method API. A variable of the structure stores the information associated
// with each buffer
struct NcpyEmBufferInfo{
  int index;  // Represents the index of the buffer information (from 0,1... numops - 1)
  NcpyOperationInfo ncpyOpInfo; // Stores all the information required for the zerocopy operation
};


/*
 * Extract ncpy buffer information from the metadata message,
 * allocate buffers and issue ncpy calls (either memcpy or cma read or rdma get)
 */
envelope* CkRdmaIssueRgets(envelope *env, ncpyEmApiMode emMode, void *forwardMsg = NULL);

void CkRdmaIssueRgets(envelope *env, void **arrPtrs, int *arrSizes, int localIndex, CkNcpyBufferPost *postStructs);

void handleEntryMethodApiCompletion(NcpyOperationInfo *info);

void handleReverseEntryMethodApiCompletion(NcpyOperationInfo *info);

// Method called to pack rdma pointers
void CkPackRdmaPtrs(char *msgBuf);

// Method called to pack rdma pointers
void CkUnpackRdmaPtrs(char *msgBuf);

// Determine the number of ncpy ops and the sum of the ncpy buffer sizes
// from the metadata message
void getRdmaNumopsAndBufsize(envelope *env, int &numops, int &bufsize, int &rootNode);

// Ack handler function for the nocopy EM API
void CkRdmaEMAckHandler(int destPe, void *ack);

void CkRdmaEMBcastPostAckHandler(void *msg);

// Structure is used for storing source buffer info to de-reg and invoke acks after completion of CMA operations
struct NcpyP2PAckInfo{
  int numOps;
  #if defined(__GNUC__) || defined(__clang__)
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wpedantic"
  #endif
  CkNcpyBuffer src[0];
  #if defined(__GNUC__) || defined(__clang__)
  #pragma GCC diagnostic pop
  #endif
};

/***************************** Zerocopy Bcast Entry Method API ****************************/
struct NcpyBcastAckInfo{
  int numChildren;
#if CMK_SMP
  // Counter is an atomic variable in the SMP mode because on the root node, both the
  // worker thread (in the case of a memcpy transfer) and the comm thread (CMA or RDMA transfer)
  // can increment the variable.
  std::atomic<int> counter;
#else
  int counter;
#endif
  int pe;
  int numops;
  bool isRoot;

#if CMK_SMP
  int getCounter() const {
    return counter.load(std::memory_order_acquire);
  }
  void setCounter(int r) {
    return counter.store(r, std::memory_order_release);
  }
  int incCounter() {
    return counter.fetch_add(1, std::memory_order_release);
  }
  int decCounter() {
    return counter.fetch_sub(1, std::memory_order_release);
  }
#else
  int getCounter() const { return counter; }
  void setCounter(int r) { counter = r; }
  int incCounter() { return counter++; }
  int decCounter() { return counter--; }
#endif
};

struct NcpyBcastRootAckInfo : public NcpyBcastAckInfo {
  #if defined(__GNUC__) || defined(__clang__)
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wpedantic"
  #endif
  CkNcpyBuffer src[0];
  #if defined(__GNUC__) || defined(__clang__)
  #pragma GCC diagnostic pop
  #endif
};

struct NcpyBcastInterimAckInfo : public NcpyBcastAckInfo {
  void *msg;
  bool isRecv;
  bool isArray;
  NcpyEmInfo *ncpyEmInfo;
};

// Method called on the bcast source to store some information for ack handling
void CkRdmaPrepareBcastMsg(envelope *env);

// Method called on the p2p source to store information for de-reg and ack handling for CMA transfers
void CkRdmaPrepareZCMsg(envelope *env, int node);

// Method called on ZC API source (internally calls CkRdmaPrepareBcastMsg or CkRdmaPrepareZCMsg)
void CkRdmaPrepareP2PMsg(envelope *env);

void CkReplaceSourcePtrsInBcastMsg(envelope *env, NcpyBcastInterimAckInfo *bcastAckInfo, int origPe);

// Method called to extract the parent bcastAckInfo from the received message for ack handling
const void *getParentBcastAckInfo(void *msg, int &srcPe);

// Allocate a NcpyBcastInterimAckInfo and return the pointer
NcpyBcastInterimAckInfo *allocateInterimNodeAckObj(envelope *myEnv, envelope *myChildEnv, int pe);

void forwardMessageToChildNodes(envelope *myChildrenMsg, UChar msgType);

void forwardMessageToPeerNodes(envelope *myMsg, UChar msgType);

void handleBcastEntryMethodApiCompletion(NcpyOperationInfo *info);

void handleBcastReverseEntryMethodApiCompletion(NcpyOperationInfo *info);

void deregisterMemFromMsg(envelope *env, bool isRecv);

void handleMsgUsingCMAPostCompletionForSendBcast(envelope *copyenv, envelope *env, CkNcpyBuffer &source);

void processBcastSendEmApiCompletion(NcpyEmInfo *ncpyEmInfo, int destPe);

// Method called on intermediate nodes after RGET to switch old source pointers with my pointers
void CkReplaceSourcePtrsInBcastMsg(envelope *prevEnv, envelope *env, void *bcastAckInfo, int origPe);

void processBcastRecvEmApiCompletion(NcpyEmInfo *ncpyEmInfo, int destPe);

// Method called on the root node and other intermediate parent nodes on completion of RGET through ZC Bcast
void CkRdmaEMBcastAckHandler(void *ack);

void handleMsgOnChildPostCompletionForRecvBcast(envelope *env, NcpyEmInfo *ncpyEmInfo);

void handleMsgOnInterimPostCompletionForRecvBcast(envelope *env, NcpyBcastInterimAckInfo *bcastAckInfo, int pe);



/***************************** Zerocopy Readonly Bcast Support ****************************/

/* Support for Zerocopy Broadcast of large readonly variables */
CkpvExtern(int, _numPendingRORdmaTransfers);

struct NcpyROBcastBuffAckInfo {
  const void *ptr;

  int regMode;

  int pe;

  // machine specific information about the buffer
  #if defined(__GNUC__) || defined(__clang__)
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wpedantic"
  #endif
  char layerInfo[CMK_COMMON_NOCOPY_DIRECT_BYTES + CMK_NOCOPY_DIRECT_BYTES];
  #if defined(__GNUC__) || defined(__clang__)
  #pragma GCC diagnostic pop
  #endif
};

struct NcpyROBcastAckInfo {
  int numChildren;
  int counter;
  int numops;
  bool isRoot;
  #if defined(__GNUC__) || defined(__clang__)
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wpedantic"
  #endif
  NcpyROBcastBuffAckInfo buffAckInfo[0];
  #if defined(__GNUC__) || defined(__clang__)
  #pragma GCC diagnostic pop
  #endif
};

void readonlyUpdateNumops();

void readonlyAllocateOnSource();

void readonlyCreateOnSource(CkNcpyBuffer &src);

void readonlyGet(CkNcpyBuffer &src, CkNcpyBuffer &dest, void *refPtr);

void readonlyGetCompleted(NcpyOperationInfo *ncpyOpInfo);

#if CMK_SMP
void updatePeerCounterAndPush(envelope *env);
#endif

CkArray* getArrayMgrFromMsg(envelope *env);

void sendAckMsgToParent(envelope *env);

void sendRecvDoneMsgToPeers(envelope *env, CkArray *mgr);

/***************************** Other Util Methods ****************************/

// Function declaration for EM Ncpy Ack handler initialization
void initEMNcpyAckHandler(void);

// Broadcast API support
void CmiForwardProcBcastMsg(int size, char *msg); // for forwarding proc messages to my child nodes
void CmiForwardNodeBcastMsg(int size, char *msg); // for forwarding node queue messages to my child nodes

void CmiForwardMsgToPeers(int size, char *msg); // for forwarding messages to my peer PEs

#if CMK_REG_REQUIRED
void CmiInvokeRemoteDeregAckHandler(int pe, NcpyOperationInfo *info);
#endif

inline void invokeRemoteNcpyAckHandler(int pe, void *ref, ncpyHandlerIdx opMode);

// Handler method invoked on the ZC p2p API source for de-registration and ack handling for CMA transfers
void CkRdmaEMDeregAndAckHandler(void *ack);

inline bool isDeregReady(CkNcpyBuffer &buffInfo);

inline void deregisterDestBuffer(NcpyOperationInfo *ncpyOpInfo);
inline void deregisterSrcBuffer(NcpyOperationInfo *ncpyOpInfo);

inline void invokeCmaDirectRemoteDeregAckHandler(CkNcpyBuffer &buffInfo, ncpyHandlerIdx opMode);
int getRootNode(envelope *env);


// Function declaration for EM Ncpy Ack handler initialization
void initEMNcpyAckHandler(void);

struct zcPupPendingRgetsMsg {
  char cmicore[CmiMsgHeaderSizeBytes];
  CmiUInt8 id;
  int numops;
  CkGroupID locMgrId;
#if CMK_SMP
  int pe;
#endif
};


void zcPupGetCompleted(NcpyOperationInfo *ncpyOpInfo);

void _zcpyPupCompleteHandler(zcPupPendingRgetsMsg *msg);

class CkLocMgr;
void zcPupIssueRgets(CmiUInt8 id, CkLocMgr *locMgr);

void CkRdmaZCPupCustomHandler(void *ack);

void _ncpyAckHandler(ncpyHandlerMsg *msg);

/****************************** Channel API ******************************/

#if CMK_GPU_COMM
// Channel object maintained per chare
class CkChannel {
private:
  int id;
  int peer_pe;
  int send_counter;
  int recv_counter;

public:
  CkChannel() : id(-1), peer_pe(-1), send_counter(0), recv_counter(0) {}
  CkChannel(int id_) : CkChannel() { id = id_; }
  CkChannel(int id_, const CProxyElement_ArrayBase &proxy);
  CkChannel(int id_, const CProxyElement_Group &proxy);
  CkChannel(int id_, const CProxyElement_NodeGroup &proxy);

  int getID() { return id; }
  int getPeerPe() { return peer_pe; }

  // Limitation: you can only send callbacks to the source or destination PE
  void send(const void* ptr, size_t size, CkFuture* fut);
  void send(const void* ptr, size_t size, bool cb_self, const CkCallback& cb, void* msg = nullptr);
  void recv(const void* ptr, size_t size, CkFuture* fut);
  void recv(const void* ptr, size_t size, bool cb_self, const CkCallback& cb, void* msg = nullptr);
};

struct CkChannelMetadata {
  bool use_cb;
  CkCallback cb;
  int cb_pe;
  void* msg;
  CkFuture* fut;

  CkChannelMetadata() : use_cb(false), cb_pe(-1), msg(nullptr), fut(nullptr) {}
};

void CkChannelHandler(void* data);
#endif // CMK_GPU_COMM

/****************************** End of Channel API ******************************/

void setNcpyEmInfo(NcpyEmInfo *ref, envelope *env, int &numops, void *forwardMsg, ncpyEmApiMode emMode);

// Struct is used to store posted buffer information
// Information passed in a CkPostBuffer call is used to construct CkPostedBuffer
struct CkPostedBuffer {
  void *buffer;
  int bufferSize;
};

// Struct passed in a ZC Post Entry Method to allow receiver side to post
// This structure variable is passed in a CkMatchBuffer call in the Post EM
struct CkNcpyBufferPost {
  // regMode
  unsigned short int regMode;

  // deregMode
  unsigned short int deregMode;

  // index within message
  int index;

  bool postAsync;

  size_t tag;

  // NcpyEmInfo
  NcpyEmInfo *ncpyEmInfo;

  void *srcBuffer;

  size_t srcSize;

  int opIndex;

  CmiUInt8 arrayIndex;
};

void handleArrayMsgOnChildPostCompletionForRecvBcast(envelope *env, NcpyEmInfo *ncpyEmInfo);

void handleGroupMsgOnChildPostCompletionForRecvBcast(envelope *env, NcpyEmInfo *ncpyEmInfo);

void handleNGMsgOnChildPostCompletionForRecvBcast(envelope *env, NcpyEmInfo *ncpyEmInfo);

void CkMatchBuffer(CkNcpyBufferPost *post, int index, int tag);

void CkMatchNodeBuffer(CkNcpyBufferPost *post, int index, int tag);

void updatePeerCounter(NcpyEmInfo *ncpyEmInfo);

void updateTagArray(envelope *env, int localElems);

void setPosted(std::vector<int> *tagArray, envelope *env, CmiUInt8 elemIndex, int numops, int opIndex);

bool isUnposted(std::vector<int> *tagArray, envelope *env, CmiUInt8 elemIndex, int numops);

void *extractStoredBuffer(std::vector<int> *tagArray, envelope *env, CmiUInt8 elemIndex, int numops, int opIndex);

int CkPerformRget(CkNcpyBufferPost &post, void *destBuffer, int destSize);

void setPostStruct(CkNcpyBufferPost *ncpyPost, int index, CkNcpyBuffer &buffObj, CmiUInt8 elemIndex);

void initPostStruct(CkNcpyBufferPost *ncpyPost, int index);

void CkPostBufferInternal(void *destBuffer, size_t destSize, int tag);
void CkPostNodeBufferInternal(void *destBuffer, size_t destSize, int tag);

namespace sizeofutils {
  template <typename T>
  static inline constexpr size_t safe_sizeof(T * ptr)
  {
      return sizeof(T);
  }
  
  template <>
  inline constexpr size_t safe_sizeof(void * ptr)
  {
      return 1;
  }
}

template <typename T>
void CkPostBuffer(T *buffer, size_t size, int tag) {
  int destSize = (std::is_same<T, void>::value) ? size : sizeofutils::safe_sizeof(buffer) * size;
  void *destBuffer = (void *)buffer;
  CkPostBufferInternal(destBuffer, destSize, tag);
}

template <typename T>
void CkPostNodeBuffer(T *buffer, size_t size, int tag) {
  int destSize = (std::is_same<T, void>::value) ? size : sizeofutils::safe_sizeof(buffer) * size;
  void *destBuffer = (void *)buffer;
  CkPostNodeBufferInternal(destBuffer, destSize, tag);
}

#endif

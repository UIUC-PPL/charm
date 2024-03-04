#ifndef _CONV_RDMA_H
#define _CONV_RDMA_H

#include "cmirdmautils.h"
#include "pup.h"

/*********************************** Zerocopy Direct API **********************************/
typedef void (*RdmaAckCallerFn)(void *token);

/* Support for Direct API */
void CmiSetRdmaCommonInfo(void *info, const void *ptr, int size);
int CmiGetRdmaCommonInfoSize(void);

void CmiSetRdmaBufferInfo(void *info, const void *ptr, int size, unsigned short int mode);

// Function to set the ack handler for the Direct API
void CmiSetDirectNcpyAckHandler(RdmaAckCallerFn fn);

/* CmiIssueRget initiates an RDMA read operation, transferring 'size' bytes of data from the address space of 'srcPe' to local address, 'destAddr'.
 * When the runtime invokes srcAck on the source (target), it indicates safety to overwrite or free the srcAddr buffer.
 * When the runtime invokes destAck on the destination (initiator), it indicates that the data has been successfully received in the
 * destAddr buffer.
 */
void CmiIssueRget(NcpyOperationInfo *ncpyOpInfo);

/* CmiIssueRput initiates an RDMA write operation, transferring 'size' bytes of data from the local address, 'srcAddr' to the address space of 'destPe'.
 * When the runtime invokes srcAck on the source (initiator), it indicates safety to overwrite or free the srcAddr buffer.
 * When the runtime invokes destAck on the destination (target), it indicates that the data has been successfully received in the
 * destAddr buffer.
 */

void CmiIssueRput(NcpyOperationInfo *ncpyOpInfo);

void CmiDeregisterMem(const void *ptr, void *info, int pe, unsigned short int mode);

#if CMK_USE_CMA
void CmiIssueRgetUsingCMA(
  const void* srcAddr,
  void *srcInfo,
  int srcPe,
  const void* destAddr,
  void *destInfo,
  int destPe,
  size_t size);

void CmiIssueRputUsingCMA(
  const void* destAddr,
  void *destInfo,
  int destPe,
  const void* srcAddr,
  void *srcInfo,
  int srcPe,
  size_t size);
#endif

// Allocation from pool
void *CmiRdmaAlloc(int size);

int CmiDoesCMAWork(void);
// Function declaration for supporting generic Direct Nocopy API
void CmiOnesidedDirectInit(void);

void CmiSetNcpyAckSize(int ackSize);

#ifndef CMK_NOCOPY_DIRECT_BYTES

#if defined(_WIN32)
#define CMK_NOCOPY_DIRECT_BYTES 1
/* It is required to declare CMK_NOCOPY_DIRECT_BYTES to 1 instead of 0
 * as this avoids the C2229 error (illegal zero-sized array)
 * for char layerInfo[CMK_NOCOPY_DIRECT_BYTES] which is seen for
 * a 0 sized array on VC++
 */
#else
#define CMK_NOCOPY_DIRECT_BYTES 0
#endif // end of if defined(_WIN32)

#endif // end of ifndef CMK_NOCOPY_DIRECT_BYTES

#ifndef CMK_COMMON_NOCOPY_DIRECT_BYTES
#define CMK_COMMON_NOCOPY_DIRECT_BYTES 0
#endif

// Represents the mode of host-side zerocopy transfer
// CkNcpyMode::MEMCPY indicates that the PEs are on the logical node and memcpy can be used
// CkNcpyMode::CMA indicates that the PEs are on the same physical node and CMA can be used
// CkNcpyMode::RDMA indicates that the neither MEMCPY or CMA can be used and REMOTE Direct Memory Access needs to be used
enum class CmiNcpyMode : char { MEMCPY, CMA, RDMA };

// Represents the completion status of the zerocopy transfer (used as a return value for CkNcpyBuffer::get & CkNcpyBuffer:::put)
// CMA and MEMCPY transfers complete instantly and return CkNcpyStatus::complete
// RDMA transfers use a remote asynchronous call and hence return CkNcpyStatus::incomplete
enum class CmiNcpyStatus : char { incomplete, complete };

// Represents the remote handler tag that should be invoked
// ncpyHandlerIdx::EM_ACK tag is used to remotely invoke CkRdmaEMAckHandler
// ncpyHandlerIdx::BCAST_ACK tag is used to remotely invoke CkRdmaEMBcastAckHandler
// ncpyHandlerIdx::BCAST_POST_ACK is used to remotely invoke CkRdmaEMBcastPostAckHandler
// ncpyHandlerIdx::CMA_DEREG_ACK is used to remotely invoke CkRdmaEMDeregAndAckHandler
enum class ncpyHandlerIdx: char {
  EM_ACK,
  BCAST_ACK,
  BCAST_POST_ACK,
  CMA_DEREG_ACK,
  CMA_DEREG_ACK_DIRECT,
  CMA_DEREG_ACK_ZC_PUP,
  CMA_DEREG_ACK_ZC_PUP_CUSTOM,
};

class CmiNcpyBuffer {

  //private:
  public:

  // bool to indicate registration for current values of ptr and cnt on pe
  bool isRegistered;

  // machine specific information about the buffer
  #if defined(__GNUC__) || defined(__clang__)
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wpedantic"
  #endif
  char layerInfo[CMK_COMMON_NOCOPY_DIRECT_BYTES + CMK_NOCOPY_DIRECT_BYTES];
  #if defined(__GNUC__) || defined(__clang__)
  #pragma GCC diagnostic pop
  #endif

#if CMK_ERROR_CHECKING
  void checkRegModeIsValid() {
    if (regMode > CMK_BUFFER_NOREG)
      CmiAbort("checkRegModeIsValid: Invalid value for regMode!\n");
  }

  void checkDeregModeIsValid() {
    if (deregMode < CMK_BUFFER_DEREG || deregMode > CMK_BUFFER_NODEREG)
      CmiAbort("checkDeregModeIsValid: Invalid value for deregMode!\n");
  }
#endif

  // pointer to the buffer
  const void *ptr;

  // number of bytes
  size_t cnt;

  // home pe
  int pe;

  // regMode
  unsigned short int regMode;

  // deregMode
  unsigned short int deregMode;

  // reference pointer
  const void *ref;

  // ack handling pointer used for bcast and CMA p2p transfers
  const void *refAckInfo;

  CmiNcpyBuffer() : isRegistered(false), ptr(NULL), cnt(0), pe(-1), regMode(CMK_BUFFER_REG), deregMode(CMK_BUFFER_DEREG), ref(NULL), refAckInfo(NULL) {}

  explicit CmiNcpyBuffer(const void *ptr_, size_t cnt_, unsigned short int regMode_=CMK_BUFFER_REG, unsigned short int deregMode_=CMK_BUFFER_DEREG) {
    init(ptr_, cnt_, regMode_, deregMode_);
  }

  void print() {
    CmiPrintf("[%d][%d][%d] CmiNcpyBuffer print: ptr:%p, size:%zu, pe:%d, regMode=%d, deregMode=%d, ref:%p, refAckInfo:%p\n", CmiMyPe(), CmiMyNode(), CmiMyRank(), ptr, cnt, pe, regMode, deregMode, ref, refAckInfo);
  }

  void init(const void *ptr_, size_t cnt_, unsigned short int regMode_=CMK_BUFFER_REG, unsigned short int deregMode_=CMK_BUFFER_DEREG) {
    ptr  = ptr_;
    cnt  = cnt_;
    pe   = CmiMyPe();
    regMode = regMode_;
    deregMode = deregMode_;

    isRegistered = false;

#if CMK_ERROR_CHECKING
    // Ensure that regMode is valid
    checkRegModeIsValid();

    // Ensure that deregMode is valid
    checkDeregModeIsValid();
#endif

    ref = NULL;
    refAckInfo = NULL;

    // Register memory everytime new values are initialized
    if(cnt > 0)
      registerMem();
  }

  void setRef(const void *ref_) {
    ref = ref_;
  }

  const void *getRef() {
    return ref;
  }

  // Register(Pin) the memory for the buffer
  void registerMem()
  {
    // Check that this object is local when registerMem is called
    CmiAssert(CmiNodeOf(pe) == CmiMyNode());

    // Set machine layer information when regMode is not CMK_BUFFER_NOREG
    if(regMode != CMK_BUFFER_NOREG) {

      CmiSetRdmaCommonInfo(&layerInfo[0], ptr, cnt);

      /* Set the pointer layerInfo unconditionally for layers that don't require pinning (MPI, PAMI)
       * or if regMode is REG, PREREG on layers that require pinning (GNI, Verbs, OFI, UCX) */
#if CMK_REG_REQUIRED
      if(regMode == CMK_BUFFER_REG || regMode == CMK_BUFFER_PREREG)
#endif
      {
        CmiSetRdmaBufferInfo(layerInfo + CmiGetRdmaCommonInfoSize(), ptr, cnt, regMode);
        isRegistered = true;
      }
    }
  }

  void setMode(unsigned short int regMode_) { regMode = regMode_; }

  // Deregister(Unpin) the memory that is registered for the buffer
  void deregisterMem() {
    // Check that this object is local when deregisterMem is called
    CmiAssert(CmiNodeOf(pe) == CmiMyNode());

    if(isRegistered == false)
      return;

#if CMK_REG_REQUIRED
    if(regMode != CMK_BUFFER_NOREG) {
      CmiDeregisterMem(ptr, layerInfo + CmiGetRdmaCommonInfoSize(), pe, regMode);
      isRegistered = false;
    }
#endif
  }

  void pup(PUP::er &p) {
    p((char *)&ptr, sizeof(ptr));
    p((char *)&ref, sizeof(ref));
    p((char *)&refAckInfo, sizeof(refAckInfo));
    p|cnt;
    p|pe;
    p|regMode;
    p|deregMode;
    p|isRegistered;
    PUParray(p, layerInfo, CMK_COMMON_NOCOPY_DIRECT_BYTES + CMK_NOCOPY_DIRECT_BYTES);
  }

  void memcpyGet(CmiNcpyBuffer &source);
  void memcpyPut(CmiNcpyBuffer &destination);

#if CMK_USE_CMA
  void cmaGet(CmiNcpyBuffer &source);
  void cmaPut(CmiNcpyBuffer &destination);
#endif

  NcpyOperationInfo *createNcpyOpInfo(CmiNcpyBuffer &source, CmiNcpyBuffer &destination, int ackSize, char *srcAck, char *destAck, int rootNode, int opMode, void *refPtr);

  void rdmaGet(CmiNcpyBuffer &source, int ackSize, char *srcAck, char *destAck);
  void rdmaPut(CmiNcpyBuffer &destination, int ackSize, char *srcAck, char *destAck);

  friend inline void deregisterBuffer(CmiNcpyBuffer &buffInfo);


};

/***************************** Other Util *********************************/

void invokeZCPupHandler(void *ref, int pe);
inline void deregisterBuffer(CmiNcpyBuffer &buffInfo) {
  CmiDeregisterMem(buffInfo.ptr, buffInfo.layerInfo + CmiGetRdmaCommonInfoSize(), buffInfo.pe, buffInfo.regMode);
  buffInfo.isRegistered = false;
}
CmiNcpyMode findTransferMode(int srcPe, int destPe);
CmiNcpyMode findTransferModeWithNodes(int srcNode, int destNode);


// Converse message to invoke the Ncpy handler on a remote process
struct ncpyHandlerMsg{
  char cmicore[CmiMsgHeaderSizeBytes];
  ncpyHandlerIdx opMode;
  void *ref;
};

struct zcPupSourceInfo{
  CmiNcpyBuffer src;
  std::function<void (void *)> deallocate;
};

void zcPupDone(void *ref);
void zcPupHandler(ncpyHandlerMsg *msg);

zcPupSourceInfo *zcPupAddSource(CmiNcpyBuffer &src);
zcPupSourceInfo *zcPupAddSource(CmiNcpyBuffer &src, std::function<void (void *)> deallocate);

void zcPupGet(CmiNcpyBuffer &src, CmiNcpyBuffer &dest);

/****************************** Channel API ******************************/

#if CMK_GPU_COMM
void CmiChannelSend(int dest_pe, int id, const void*& ptr, size_t size, void* metadata, uint64_t tag);
void CmiChannelRecv(int id, const void*& ptr, size_t size, void* metadata, uint64_t tag);
void CmiChannelHandlerInit(RdmaAckCallerFn fn);
void CmiChannelHandler(void* data);
#endif

#endif

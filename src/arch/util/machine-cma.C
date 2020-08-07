#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sys/uio.h>

// Method checks if process has permissions to use CMA
int CmiInitCma() {
  char buffer   = '0';
  int cma_works = 0;
  int fd;

  // determine permissions
  fd = open("/proc/sys/kernel/yama/ptrace_scope", O_RDONLY);
  if (0 <= fd) {
    if (read (fd, &buffer, 1) != 1) {
      CmiAbort("CMA> reading /proc/sys/kernel/yama/ptrace_scope failed!");
    }
    close(fd);
  }

  if('0' != buffer) {
#if defined PR_SET_PTRACER
    // set ptrace scope to allow attach
    int ret = prctl (PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    if (0 == ret) {
      cma_works = 1;
    }
#endif
  } else {
    cma_works = 1;
  }
  return cma_works;
}

// Method to display thresholds for regular messaging using CMA
void CmiDisplayCMAThresholds(int cma_min_threshold, int cma_max_threshold) {
  if(_Cmi_mynode == 0) {
      if(cma_min_threshold > cma_max_threshold) {
        CmiAbort("CMA size thresholds incorrect! Values should satisfy cma_min_threshold <= cma_min_threshold condition");
      }
      CmiPrintf("Charm++> CMA enabled for within node transfers of messages(sized between %d & %d bytes)\n", cma_min_threshold, cma_max_threshold);
  }
}

/* Used to read from a list of remote buffers and write into a
 * list of local buffers over SHM using the cma library
 */
int readShmCma(
  pid_t remote_pid,
  struct iovec *local,
  struct iovec *remote,
  int numOps,
  size_t total_bytes) {

  ssize_t nread = process_vm_readv(remote_pid, local, numOps, remote, numOps, 0);
  if(nread != total_bytes) {
    CmiAbort("process_vm_readv failed! Size to be read:%zu bytes, Size actually read:%zd bytes (negative value indicates error), error no:%d, error message:%s!\n", total_bytes, nread, errno, strerror(errno));
    return errno;
  }
  return 0;
}

/* Used to write into a list of remote buffers by reading from a
 * list of local buffers over SHM using the cma library
 */
int writeShmCma(
  pid_t remote_pid,
  struct iovec *local,
  struct iovec *remote,
  int numOps,
  size_t total_bytes) {

  ssize_t nwrite = process_vm_writev(remote_pid, local, numOps, remote, numOps, 0);
  if(nwrite != total_bytes) {
    CmiAbort("process_vm_writev failed! Size to be written:%zu bytes, Size actually written:%zd bytes (negative value indicates error), error no:%d, error message:%s!\n", total_bytes, nwrite, errno, strerror(errno));
    return errno;
  }
  return 0;
}

// Metadata of a buffer that is to be sent to another intra-host process over CMA
typedef struct _cma_src_buffer_info {
  int srcPE;
  pid_t srcPid;
  void *srcAddr;
  size_t size;
}CmaSrcBufferInfo_t;

// Method invoked on receiving a CMK_CMA_MD_MSG
// This method uses the buffer metadata to perform a CMA read. It also modifies *sizePtr & *msgPtr to
// point to the buffer message
void handleOneCmaMdMsg(size_t *sizePtr, char **msgPtr) {
  struct iovec local, remote;
  char *destAddr;

  // Get buffer metadata
  CmaSrcBufferInfo_t *bufInfo = (CmaSrcBufferInfo_t *)(*msgPtr + CmiMsgHeaderSizeBytes);

  // Allocate a buffer to hold the buffer
  destAddr = (char *)CmiAlloc(bufInfo->size);

  local.iov_base = (void *)destAddr;
  local.iov_len  = bufInfo->size;

  remote.iov_base = bufInfo->srcAddr;
  remote.iov_len  = bufInfo->size;

  // Perform CMA read into destAddr
  readShmCma(bufInfo->srcPid,
             &local,
             &remote,
             1,
             bufInfo->size);

  // Send the buffer md msg back as an ack msg to signal CMA read completion in order to free buffers
  // on the source process
  CMI_CMA_MSGTYPE(*msgPtr) = CMK_CMA_ACK_MSG;

  CmiInterSendNetworkFunc(bufInfo->srcPE,
                          CmiMyPartition(),
                          CmiMsgHeaderSizeBytes + sizeof(CmaSrcBufferInfo_t),
                          *msgPtr,
                          P2P_SYNC);

  // Reassign *msgPtr to the buffer
  *msgPtr = destAddr;
  // Reassign *sizePtr to the size of the buffer
  *sizePtr = local.iov_len;
}


// Method invoked on receiving CMK_CMA_ACK_MSG
// This method frees the buffer and the received buffer ack msg
void handleOneCmaAckMsg(size_t size, void *msg) {

  // Get buffer metadata
  CmaSrcBufferInfo_t *bufInfo = (CmaSrcBufferInfo_t *)((char *)msg + CmiMsgHeaderSizeBytes);

  // Free the large buffer sent
  CmiFree(bufInfo->srcAddr);

  // Free this ack message
  CmiFree(msg);
}

// Method invoked to send the buffer via CMA
// This method creates a buffer metadata msg from a buffer and modifies the *msgPtr and *sizePtr to point to
// the buffer metadata msg.
void CmiSendMessageCma(char **msgPtr, size_t *sizePtr) {

  // Send buffer metadata instead of original msg
  // Buffer metadata msg consists of pid, addr, size for the other process to perform a read through CMA
  char *cmaBufMdMsg = (char *)CmiAlloc(CmiMsgHeaderSizeBytes + sizeof(CmaSrcBufferInfo_t));
  CmaSrcBufferInfo_t *bufInfo = (CmaSrcBufferInfo_t *)(cmaBufMdMsg + CmiMsgHeaderSizeBytes);
  bufInfo->srcPE  = CmiMyPe();
  bufInfo->srcPid = getpid();
  bufInfo->srcAddr = *msgPtr;
  bufInfo->size    = *sizePtr;

  // Tag this message as a CMA buffer md message
  CMI_CMA_MSGTYPE(cmaBufMdMsg) = CMK_CMA_MD_MSG;

  // Reassign *sizePtr to store the size of the buffer md msg
  *sizePtr = CmiMsgHeaderSizeBytes + sizeof(CmaSrcBufferInfo_t);

  // Reassign *msgPtr to point to the buffer metadata msg
  *msgPtr = (char *)cmaBufMdMsg;
}

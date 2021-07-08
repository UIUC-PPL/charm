#ifndef MACHINE_ONESIDED_H_
#define MACHINE_ONESIDED_H_

#include "mempool.h"

void _initOnesided();

void PumpOneSidedRDMATransactions(gni_cq_handle_t rdma_cq, CmiNodeLock rdma_cq_lock);

/* Support for Nocopy Direct API */

/* Type of RDMA transaction
   This is used in handling each type of RDMA transaction appropriately on completion
 */
enum CMK_RDMA_TYPE : unsigned char {
  DIRECT_SEND_RECV=1,
  DIRECT_SEND_RECV_UNALIGNED
};

// Machine specific information for a nocopy pointer
typedef struct _cmi_gni_rzv_rdma_pointer {
  // memory handle for the buffer
  gni_mem_handle_t mem_hndl;
} CmiGNIRzvRdmaPtr_t;

/* Compiler checks to ensure that CMK_NOCOPY_DIRECT_BYTES in conv-common.h
 * is set to sizeof(CmiGNIRzvRdmaPtr_t). CMK_NOCOPY_DIRECT_BYTES is used in
 * ckrdma.h to reserve bytes for source or destination metadata info.           */
#define DUMB_STATIC_ASSERT(test) typedef char sizeCheckAssertion[(!!(test))*2-1]

/* Check the value of CMK_NOCOPY_DIRECT_BYTES if the compiler reports an
 * error with the message "the size of an array must be greater than zero" */
DUMB_STATIC_ASSERT(sizeof(CmiGNIRzvRdmaPtr_t) == CMK_NOCOPY_DIRECT_BYTES);

/* Machine specific metadata that stores all information required for a GET/PUT operation
 * This has three use-cases:
 *  - Unaligned GET, which uses PUT underneath
 *  - REG/PREREG mode GET in SMP mode, which requires worker thread to send this structure
 *    to comm thread and comm thread then performs the GET operation
 *  - REG/PREREG mode PUT in SMP mode, which requires worker thread to send this structure
 *  - to comm thread and comm thread then performs the PUT operation
 */
typedef struct _cmi_gni_rzv_rdma_direct_info {
  gni_mem_handle_t dest_mem_hndl;
  uint64_t dest_addr;
  gni_mem_handle_t src_mem_hndl;
  uint64_t src_addr;
  int destPe;
  int size;
  uint64_t ref;
} CmiGNIRzvRdmaDirectInfo_t;

/* Machine specific metadata information required to register a buffer and perform
 * an RDMA operation with a remote buffer. This metadata information is used to perform
 * registration and a PUT operation when the remote buffer wants to perform a GET with an
 * unregistered buffer. Similary, the metadata information is used to perform registration
 * and a GET operation when the remote buffer wants to perform a PUT with an unregistered
 * buffer.*/
typedef struct _cmi_gni_rzv_rdma_reverse_op {
  const void *destAddr;
  int destPe;
  int destRegMode;
  const void *srcAddr;
  int srcPe;
  int srcRegMode;

  gni_mem_handle_t rem_mem_hndl;
  int ackSize;
  int size;
} CmiGNIRzvRdmaReverseOp_t;

// Set the machine specific information for a nocopy pointer
void LrtsSetRdmaBufferInfo(void *info, const void *ptr, int size, unsigned short int mode);

// Perform an RDMA Get call into the local destination address from the remote source address
void LrtsIssueRget(NcpyOperationInfo *ncpyOpInfo);

// Perform an RDMA Put call into the remote destination address from the local source address
void LrtsIssueRput(NcpyOperationInfo *ncpyOpInfo);

// Method performs RDMA operations
gni_return_t post_rdma(
  uint64_t remote_addr,
  gni_mem_handle_t remote_mem_hndl,
  uint64_t local_addr,
  gni_mem_handle_t local_mem_hndl,
  int length,
  uint64_t post_id,
  int destNode,
  int type,
  unsigned short int mode);

// Register memory and return mem_hndl
gni_mem_handle_t registerDirectMem(const void *ptr, int size, unsigned short int mode);

// Deregister local memory handle
void deregisterDirectMem(gni_mem_handle_t mem_hndl, int pe);

// Method invoked to deregister memory handle
void LrtsDeregisterMem(const void *ptr, void *info, int pe, unsigned short int mode);

#if CMK_SMP
// Method used by the comm thread to perform GET
void _performOneRgetForWorkerThread(MSG_LIST *ptr);

// Method used by the comm thread to perform PUT
void _performOneRputForWorkerThread(MSG_LIST *ptr);
#endif

#endif /* end if for MACHINE_ONESIDED_H_ */

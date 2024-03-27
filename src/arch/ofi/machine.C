/** @file
 * OFI LRTS machine layer
 *
 * Copyright (c) 2017, Intel Corporation. All rights reserved.
 * See LICENSE in this directory.
 *
 * Authors: Yohann Burette <yohann.burette@intel.com>
 *          Mikhail Shiryaev <mikhail.shiryaev@intel.com>
 *          Marat Shamshetdinov <marat.shamshetdinov@intel.com>
 * Date:    2017-06-23
 *
 * 10,000ft view:
 *  - Each Charm++ node opens an OFI RDM endpoint
 *  - For small (enough) messages, the sender sends the data directly
 *  - For long messages,
 *      1) the sender sends a OFIRmaHeader describing the data,
 *      2) the receiver retrieves the data with RMA Read,
 *      3) once done, the receiver sends an OFIRmaAck back.
 *  - The tag associated with each communication helps the receiver
 *    parse the data (i.e. short, long or ack).
 *  - The receiver uses a OFILongMsg structure to keep track of an
 *    ongoing long message retrieval.
 *
 * Changes For CXI (as found on Slingshot-11):
 * Date : 2024-01-04
 * Author: Eric Bohm
 *
 * * Add support for CXI extensions for Cassini (AKA Slingshot-11)
 *
 * - CXI required FI_MR_ENDPOINT
 *
 *  1) Which requires that all message memory be: registered, bound to
 *     the endpoint, and activated before use.
 *
 *  2) CXI supporting endpoint must be selected for in fi_getinfo
 *
 *  3) CXI is not optimized for within node communication, so process
 *  to process schemes, i.e., XPMEM or CMA are on by default and we
 *  would not expect disabling them to be functional, let alone
 *  optimal.
 *
 *  4) Memory requirements add tracking for the memory registration
 *  key. This is kept in a prefix header for each allocated buffer.
 *  Most use cases are managed by the memory pool, which is also on by
 *  default and should not be disabled without good reason.
 *
 *  5) CXI comes with FI_MR_VIRT_ADDR=0, which means RMA transactions
 *  require both the key and the offset from the base address of the
 *  allocated buffer associated with that key.
 *
 *  6) We update to the build time environment version of libfabric
 *  instead of forcing 1.0.  (e.g. libfabric 1.15.2.0 at time of writing)
 *
 *  7) CXI defines that memory keys 0-99 support CXI optimized
 *  operations (such as reductions, or reducing depency on delivery
 *  ordering ).  We set aside 0-50 for TBD use and build up from 51.
 *
 * Runtime options:
 *  +ofi_eager_maxsize: (default: 65536) Threshold between buffered and RMA
 *                      paths.
 *  +ofi_cq_entries_count: (default: 8) Maximum number of entries to read from
 *                         the completion queue.
 *  +ofi_use_inject: (default: 1) Whether use buffered send.
 *  +ofi_num_recvs: (default: 8) Number of pre-posted receive buffers.
 *  +ofi_runtime_tcp: (default: off) During the initialization phase, the
 *                    OFI EP names need to be exchanged among all nodes. By
 *                    default, the exchange is done with both PMI and OFI. If
 *                    this flag is set then the exchange is done with PMI only.
 *
 *  Memory pool specific options:
 *  +ofi_mempool_init_size_mb: (default: 8) The initial size of memory pool in MBytes.
 *  +ofi_mempool_expand_size_mb: (default: 4) The size of expanding chunk in MBytes.
 *  +ofi_mempool_max_size_mb: (default: 512) The limit for total size
 *                            of memory pool in MBytes.
 *  +ofi_mempool_lb_size: (default: 1024) The left border size in bytes
 *                        from which the memory pool is used.
 *  +ofi_mempool_rb_size: (default: 67108864) The right border size in bytes
 *                        to which the memory pool is used.
 * @ingroup Machine
 */
/*@{*/

#include <stdio.h>
#include <errno.h>
#include "converse.h"
#include "cmirdmautils.h"
#include <algorithm>

/*Support for ++debug: */
#include <unistd.h> /*For getpid()*/
#include <stdlib.h> /*For sleep()*/

#include "machine.h"

// Trace communication thread

#if CMK_TRACE_ENABLED && CMK_SMP_TRACE_COMMTHREAD
#define TRACE_THRESHOLD     0.00001
#undef CMI_MACH_TRACE_USEREVENTS
#define CMI_MACH_TRACE_USEREVENTS 1
#else
#undef CMK_SMP_TRACE_COMMTHREAD
#define CMK_SMP_TRACE_COMMTHREAD 0
#endif

#define CMK_TRACE_COMMOVERHEAD 0
#if CMK_TRACE_ENABLED && CMK_TRACE_COMMOVERHEAD
#undef CMI_MACH_TRACE_USEREVENTS
#define CMI_MACH_TRACE_USEREVENTS 1
#else
#undef CMK_TRACE_COMMOVERHEAD
#define CMK_TRACE_COMMOVERHEAD 0
#endif

#if CMI_MACH_TRACE_USEREVENTS && CMK_TRACE_ENABLED
CpvStaticDeclare(double, projTraceStart);
#define  START_EVENT()  CpvAccess(projTraceStart) = CmiWallTimer();
#define  END_EVENT(x)   traceUserBracketEvent(x, CpvAccess(projTraceStart), CmiWallTimer());
#define  EVENT_TIME()   CpvAccess(projTraceStart)
#else
#define  START_EVENT()
#define  END_EVENT(x)
#define  EVENT_TIME()   (0.0)
#endif


/* TODO: macros regarding redefining locks that will affect pcqueue.h*/
#include "pcqueue.h"

/* =======Beginning of Definitions of Performance-Specific Macros =======*/
/* TODO: add any that are related */

/* ======= This where we define the macros for the 0-99 special MRs ====== */

#define OFI_POSTED_RECV_MR_KEY 0
#define CMK_SMP_SENDQ 1
/* =======End of Definitions of Performance-Specific Macros =======*/


/* =====Beginning of Definitions of Message-Corruption Related Macros=====*/
/* TODO: add any that are related */
/* =====End of Definitions of Message-Corruption Related Macros=====*/


/* =====Beginning of Declarations of Machine Specific Variables===== */


/* =====End of Declarations of Machine Specific Variables===== */

#include "machine-lrts.h"

#include "machine-common-core.C"

/* Libfabric headers */
#include <rdma/fabric.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_errno.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_tagged.h>
#include <rdma/fi_rma.h>

#define USE_OFIREQUEST_CACHE 1

/* Definition of OFIRequest + request cache */
#include "request.h"

/* Runtime to exchange EP addresses during LrtsInit() */
/* someday, we'll update to pmix, today is not that day */
#define CMK_USE_CRAYPMI2 1
#if CMK_USE_CRAYPMI2
#include "runtime-craypmi2.C"
#elif CMK_USE_CRAYPMI
#include "runtime-craypmi.C"
#elif CMK_USE_PMI || CMK_USE_SIMPLEPMI
#include "runtime-pmi.C"
#elif CMK_USE_PMI2
#include "runtime-pmi2.C"
#elif CMK_USE_PMIX
#include "runtime-pmix.C"
#endif

#if CMK_CXI
  /** use mempools in CXI to aggregate FI_MR_ENDPOINT registration reqs into big blocks */
#define oneMB (1024ll*1024)
#define oneGB (1024ll*1024*1024)
#define ALIGN64(x)       (size_t)((~63)&((x)+63))
#define ALIGNHUGEPAGE(x)   (size_t)((~(_tlbpagesize-1))&((x)+_tlbpagesize-1))

#define USE_MEMPOOL 1
#define LARGEPAGE 0
#else
#define USE_MEMPOOL 0
#endif

static int _tlbpagesize = 4096;
#if USE_MEMPOOL
#if LARGEPAGE
// separate pool of memory mapped huge pages
static CmiInt8 BIG_MSG  =  16*oneMB;
static CmiInt8 ONE_SEG  =  4*oneMB;
#else
static CmiInt8 BIG_MSG  =  8*oneMB;
static CmiInt8 ONE_SEG  =  2*oneMB;
#endif


#if USE_MEMPOOL

void* LrtsPoolAlloc(int n_bytes);

#include "mempool.h"
#define MEMPOOL_INIT_SIZE_MB_DEFAULT   4
#define MEMPOOL_EXPAND_SIZE_MB_DEFAULT 20
#define MEMPOOL_MAX_SIZE_MB_DEFAULT    32
#define MEMPOOL_LB_DEFAULT             0
#define MEMPOOL_RB_DEFAULT             134217728
#define ONE_MB                         1048576
#define ALIGNBUF (sizeof(mempool_header)+sizeof(CmiChunkHeader))
#define   GetMempoolBlockPtr(x)   MEMPOOL_GetBlockPtr(MEMPOOL_GetMempoolHeader(x,ALIGNBUF))
#define   GetMempoolPtr(x)        MEMPOOL_GetMempoolPtr(MEMPOOL_GetMempoolHeader(x,ALIGNBUF))

#define   GetMempoolsize(x)       MEMPOOL_GetSize(MEMPOOL_GetMempoolHeader(x,ALIGNBUF))
#define   GetMemHndl(x)           MEMPOOL_GetMemHndl(MEMPOOL_GetMempoolHeader(x,ALIGNBUF))

#define   GetMemHndlFromBlockHeader(x) MEMPOOL_GetBlockMemHndl(x)
#define   GetSizeFromBlockHeader(x)    MEMPOOL_GetBlockSize(x)
#define   GetBaseAllocPtr(x) GetMempoolBlockPtr(x)
#define   GetMemOffsetFromBase(x) ((char*)(x) - (char *) GetBaseAllocPtr(x))



CpvDeclare(mempool_type*, mempool);

#endif /* USE_MEMPOOL */

#define CmiSetMsgSize(msg, sz)  ((((CmiMsgHeaderBasic *)msg)->size) = (sz))
#define CmiGetMsgSize(msg)  ((((CmiMsgHeaderBasic *)msg)->size))

#define CACHELINE_LEN 64

#define OFI_NUM_RECV_REQS_DEFAULT    8
#define OFI_NUM_RECV_REQS_MAX        4096

#define OFI_EAGER_MAXSIZE_DEFAULT    65536
#define OFI_EAGER_MAXSIZE_MAX        1048576

#define OFI_CQ_ENTRIES_COUNT_DEFAULT 8
#define OFI_CQ_ENTRIES_COUNT_MAX     1024

#define OFI_USE_INJECT_DEFAULT       1

#define OFI_KEY_FORMAT_EPNAME "ofi-epname-%i"

#define OFI_OP_SHORT 0x1ULL
#define OFI_OP_LONG  0x2ULL
#define OFI_OP_ACK   0x3ULL
#define OFI_RDMA_DIRECT_REG_AND_PUT 0x4ULL
#define OFI_RDMA_DIRECT_REG_AND_GET 0x5ULL

#define OFI_RDMA_DIRECT_DEREG_AND_ACK 0x6ULL

#define OFI_OP_NAMES 0x8ULL

#define OFI_READ_OP 1
#define OFI_WRITE_OP 2

#define OFI_OP_MASK  0x7ULL

#define MR_ACCESS_PERMISSIONS (FI_REMOTE_READ | FI_READ | FI_RECV | FI_SEND | FI_REMOTE_WRITE | FI_WRITE)

static inline int process_completion_queue();

#define ALIGNED_ALLOC(ptr, size)                                        \
  do {                                                                  \
      int pm_ret = posix_memalign((void**)(&ptr), CACHELINE_LEN, size); \
      if (CMI_UNLIKELY((pm_ret != 0) || !ptr))                          \
      {                                                                 \
          CmiAbort("posix_memalign: ret %d", pm_ret);                   \
      }                                                                 \
  } while (0)

#define OFI_RETRY(func)                                 \
    do {                                                \
        intmax_t _ret;                                  \
        do {                                            \
            _ret = func;                                \
            if (CMI_LIKELY(_ret == 0)) break;           \
            if (_ret != -FI_EAGAIN) {                   \
                CmiAbort("OFI_RETRY: ret %jd\n", _ret); \
            }                                           \
            process_completion_queue();                 \
        } while (_ret == -FI_EAGAIN);                   \
    } while (0)

/* OFI_INFO is used to print information messages during LrtsInit() */
#define OFI_INFO(...) \
    if (*myNodeID == 0) CmiPrintf("Charm++>ofi> " __VA_ARGS__)

#define PRINT_THREAD_INFO(message)              \
    MACHSTATE5(2,"thread info: process_idx=%i " \
              "local_thread_idx=%i "            \
              "global_thread_idx=%i "           \
              "local_worker_thread_count=%i "   \
              "is_comm_thread=%i\n",            \
              CmiMyNode(),                      \
              CmiMyRank(),                      \
              CmiMyPe(),                        \
              CmiMyNodeSize(),                  \
              CmiInCommThread());

/**
 * OFI RMA Header
 * Message sent by sender to receiver during RMA Read of long messages.
 *  - nodeNo: Target node number
 *  - src_msg: Address or offset from registered source address
 *  - len: Length of message
 *  - key: Remote key
 *  - mr: Address of memory region; Sent back as part of OFIRmaAck
 *  - orig_msg: actual address of source message; Sent back as part of OFIRmaAck
 */
typedef struct OFIRmaHeader {
    uint64_t src_msg;
    uint64_t len;
    uint64_t key;
    uint64_t mr;
    uint64_t orig_msg;
    int      nodeNo;
} OFIRmaHeader;

/**
 * OFI RMA Ack
 * Message sent by receiver to sender during RMA Read of long messages.
 *  - src_msg: Address of source msg; Received as part of OFIRmaHeader
 *  - mr: Address of memory region; Received as part of OFIRmaHeader
 */
typedef struct OFIRmaAck {
    uint64_t src_msg;
    uint64_t mr;
} OFIRmaAck;

/**
 * OFI Long Message
 * Structure stored by the receiver about ongoing RMA Read of long message.
 *  - asm_msg: Assembly buffer where the data is RMA Read into
 *  - nodeNo: Target node number
 *  - rma_ack: OFI Rma Ack sent to sender once all the data has been RMA Read
 *  - completion_count: Number of expected RMA Read completions
 *  - mr: Memory Region where the data is RMA Read into
 */
typedef struct OFILongMsg {
    char                *asm_msg;
    int                 nodeNo;
    struct OFIRmaAck    rma_ack;
    size_t              completion_count;
    struct fid_mr       *mr;
} OFILongMsg;

/**
 * TO_OFI_REQ is used retrieve the request associated with a given fi_context.
 */
#define TO_OFI_REQ(_ptr_context) \
    container_of((_ptr_context), OFIRequest, context)

typedef struct OFIContext {
    /** Endpoint to communicate on */
    struct fid_ep *ep;

    /** Completion queue handle */
    struct fid_cq *cq;

    /**
     * Maximum size for eager messages.
     * RMA Read for larger messages.
     */
    size_t eager_maxsize;

    /**
     * Maximum inject size.
     */
    size_t inject_maxsize;

    /**
     * Maximum number of completion queue entries that
     * can be retrieved by each fi_cq_read() call.
     */
    size_t cq_entries_count;

    /**
     * Whether to use buffered send (aka inject)
     */
    int use_inject;

#if USE_OFIREQUEST_CACHE
    /** OFIRequest allocator */
    request_cache_t *request_cache;
#endif

#if CMK_SMP && CMK_SMP_SENDQ
    /**
     * Producer/Consumer Queue used in CMK_SMP mode:
     *  - worker thread pushes messages to the queue
     *  - comm thread pops and sends
     * Locking is already done by PCQueue.
     */
    PCQueue send_queue;
#endif

    /** Fabric Domain handle */
    struct fid_fabric *fabric;

    /** Access Domain handle */
    struct fid_domain *domain;

    /** Address vector handle */
    struct fid_av *av;

    /**
     * Maximum size for RMA operations.
     * Multiple RMA operations for larger messages.
     */
    size_t rma_maxsize;

    /**
     * MR mode:
     *  - FI_MR_SCALABLE allows us to register all the memory with our own key,
     *  - FI_MR_BASIC requires us to register the RMA buffers and to exchange the keys.
     *  - FI_MR_ENDPOINT requires us to register and bind and enable our MRs, but we can use our own 32 bit keys locally.
     */
#if CMK_CXI
    uint32_t mr_mode;
#else
    enum fi_mr_mode mr_mode;
#endif

#if CMK_CXI
  /** Used as unique key value in FI_MR_ENDPOINT mode */
  // only 32 bits available to us
  uint32_t mr_counter;
#else
  /** Used as unique key value in FI_MR_SCALABLE mode */
  uint64_t mr_counter;
#endif
    int num_recv_reqs;

    /** Pre-posted receive requests */
    OFIRequest **recv_reqs;

#if USE_MEMPOOL
    size_t mempool_init_size;
    size_t mempool_expand_size;
    long long mempool_max_size;
    size_t mempool_lb_size;
    size_t mempool_rb_size;
#endif
} OFIContext __attribute__ ((aligned (CACHELINE_LEN)));

static void recv_callback(struct fi_cq_tagged_entry *e, OFIRequest *req);
static int fill_av(int myid, int nnodes, struct fid_ep *ep,
                   struct fid_av *av, struct fid_cq *cq);
static int fill_av_ofi(int myid, int nnodes, struct fid_ep *ep,
                       struct fid_av *av, struct fid_cq *cq);
#if CMK_CXI
static int ofi_reg_bind_enable(const void *buf,
			       size_t len, struct fid_mr **mr, OFIContext *context);
#endif



static OFIContext context;
#if LARGEPAGE

/* directly mmap memory from hugetlbfs for large pages */

#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>

#ifdef __cplusplus
extern "C" {
#endif
#include <hugetlbfs.h>
#ifdef __cplusplus
}
#endif
/** copied from the GNI layer */
// size must be _tlbpagesize aligned
void *my_get_huge_pages(size_t size)
{
    char filename[512];
    int fd;
    mode_t mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
    void *ptr = NULL;

    snprintf(filename, sizeof(filename), "%s/charm_mempool.%d.%d", hugetlbfs_find_path_for_size(_tlbpagesize), getpid(), rand());
    fd = open(filename, O_RDWR | O_CREAT, mode);
    if (fd == -1) {
        CmiAbort("my_get_huge_pages: open filed");
    }
    ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    if (ptr == MAP_FAILED) ptr = NULL;
//printf("[%d] my_get_huge_pages: %s %d %p\n", myrank, filename, size, ptr);
    close(fd);
    unlink(filename);
    return ptr;
}

void my_free_huge_pages(void *ptr, int size)
{
//printf("[%d] my_free_huge_pages: %p %d\n", myrank, ptr, size);
    int ret = munmap(ptr, size);
    if (ret == -1) CmiAbort("munmap failed in my_free_huge_pages");
}

#endif

#include "machine-rdma.h"
#if CMK_ONESIDED_IMPL
#include "machine-onesided.h"
#endif


/* transformed from cpuaffinity.C due to our need to parse the same
 sort of arg string, but having to do so before CmiNumPesGlobal (and
 similar quantities) have been defined
*/
static int search_map(char *mapstring, int pe)
{
  int NumPesGlobal;
  PMI_Get_universe_size(&NumPesGlobal);
  int *map = (int *)malloc(NumPesGlobal*sizeof(int));
  char *ptr = NULL;
  int h, i, j, k, count;
  int plusarr[128];
  char *str;

  char *mapstr = (char*)malloc(strlen(mapstring)+1);
  strcpy(mapstr, mapstring);

  str = strtok_r(mapstr, ",", &ptr);
  count = 0;
  while (str && count < NumPesGlobal)
  {
      int hasdash=0, hascolon=0, hasdot=0, hasstar1=0, hasstar2=0, numplus=0;
      int start, end, stride=1, block=1;
      int iter=1;
      plusarr[0] = 0;
      for (i=0; i<strlen(str); i++) {
          if (str[i] == '-' && i!=0) hasdash=1;
          else if (str[i] == ':') hascolon=1;
	  else if (str[i] == '.') hasdot=1;
	  else if (str[i] == 'x') hasstar1=1;
	  else if (str[i] == 'X') hasstar2=1;
	  else if (str[i] == '+') {
            if (str[i+1] == '+' || str[i+1] == '-') {
              printf("Warning: Check the format of \"%s\".\n", str);
            } else if (sscanf(&str[i], "+%d", &plusarr[++numplus]) != 1) {
              printf("Warning: Check the format of \"%s\".\n", str);
              --numplus;
            }
          }
      }
      if (hasstar1 || hasstar2) {
          if (hasstar1) sscanf(str, "%dx", &iter);
          if (hasstar2) sscanf(str, "%dX", &iter);
          while (*str!='x' && *str!='X') str++;
          str++;
      }
      if (hasdash) {
          if (hascolon) {
            if (hasdot) {
              if (sscanf(str, "%d-%d:%d.%d", &start, &end, &stride, &block) != 4)
                 printf("Warning: Check the format of \"%s\".\n", str);
            }
            else {
              if (sscanf(str, "%d-%d:%d", &start, &end, &stride) != 3)
                 printf("Warning: Check the format of \"%s\".\n", str);
            }
          }
          else {
            if (sscanf(str, "%d-%d", &start, &end) != 2)
                 printf("Warning: Check the format of \"%s\".\n", str);
          }
      }
      else {
          sscanf(str, "%d", &start);
          end = start;
      }
      if (block > stride) {
        printf("Warning: invalid block size in \"%s\" ignored.\n", str);
        block=1;
      }
      //if (CmiMyPe() == 0) printf("iter: %d start: %d end: %d stride: %d, block: %d. plus %d \n", iter, start, end, stride, block, numplus);
      for (k = 0; k<iter; k++) {
        for (i = start; i<=end; i+=stride) {
          for (j=0; j<block; j++) {
            if (i+j>end) break;
            for (h=0; h<=numplus; h++) {
              map[count++] = i+j+plusarr[h];
              if (count == NumPesGlobal) break;
            }
            if (count == NumPesGlobal) break;
          }
          if (count == NumPesGlobal) break;
        }
        if (count == NumPesGlobal) break;
      }
      str = strtok_r(NULL, ",", &ptr);
  }
  i = map[pe % count];

  free(map);
  free(mapstr);
  return i;
}

/* ### Beginning of Machine-startup Related Functions ### */
void LrtsInit(int *argc, char ***argv, int *numNodes, int *myNodeID)
{
  struct fi_info        *providers;
  struct fi_info        *prov;
  struct fi_info        *hints;
  struct fi_domain_attr domain_attr = {0};
  struct fi_tx_attr     tx_attr = { 0 };
  struct fi_cq_attr     cq_attr = { 0 };
  struct fi_av_attr     av_attr = { (enum fi_av_type)0 };
  int                   fi_version;
  size_t                max_header_size;

  int i;
  int ret;

  /**
   * Initialize our runtime environment -- e.g. PMI.
   */
  ret = runtime_init(myNodeID, numNodes);
  //  CmiPrintf("[%d] nodeid %d, numnodes %d\n", *myNodeID, *myNodeID, *numNodes);
  if (ret) {
    CmiAbort("OFI::LrtsInit::runtime_init failed");
  }
  /*
	int namelength;
	PMI_KVS_Get_name_length_max(&namelength);
	char *name1=(char *) malloc(namelength+1);
	char *name2=(char *) malloc(namelength+1);
	PMI_KVS_Get_my_name(name1, namelength);
	CmiPrintf("[%d] PMI keyspace %s\n", *myNodeID, PMI);
  */
  /**
   * Hints to filter providers
   * See man fi_getinfo for a list of all filters
   * mode: This OFI machine will pass in context into communication calls
   * ep_type: Reliable datagram operation
   * resource_mgmt: Let the provider manage the resources
   * caps: Capabilities required from the provider. We want to use the
   *       tagged message queue and rma read APIs.
   */
  hints = fi_allocinfo();
  CmiAssert(NULL != hints);
  hints->mode = ~0;
  hints->domain_attr->mode = ~0;
#if CMK_CXI
  hints->domain_attr->mr_mode          = FI_MR_ENDPOINT;
#endif
  hints->mode                          = FI_CONTEXT;
  hints->ep_attr->type                 = FI_EP_RDM;
#if CMK_CXI
  hints->ep_attr->protocol             = FI_PROTO_CXI;
  hints->domain_attr->threading = FI_THREAD_SAFE;
  //hints->domain_attr->threading = FI_THREAD_DOMAIN;
  hints->domain_attr->control_progress = FI_PROGRESS_MANUAL;
  hints->domain_attr->data_progress = FI_PROGRESS_MANUAL;
  hints->domain_attr->auth_key          =NULL;
  //     hints->ep_attr->type                 = FI_EP_MSG;
#endif
  hints->domain_attr->resource_mgmt    = FI_RM_ENABLED;
  hints->caps                          = FI_TAGGED;
  hints->caps                         |= FI_RMA;
  hints->caps                         |= FI_REMOTE_READ;
#if CMK_CXI
  // Figure out which NIC we should request based on the one that
  // should be closest.
       /* This is overly complicated for several reasons:

	* 1. The hardware itself is not built to have the numerical ID
	* ordering of different types of hardware correlate with
	* proximity at all.  E.g., on frontier core 0 is in NUMA 0 which
	* means it is closest to GPU 4 and HSN (NIC) 2, but is a direct
	* peer of cores 0-15.  So, proximal ordering outside of type
	* should not be considered predictive of proximity.  That
	* relationship has to be detected by other means.

	* 2. HWLOC doesn't have a hwloc_get_closest_nic because... NIC
	* doesn't even rate an object type in their ontology, let
	* alone get first class treatment.  Given that PCI devices
	* don't have a cpuset, there are a bunch of HWLOC features
	* that don't work for them.  But it is the portable hardware
	* interrogation API we have to hand.  So, instead we get our
	* NUMAnode, and then get the PCI objects inside it. Get the
	* (Ethernet)->Net(Slingshot) object and take the name from it,
	* (e.g., hsn2). Get the last digit and append it to "cxi".
	* There may be a better way to do this, but it isn't apparent
	* to me based on their documentation.

	* 2a. How one actually extracts that information from HWLOC is
	* difficult to unravel.  As it somehow accessible to their
	* lstopo utility, but from within their C API the PCI devices
	* do *not* have such convenient labeling as something special
	* needs to happen to get their linuxfs utilities to inject
	* that derived information into your topology object.  As an
	* interim solution we allow the user to map their cxi[0..3]
	* selection using command line arguments.

	* 2b. Likewise the 1:1 relationship we assume here between
	* cxi[0..3] and hsn[0..3] is informed speculation backed up by
	* no documentation.  Because, why have cxi0..3 at all if they
	* don't correlate with the underlying hsn0..3?  We assume the
	* designers aren't insane or malicious, just stuck on the other
	* side of an NDA.

	* 3. LrtsInit is of necessity fairly early in the startup
	* process, so a lot of the infrastructure we might otherwise rely
	* upon hasn't been set up yet.  But, we do have the hwloc
	* topology and cray-pmi.

	* 4. We might not (depending on what does the binding) have
	* bound this process yet, so exactly where we are and how
	* close that is to any particular NIC is sort of fluid.

	* 5. How many CXI domain interfaces exist?  You can't tell on
	* the head node, the answer could easily be zero there.  You
	* also can't be sure that whatever was true at compile time
	* will be true at run time.  Crusher and Frontier have four.
	* Delta has one.  Perlmutter has four on GPU nodes and one on
	* CPU nodes.  The user could easily be confused, so we can't
	* rely on them telling us.  This has to be determined at
	* run time.
	*/

  char *cximap=NULL;
  CmiGetArgStringDesc(*argv, "+cximap", &cximap, "define cxi interface to process mapping");
#endif
  /**
   * FI_VERSION provides binary backward and forward compatibility support
   * Specify the version of OFI this machine is coded to, the provider will
   * select struct layouts that are compatible with this version.
   */
  //    fi_version = FI_VERSION(1, 15);
  // CXI versions itself differently from OFI

#if CMK_CXI
  /* CXI has its own versioning, so just use whatever the build env
     is until we come up with some CXI version specific changes */
  fi_version = FI_VERSION(FI_MAJOR_VERSION, FI_MINOR_VERSION);
#else
  fi_version = FI_VERSION(1, 0);
#endif
  ret = fi_getinfo(fi_version, NULL, NULL, 0ULL, hints, &providers);

  if (ret < 0) {
    CmiAbort("OFI::LrtsInit::fi_getinfo error");
  }

  if (providers == NULL) {
    CmiAbort("OFI::LrtsInit::No provider found");
  }

#if CMK_CXI
  char myDomainName[5]="cxi0";
  char priorDomain[5]="null";
  short numcxi=0;
  for(fi_info *aprov = providers; aprov!=NULL; aprov=aprov->next)
    { // count up the CXI interfaces
      if(strncmp(aprov->domain_attr->name,myDomainName,3)==0 && strncmp(aprov->domain_attr->name,priorDomain,4)!=0)
      {
	numcxi++;
	strncpy(priorDomain,aprov->domain_attr->name,4);
      }
    }
  short myNet;
  int numPesOnNode;
#define HAS_PMI_Get_numpes_in_app_on_smp 1
#if HAS_PMI_Get_numpes_in_app_on_smp
  PMI_Get_numpes_in_app_on_smp(&numPesOnNode);
#else
  // how do we learn how many processes there are on this node?
#endif
  int myRank=*myNodeID%numPesOnNode;
  if(cximap != NULL)
    {
      myNet=search_map(cximap,myRank);
      //      CmiPrintf("map sets process %d to rank %d to cxi%d\n",*myNodeID, myRank, myNet);
    }
  else
    {
      int quad= (numPesOnNode>=numcxi) ? numcxi : numPesOnNode;
      //      CmiPrintf("[%d] divnumPesOnNode %d numcxi %d quad %d\n",myRank, numPesOnNode, numcxi, quad);
      // determine where we fall in the ordering
      // Default OS id order on frontier
      /* 0-15  -> HSN-2
       * 16-31 -> HSN-1
       * 32-47 -> HSN-3
       * 48-63 -> HSN-0
       but experimentally, the best order seems to be 1302
       */


      ///      short hsnOrder[numcxi]={2,1,3,0};
      if(numcxi==4)
	{
	  short hsnOrder[4]= {1,3,0,2};
	  if(myRank%quad>numcxi)
	    {
	      CmiPrintf("Error: myrank %d quad %d myrank/quad %n",myRank,quad, myRank/quad);
	      CmiAbort("cxi mapping failure");
	    }
	  myNet=hsnOrder[myRank%quad];
	}
      else
	{
	  CmiAssert(numcxi==1);
	  //theoretically there are cases other than 4 and 1, but
	  //until someone sights such an incrayptid on a machine floor,
	  //we're just going to assume they don't exist.
	  myNet=0;
	}
    }
  snprintf(myDomainName,5, "cxi%d", myNet);

  for(fi_info *aprov = providers; aprov!=NULL; aprov=aprov->next)
    {
      // if we're running multiple processes per node, we should
      // choose the CXI interface closest to our placement.  This is
      // a little awkward as we're at an information low moment
      // early in the bootstrapping process.
      //OFI_INFO("aprovider: %s domain %s\n", aprov->fabric_attr->prov_name, aprov->domain_attr->name);
      if(strncmp(aprov->domain_attr->name,myDomainName,4)==0)
	{
	  prov = aprov;
#if OFI_VERBOSE_STARTUP
	  if(*myNodeID<=numPesOnNode)
	    CmiPrintf("Process [%d] will use domain %s\n", *myNodeID, myDomainName);
#else
	  //assume a manual map wants confirming output
	  if(cximap != NULL)
	    CmiPrintf("Process [%d] will use domain %s\n", *myNodeID, myDomainName);
#endif
	}
    }
#endif

  context.inject_maxsize = prov->tx_attr->inject_size;
  context.eager_maxsize = OFI_EAGER_MAXSIZE_DEFAULT;
  CmiGetArgInt(*argv, "+ofi_eager_maxsize", (int*)&context.eager_maxsize);
  if (context.eager_maxsize > prov->ep_attr->max_msg_size)
    CmiAbort("OFI::LrtsInit::Eager max size > max msg size.");
  if (context.eager_maxsize > OFI_EAGER_MAXSIZE_MAX || context.eager_maxsize < 0)
    CmiAbort("OFI::LrtsInit::Eager max size range error.");
  max_header_size = (sizeof(OFIRmaHeader) >= sizeof(OFIRmaAck)) ? sizeof(OFIRmaHeader) : sizeof(OFIRmaAck);
  if (context.eager_maxsize < max_header_size)
    CmiAbort("OFI::LrtsInit::Eager max size too small to fit headers.");
  context.cq_entries_count = OFI_CQ_ENTRIES_COUNT_DEFAULT;
  CmiGetArgInt(*argv, "+ofi_cq_entries_count", (int*)&context.cq_entries_count);
  if (context.cq_entries_count > OFI_CQ_ENTRIES_COUNT_MAX || context.cq_entries_count <= 0)
    CmiAbort("OFI::LrtsInit::Cq entries count range error");
  context.use_inject = OFI_USE_INJECT_DEFAULT;
  CmiGetArgInt(*argv, "+ofi_use_inject", &context.use_inject);
  if (context.use_inject < 0)
    CmiAbort("OFI::LrtsInit::Use inject value error");
  context.rma_maxsize = prov->ep_attr->max_msg_size;
#if CMK_CXI
  context.mr_mode = prov->domain_attr->mr_mode;
#else
  // the old code path uses the defunct enum
  context.mr_mode = static_cast<fi_mr_mode>(prov->domain_attr->mr_mode);
#endif

#define OFI_VERBOSE_STARTUP 0
#if OFI_VERBOSE_STARTUP
  OFI_INFO("[%d]provider: %s\n", *myNodeID, prov->fabric_attr->prov_name);
  OFI_INFO("[%d]domain: %s\n", *myNodeID, prov->domain_attr->name);
  OFI_INFO("control progress: %d\n", prov->domain_attr->control_progress);
  OFI_INFO("data progress: %d\n", prov->domain_attr->data_progress);
  OFI_INFO("maximum inject message size: %ld\n", context.inject_maxsize);
  OFI_INFO("eager maximum message size: %ld (maximum header size: %ld)\n",
	   context.eager_maxsize, max_header_size);
  OFI_INFO("cq entries count: %ld\n", context.cq_entries_count);
  OFI_INFO("use inject: %d\n", context.use_inject);

#if CMK_CXI
  OFI_INFO("requested mr mode: 0x%x\n", FI_MR_ENDPOINT);
  OFI_INFO("requested mr mode & mr_mode: 0x%x\n", (FI_MR_ENDPOINT) & context.mr_mode);
#endif
  // start at 51 for the normal stuff, like pool messages
  context.mr_counter = 51;
  OFI_INFO("maximum rma size: %ld\n", context.rma_maxsize);
  OFI_INFO("mr mode: 0x%x\n", context.mr_mode);

  OFI_INFO("mr virtual address support : 0x%x\n", context.mr_mode & FI_MR_VIRT_ADDR);
    OFI_INFO("use memory pool: %d\n", USE_MEMPOOL);
#endif //verbose

#if CMK_CXI
  if ((context.mr_mode & FI_MR_ENDPOINT)==0)
    CmiAbort("OFI::LrtsInit::Unsupported MR mode FI_MR_ENDPOINT");
#else
  /* keeping this for now, should debug this on a non-cray and make
     sure we get a basic OFI working there without these defunct MR
     modes.  Currently, we don't actually care about non CXI OFI,
     but it could be good on AWS EFA and potentially good on future
     platforms where there is an optimal provider for the underlying
     hardware. */
  if ((context.mr_mode != FI_MR_BASIC) &&
      (context.mr_mode != FI_MR_SCALABLE)) {
    CmiAbort("OFI::LrtsInit::Unsupported MR mode");
  }
#endif


#if USE_MEMPOOL
  size_t mempool_init_size_mb = MEMPOOL_INIT_SIZE_MB_DEFAULT;
  CmiGetArgInt(*argv, "+ofi_mempool_init_size_mb", (int*)&mempool_init_size_mb);
  context.mempool_init_size = mempool_init_size_mb * ONE_MB;

  size_t mempool_expand_size_mb = MEMPOOL_EXPAND_SIZE_MB_DEFAULT;
  CmiGetArgInt(*argv, "+ofi_mempool_expand_size_mb", (int*)&mempool_expand_size_mb);
  context.mempool_expand_size = mempool_expand_size_mb * ONE_MB;

  long long mempool_max_size_mb = MEMPOOL_MAX_SIZE_MB_DEFAULT;
  CmiGetArgInt(*argv, "+ofi_mempool_max_size_mb", (int*)&mempool_max_size_mb);
  context.mempool_max_size = mempool_max_size_mb * ONE_MB;

  context.mempool_lb_size = MEMPOOL_LB_DEFAULT;
  CmiGetArgInt(*argv, "+ofi_mempool_lb_size", (int*)&context.mempool_lb_size);

  context.mempool_rb_size = MEMPOOL_RB_DEFAULT;
  CmiGetArgInt(*argv, "+ofi_mempool_rb_size", (int*)&context.mempool_rb_size);

  if (context.mempool_lb_size > context.mempool_rb_size)
    CmiAbort("OFI::LrtsInit::Mempool left border should be less or equal to right border");
#if OFI_VERBOSE_STARTUP
  OFI_INFO("mempool init size: %ld\n", context.mempool_init_size);
  OFI_INFO("mempool expand size: %ld\n", context.mempool_expand_size);
  OFI_INFO("mempool max size: %lld\n", context.mempool_max_size);
  OFI_INFO("mempool left border size: %ld\n", context.mempool_lb_size);
  OFI_INFO("mempool right border size: %ld\n", context.mempool_rb_size);
#endif
#endif

  /**
   * Open fabric
   * The getinfo struct returns a fabric attribute struct that can be used to
   * instantiate the virtual or physical network. This opens a "fabric
   * provider". See man fi_fabric for details.
   */
  //    CmiPrintf("[%d] PMI_initialized %d : %d\n",*myNodeID, PMI2_Initialized(), PMI_SUCCESS);
  ret = fi_fabric(prov->fabric_attr, &context.fabric, NULL);
  if (ret < 0) {
    MACHSTATE1(3, "fi_fabric error: %d\n", ret);
    fi_freeinfo(providers);
    CmiAbort("OFI::LrtsInit::fi_fabric error");
  }

  /**
   * Create the access domain, which is the physical or virtual network or
   * hardware port/collection of ports.  Returns a domain object that can be
   * used to create endpoints.  See man fi_domain for details.
   */

  ret = fi_domain(context.fabric, prov, &context.domain, NULL);
  if (ret < 0) {
    MACHSTATE2(3, "[%d] fi_domain error: %d\n",*myNodeID, ret);
    fi_freeinfo(providers);
    //      CmiPrintf("[%d] fi_domain error: %d\n", *myNodeID, ret);
    CmiAbort("OFI::LrtsInit::fi_domain error, for single node use try --network=single_node_vni");
  }
  /**
   * Create a transport level communication endpoint.  To use the endpoint,
   * it must be bound to completion counters or event queues and enabled,
   * and the resources consumed by it, such as address vectors, counters,
   * completion queues, etc. See man fi_endpoint for more details.
   */
  ret = fi_endpoint(context.domain, /* In:  Domain object   */
		    prov,           /* In:  Provider        */
		    &context.ep,    /* Out: Endpoint object */
		    NULL);          /* Optional context     */
  if (ret < 0) {
    MACHSTATE1(3, "fi_endpoint error: %d\n", ret);
    fi_freeinfo(providers);
    CmiAbort("OFI::LrtsInit::fi_endpoint error %d", ret);
  }

  /**
   * Create the objects that will be bound to the endpoint.
   * The objects include:
   *     - completion queue for events
   *     - address vector of other endpoint addresses
   */
  cq_attr.format = FI_CQ_FORMAT_TAGGED;
  ret = fi_cq_open(context.domain, &cq_attr, &context.cq, NULL);
  if (ret < 0) {
    CmiAbort("OFI::LrtsInit::fi_cq_open error");
  }

  /**
   * Since the communications happen between Nodes and that each Node
   * has a number (NodeNo), we can use the Address Vector in FI_AV_TABLE
   * mode. The addresses of the Nodes simply need to be inserted in order
   * so that the NodeNo becomes the index in the AV. The advantage being
   * that the fi_addrs are stored by the OFI provider.
   */
  av_attr.type = FI_AV_TABLE;
  ret = fi_av_open(context.domain, &av_attr, &context.av, NULL);
  if (ret < 0) {
    CmiAbort("OFI::LrtsInit::fi_av_open error");
  }

  /**
   * Bind the CQ and AV to the endpoint object.
   */
  ret = fi_ep_bind(context.ep,
		   (fid_t)context.cq,
		   FI_RECV | FI_TRANSMIT);
  if (ret < 0) {
    CmiAbort("OFI::LrtsInit::fi_bind EP-CQ error");
  }
  ret = fi_ep_bind(context.ep,
		   (fid_t)context.av,
		   0);
  if (ret < 0) {
    CmiAbort("OFI::LrtsInit::fi_bind EP-AV error");
  }

  /**
   * Enable the endpoint for communication
   * This commits the bind operations.
   */
  ret = fi_enable(context.ep);
  if (ret < 0) {
    CmiAbort("OFI::LrtsInit::fi_enable error");
  }
#if OFI_VERBOSE_STARTUP
  OFI_INFO("use request cache: %d\n", USE_OFIREQUEST_CACHE);
#endif
#if USE_OFIREQUEST_CACHE
  /**
   * Create request cache.
   */
  context.request_cache = create_request_cache();
#endif

  /**
   * Create local receive buffers and pre-post them.
   */
  context.num_recv_reqs = OFI_NUM_RECV_REQS_DEFAULT;
  CmiGetArgInt(*argv, "+ofi_num_recvs", &context.num_recv_reqs);
  if (context.num_recv_reqs > OFI_NUM_RECV_REQS_MAX || context.num_recv_reqs <= 0)
    CmiAbort("OFI::LrtsInit::Num recv reqs range error");
#if OFI_VERBOSE_STARTUP
  OFI_INFO("number of pre-allocated recvs: %i\n", context.num_recv_reqs);
#endif
  /**
   * Exchange EP names and insert them into the AV.
   */
  //this is now the default because it is stable, but allow the
  //argument for backward compatibility
  CmiGetArgFlag(*argv, "+ofi_runtime_tcp");

  if (CmiGetArgFlag(*argv, "+ofi_runtime_ofi")) {
    OFI_INFO("exchanging addresses over OFI\n");
    ret = fill_av_ofi(*myNodeID, *numNodes, context.ep,
		      context.av, context.cq);
    if (ret < 0) {
      CmiAbort("OFI::LrtsInit::fill_av_ofi");
    }
  }
  else //
    {
    OFI_INFO("exchanging addresses over TCP\n");
    ret = fill_av(*myNodeID, *numNodes, context.ep,
		  context.av, context.cq);
    if (ret < 0) {
      CmiAbort("OFI::LrtsInit::fill_av");
    }
  }

#if CMK_SMP && CMK_SMP_SENDQ
  /**
   * Initialize send queue.
   */
  context.send_queue = PCQueueCreate();
#endif

  /**
   * Free providers info since it's not needed anymore.
   */
  fi_freeinfo(hints);
  hints = NULL;
  fi_freeinfo(providers);
  providers = NULL;
}

static inline
void prepost_buffers()
{
    OFIRequest **reqs=NULL;
#if CMK_CXI
    // CmiAlloc will go through LrtsAlloc, which will use a memory
    // pool, which should do all the right things wrt register, bind,
    // enable behind the scenes
    reqs = (OFIRequest **) CmiAlloc(sizeof(void*) * context.num_recv_reqs);
#else
    ALIGNED_ALLOC(reqs,(sizeof(void*) * context.num_recv_reqs));
#endif

    int i;
    for (i = 0; i < context.num_recv_reqs; i++) {
#if USE_OFIREQUEST_CACHE
        reqs[i] = alloc_request(context.request_cache);
#else
        reqs[i] = (OFIRequest *) CmiAlloc(sizeof(OFIRequest));
#endif
        reqs[i]->callback = recv_callback;

	reqs[i]->data.recv_buffer = CmiAlloc(context.eager_maxsize);
        CmiAssert(reqs[i]->data.recv_buffer);

        MACHSTATE2(3, "---> posting recv req %p buf=%p",
                   reqs[i], reqs[i]->data.recv_buffer);

        /* Receive from any node with any tag */
        OFI_RETRY(fi_trecv(context.ep,
                           reqs[i]->data.recv_buffer,
                           context.eager_maxsize,
                           NULL,
                           FI_ADDR_UNSPEC,
                           0,
                           OFI_OP_MASK,
                           &(reqs[i]->context)));
    }
    context.recv_reqs = reqs;
}

static inline
void send_short_callback(struct fi_cq_tagged_entry *e, OFIRequest *req)
{
    /**
     * A short message was sent.
     * Free up resources.
     */
    char *msg=NULL;

    MACHSTATE(3, "OFI::send_short_callback {");

    msg = (char *)req->data.short_msg;
    CmiAssert(msg);
    MACHSTATE1(3, "--> msg=%p", msg);

    if(req->freeMe)
      CmiFree(msg);

#if USE_OFIREQUEST_CACHE
    free_request(req);
#else
    CmiFree(req);
#endif

    MACHSTATE(3, "} OFI::send_short_callback done");
}

static inline
void send_rma_callback(struct fi_cq_tagged_entry *e, OFIRequest *req)
{
    /**
     * An OFIRmaHeader was sent.
     * Free up resources.
     */
    OFIRmaHeader *header=NULL;

    MACHSTATE(3, "OFI::send_rma_callback {");

    header = req->data.rma_header;
    free(header);

#if USE_OFIREQUEST_CACHE
    free_request(req);
#else
    CmiFree(req);
#endif

    MACHSTATE(3, "} OFI::send_rma_callback done");
}


#ifdef CMK_CXI
static inline
void ofi_send_reg(void *buf, size_t buf_size, int addr, uint64_t tag, OFIRequest *req, 	struct fid_mr* mr)
{
    if (context.use_inject && buf_size <= context.inject_maxsize)
    {
        /**
         * The message is small enough to be injected.
         * This won't generate any completion, so we can free the msg now.
         */
        MACHSTATE(3, "----> inject");

        OFI_RETRY(fi_tinject(context.ep,
                             buf,
                             buf_size,
                             addr,
                             tag));
        req->callback(NULL, req);
    }
    else
      {

	MACHSTATE3(3, "msg send mr %p: mr key %lu buf %p\n", mr, fi_mr_key(mr), buf);
        /* Else, use regular send. */
        OFI_RETRY(fi_tsend(context.ep,
                           buf,
                           buf_size,
#if CMK_CXI
			   fi_mr_desc(mr),
#else
			   NULL,
#endif
			   addr,
                           tag,
                           &req->context));
    }
}
#endif


static inline
void ofi_send(void *buf, size_t buf_size, int addr, uint64_t tag, OFIRequest *req)
{
    if (context.use_inject && buf_size <= context.inject_maxsize)
    {
        /**
         * The message is small enough to be injected.
         * This won't generate any completion, so we can free the msg now.
         */
        MACHSTATE(3, "----> inject");

        OFI_RETRY(fi_tinject(context.ep,
                             buf,
                             buf_size,
                             addr,
                             tag));
        req->callback(NULL, req);
    }
    else
      {
#if CMK_CXI

	struct fid_mr* mr = (struct fid_mr *) GetMemHndl(buf);
#endif

	MACHSTATE3(3, "msg send mr %p: mr key %lu buf %p\n", mr, fi_mr_key(mr), buf);
        /* Else, use regular send. */
        OFI_RETRY(fi_tsend(context.ep,
                           buf,
                           buf_size,
#if CMK_CXI
			   fi_mr_desc(mr),
#else
			   NULL,
#endif
			   addr,
                           tag,
                           &req->context));
    }
}

static inline
void ofi_register_and_send(void *buf, size_t buf_size, int addr, uint64_t tag, OFIRequest *req)
{
    if (context.use_inject && buf_size <= context.inject_maxsize)
    {
        /**
         * The message is small enough to be injected.
         * This won't generate any completion, so we can free the msg now.
         */
        MACHSTATE(3, "----> inject");
#if CMK_CXI

	struct fid_mr* mr;
	ofi_reg_bind_enable(buf, buf_size, &mr,&context);
#endif

        OFI_RETRY(fi_tinject(context.ep,
                             buf,
                             buf_size,
                             addr,
                             tag));
        req->callback(NULL, req);
    }
    else
      {
#if CMK_CXI

	struct fid_mr* mr;
	ofi_reg_bind_enable(buf, buf_size, &mr,&context);
#endif

	MACHSTATE3(3, "msg send mr %p: mr key %lu buf %p\n", mr, fi_mr_key(mr), buf);
        /* Else, use regular send. */
        OFI_RETRY(fi_tsend(context.ep,
                           buf,
                           buf_size,
#if CMK_CXI
			   fi_mr_desc(mr),
#else
			   NULL,
#endif
			   addr,
                           tag,
                           &req->context));
    }
}

/**
 * sendMsg is used to send a message.
 * In CMK_SMP mode, this is called by the comm thread.
 */
static inline int sendMsg(OFIRequest *req)
{
    int       ret;
    uint64_t  op;
    char     *buf=NULL;
    size_t    len;

    MACHSTATE5(2,
               "OFI::sendMsg destNode=%i destPE=%i size=%i msg=%p mode=%i {",
               req->destNode, req->destPE, req->size, req->data.short_msg, req->mode);

    if (req->size <= context.eager_maxsize) {
        /**
         * The message is small enough to be sent entirely.
         */
        MACHSTATE(3, "--> eager");

        op = OFI_OP_SHORT;
        buf = (char *)req->data.short_msg;
        len = req->size;
    } else {
        /**
         * The message is too long to be sent directly.
         * Let other side use RMA Read instead by sending an OFIRmaHeader.
         */
        MACHSTATE(3, "--> long");

        op = OFI_OP_LONG;
        buf = (char *)req->data.rma_header;
        len = sizeof(OFIRmaHeader);
    }

    ofi_send(buf, len, req->destNode, op, req);

    MACHSTATE(2, "} OFI::sendMsg");
    return 0;
}

const int event_send_short_callback      = 10333;
const int event_send_rma_callback        = 10444;
const int event_ofi_send                 = 10555;
const int event_sendMsg                  = 10556;
const int event_LrtsSendFunc             = 10557;
const int event_send_ack_callback        = 10560;
const int event_rma_read_callback        = 10561;
const int event_process_short_recv       = 10600;
const int event_process_long_recv        = 10601;
const int event_process_long_send_ack    = 10602;
const int event_recv_callback            = 10610;
const int event_process_completion_queue = 10650;
const int event_process_send_queue       = 10660;
const int event_reg_bind_enable          = 10670;
static int postInit = 0;

static void registerUserTraceEvents(void) {
#if CMI_MACH_TRACE_USEREVENTS && CMK_TRACE_ENABLED
  traceRegisterUserEvent("send_short_callback", event_send_short_callback);
  traceRegisterUserEvent("send_rma_callback", event_send_rma_callback);
  traceRegisterUserEvent("ofi_send", event_ofi_send);
  traceRegisterUserEvent("sendMsg", event_sendMsg);
  traceRegisterUserEvent("LrtsSendFunc", event_LrtsSendFunc);
  traceRegisterUserEvent("send_ack_callback", event_send_ack_callback);
  traceRegisterUserEvent("rma_read_callback", event_rma_read_callback);
  traceRegisterUserEvent("process_short_recv", event_process_short_recv);
  traceRegisterUserEvent("process_long_recv", event_process_long_recv);
  traceRegisterUserEvent("process_long_send_ack", event_process_long_send_ack);
  traceRegisterUserEvent("recv_callback", event_recv_callback);
  traceRegisterUserEvent("process_completion_queue", event_process_completion_queue);
  traceRegisterUserEvent("process_send_queue", event_process_send_queue);
  traceRegisterUserEvent("reg_bind_enable", event_reg_bind_enable);
#endif
}


/**
 * In non-SMP mode, this is used to send a message.
 * In CMK_SMP mode, this is called by a worker thread to send a message.
 */
CmiCommHandle LrtsSendFunc(int destNode, int destPE, int size, char *msg, int mode)
{

    int           ret;
    OFIRequest    *req=NULL;
#if CMK_SMP_TRACE_COMMTHREAD
    double startT, endT;
#endif
    MACHSTATE5(2,
            "OFI::LrtsSendFunc destNode=%i destPE=%i size=%i msg=%p mode=%i {",
            destNode, destPE, size, msg, mode);
#if CMK_SMP_TRACE_COMMTHREAD
    startT = CmiWallTimer();
#endif
    CmiSetMsgSize(msg, size);

#if USE_OFIREQUEST_CACHE
    req = alloc_request(context.request_cache);
#else
    req = (OFIRequest *) CmiAlloc(sizeof(OFIRequest));
#endif
    CmiAssert(req);

    req->destNode = destNode;
    req->destPE   = destPE;
    req->size     = size;
    req->mode     = mode;

    if (size <= context.eager_maxsize) {
        /**
         * The message is small enough to be sent entirely.
         */
        MACHSTATE(3, "--> eager");

        req->callback = send_short_callback;
        req->data.short_msg = msg;
    } else {
      /**
       * The message is too long to be sent directly.
       * Let other side use RMA Read instead by sending an OFIRmaHeader.
       */
      OFIRmaHeader  *rma_header;
      struct fid_mr *mr=NULL;
#if CMK_CXI
      uint32_t      requested_key = 0;
      block_header *base_addr;
#else
      uint64_t      requested_key = 0;
#endif

      if ((FI_MR_BASIC & context.mr_mode) ||
	  (FI_MR_SCALABLE & context.mr_mode))
	{
	  requested_key = __sync_fetch_and_add(&(context.mr_counter), 1);
	  /* Register new MR to RMA Read from */
	  ret = fi_mr_reg(context.domain,        /* In:  domain object */
			  msg,                   /* In:  lower memory address */
			  size,                  /* In:  length */
			  MR_ACCESS_PERMISSIONS, /* In:  access permissions */
			  0ULL,                  /* In:  offset (not used) */
			  requested_key,         /* In:  requested key */
			  0ULL,                  /* In:  flags */
			  &mr,                   /* Out: memregion object */
			  NULL);                 /* In:  context (not used) */
	}
      else if (FI_MR_ENDPOINT & context.mr_mode)
	{

#if CMK_CXI
	  mr               = (struct fid_mr *) GetMemHndl(msg);
	  size_t offset = GetMemOffsetFromBase(msg);
	  MACHSTATE4(3, "msg send mr %p: mr key %lu buf %p offset %lu\n", mr, fi_mr_key(mr), msg, offset);
#else
	  CmiAbort("not implemented");
#endif
	}
      MACHSTATE(3, "--> long");
      ALIGNED_ALLOC(rma_header,sizeof(OFIRmaHeader));
      rma_header->nodeNo  = CmiMyNodeGlobal();
#if CMK_CXI
      rma_header->src_msg = GetMemOffsetFromBase(msg);
      rma_header->orig_msg = (uint64_t) msg;
#else
      rma_header->src_msg = (uint64_t)msg;
#endif
      rma_header->len     = size;
      rma_header->key     = fi_mr_key(mr);
      rma_header->mr      = (uint64_t) mr;
      req->callback        = send_rma_callback;
      req->data.rma_header = rma_header;
      MACHSTATE3(3, "sending msg size=%d, hdl=%d, xhdl=%d",CmiGetMsgSize(msg),CmiGetHandler(msg), CmiGetXHandler(msg));
    }
#if CMK_SMP_TRACE_COMMTHREAD
    endT = CmiWallTimer();
    if (endT-startT>=TRACE_THRESHOLD) traceUserBracketEvent(event_LrtsSendFunc, startT, endT);
#endif
#if CMK_SMP && CMK_SMP_SENDQ
    /* Enqueue message */
    MACHSTATE2(2, " --> (PE=%i) enqueuing message (queue depth=%i)",
               CmiMyPe(), PCQueueLength(context.send_queue));
    PCQueuePush(context.send_queue, (char *)req);
#else
    /* Send directly */
    sendMsg(req);
#endif

    MACHSTATE(2, "} OFI::LrtsSendFunc");

    return (CmiCommHandle)req;
}

static inline
void send_ack_callback(struct fi_cq_tagged_entry *e, OFIRequest *req)
{
    /**
     * An OFIRmaAck was sent (see rma_read_callback()).
     * We are done with the RMA Read operation. Free up the resources.
     */
    OFILongMsg *long_msg=NULL;
#if CMK_SMP_TRACE_COMMTHREAD
    double startT, endT;
    startT = CmiWallTimer();
#endif

    MACHSTATE(3, "OFI::send_ack_callback {");

    long_msg = req->data.long_msg;
    CmiAssert(long_msg);

    free(long_msg);

#if USE_OFIREQUEST_CACHE
    free_request(req);
#else
    CmiFree(req);
#endif
#if CMK_SMP_TRACE_COMMTHREAD
    endT = CmiWallTimer();
    if (endT-startT>=TRACE_THRESHOLD) traceUserBracketEvent(event_send_ack_callback, startT, endT);
#endif
    MACHSTATE(3, "} OFI::send_ack_callback done");
}

static inline
void rma_read_callback(struct fi_cq_tagged_entry *e, OFIRequest *req)
{
    /**
     * An RMA Read operation completed.
     */
    OFILongMsg *long_msg=NULL;
#if CMK_SMP_TRACE_COMMTHREAD
    double startT, endT;
    startT = CmiWallTimer();
#endif

    MACHSTATE(3, "OFI::rma_read_callback {");

    long_msg = req->data.long_msg;
    CmiAssert(long_msg);
    CmiAssert(long_msg->completion_count > 0);

    long_msg->completion_count--;
    MACHSTATE1(3, "--> completion_count=%ld", long_msg->completion_count);

    if (0 == long_msg->completion_count) {
        /**
         * long_msg can be destroyed in case of fi_tinject,
         * so save pointer to assembly buffer to use it below.
         */
        char* asm_msg = long_msg->asm_msg;

        /**
         *  The message has been RMA Read completely.
         *  Send ACK to notify the other side that we are done.
         *  The resources are freed by send_ack_callback().
         */
        req->callback = send_ack_callback;
        req->data.long_msg = long_msg;

        ofi_send(&long_msg->rma_ack,
                 sizeof long_msg->rma_ack,
                 long_msg->nodeNo,
                 OFI_OP_ACK,
                 req);

        /**
         * Pass received message to upper layer.
         */
        MACHSTATE1(3, "--> Finished receiving msg size=%i", CMI_MSG_SIZE(asm_msg));
	MACHSTATE4(3, "received msg size=%d, hdl=%d, xhdl=%d last=%x",CmiGetMsgSize(asm_msg),CmiGetHandler(asm_msg), CmiGetXHandler(asm_msg), asm_msg[CMI_MSG_SIZE(asm_msg)-1]);
#if CMK_SMP_TRACE_COMMTHREAD
	endT = CmiWallTimer();
	if (endT-startT>=TRACE_THRESHOLD) traceUserBracketEvent(event_rma_read_callback, startT, endT);
#endif
	handleOneRecvedMsg(CMI_MSG_SIZE(asm_msg), asm_msg);
    } else {
#if USE_OFIREQUEST_CACHE
      free_request(req);
#else
      CmiFree(req);
#endif
#if CMK_SMP_TRACE_COMMTHREAD
    endT = CmiWallTimer();
    if (endT-startT>=TRACE_THRESHOLD) traceUserBracketEvent(event_rma_read_callback, startT, endT);
#endif
    }

    MACHSTATE(3, "} OFI::rma_read_callback done");
}

static inline
void process_short_recv(struct fi_cq_tagged_entry *e, OFIRequest *req)
{
    /**
     * A short message was received:
     *   - Pass the message to the upper layer,
     *   - Allocate new recv buffer.
     */

    char    *data=NULL;
    size_t  msg_size;
#if CMK_SMP_TRACE_COMMTHREAD
    double startT, endT;
    startT = CmiWallTimer();
#endif
    MACHSTATE(3, "OFI::process_short_recv");
    data = (char *)req->data.recv_buffer;
    CmiAssert(data);

    msg_size = CMI_MSG_SIZE(data);
    MACHSTATE2(3, "--> eager msg (e->len=%ld msg_size=%ld)", e->len, msg_size);

    req->data.recv_buffer = CmiAlloc(context.eager_maxsize);
    CmiAssert(req->data.recv_buffer);
    MACHSTATE3(3, "received msg size=%d, hdl=%d, xhdl=%d",CmiGetMsgSize(data),CmiGetHandler(data), CmiGetXHandler(data));
#if CMK_SMP_TRACE_COMMTHREAD
    endT = CmiWallTimer();
    if (endT-startT>=TRACE_THRESHOLD) traceUserBracketEvent(event_process_short_recv, startT, endT);
#endif
    handleOneRecvedMsg(e->len, data);
}

static inline
void process_long_recv(struct fi_cq_tagged_entry *e, OFIRequest *req)
{
    /**
     * An OFIRmaHeader was received:
     *   - Allocate enough space to store the long message,
     *   - Create OFILongMsg to keep track of the data retrieval,
     *   - Issue the RMA Read operation(s) to retrieve the data.
     */

    int ret;
    OFILongMsg *long_msg=NULL;
    OFIRequest *rma_req=NULL;
    OFIRmaHeader *rma_header=NULL;
    struct fid_mr *mr = NULL;
    char *asm_buf=NULL;
    int nodeNo;
    uint64_t rbuf;
    size_t len;
    uint64_t rkey;
    uint64_t rmsg;
    uint64_t rmr;
    char *lbuf=NULL;
    size_t remaining;
    size_t chunk_size;
#if CMK_SMP_TRACE_COMMTHREAD
    double startT, endT;
    startT = CmiWallTimer();
#endif
    MACHSTATE(3, "OFI::process_long_recv");
    CmiAssert(e->len == sizeof(OFIRmaHeader));

    /**
     * Parse header
     */
    rma_header = req->data.rma_header;

    nodeNo = rma_header->nodeNo;
    rmsg   = rma_header->src_msg;
    len    = rma_header->len;
    rkey   = rma_header->key;
    rmr    = rma_header->mr;

    MACHSTATE5(3, "--> Receiving long msg src node %d len=%ld rptr=0x%lx rmsg=0x%lu rmr=0x%lx", nodeNo, len, rma_header->orig_msg, rmsg, rmr);
    MACHSTATE3(3, "--> Receiving long msg rptr=0x%lx rkey=0x%lu rmr=0x%lx", rma_header->orig_msg, rkey, rmr);

    /**
     * Prepare buffer
     */


    if (FI_MR_BASIC & context.mr_mode)
      {
	MACHSTATE1(3, "FI_MR_BASIC %d", context.mr_mode);
	asm_buf = (char *)CmiAlloc(len);
	/* Register local MR to read into */
        ret = fi_mr_reg(context.domain,        /* In:  domain object */
                        asm_buf,               /* In:  lower memory address */
                        len,                   /* In:  length */
                        MR_ACCESS_PERMISSIONS, /* In:  access permissions */
                        0ULL,                  /* In:  offset (not used) */
                        0ULL,                  /* In:  requested key (none)*/
                        0ULL,                  /* In:  flags */
                        &mr,                   /* Out: memregion object */
                        NULL);                 /* In:  context (not used) */
        if (ret) {
            MACHSTATE1(3, "fi_mr_reg short buf error: %d\n", ret);
            CmiAbort("fi_mr_reg error");
        }
      }
      else if (FI_MR_ENDPOINT & context.mr_mode) {
	asm_buf = (char *)CmiAlloc(len);
	//	memset(asm_buf,0,len);
      }
    CmiAssert(asm_buf);
    /**
     * Save some information about the RMA Read operation(s)
     */
    ALIGNED_ALLOC(long_msg, sizeof(OFILongMsg));

    long_msg->asm_msg          = asm_buf;
    long_msg->nodeNo           = nodeNo;
    long_msg->rma_ack.mr       = rmr;
    long_msg->completion_count = 0;
#if CMK_CXI
    // so the other side can free the right buffer in the offset case
    long_msg->rma_ack.src_msg  = rma_header->orig_msg;
    long_msg->mr = (struct fid_mr *) GetMemHndl(asm_buf);
    MACHSTATE2(3, "long msg mempool mr %p: mr key %lu\n", long_msg->mr, fi_mr_key(long_msg->mr));
#else
    long_msg->rma_ack.src_msg  = rmsg;
    long_msg->mr               =mr;
#endif
    /**
     * Issue RMA Read request(s)
     */
    remaining = len;
    lbuf      = asm_buf;
    rbuf      = (FI_MR_SCALABLE == context.mr_mode) ? 0 : rmsg;

    while (remaining > 0) {
        /* Determine size of operation */
        chunk_size = (remaining <= context.rma_maxsize) ? remaining : context.rma_maxsize;

#if USE_OFIREQUEST_CACHE
        rma_req = alloc_request(context.request_cache);
#else
        rma_req = (OFIRequest *) CmiAlloc(sizeof(OFIRequest));
#endif
        CmiAssert(rma_req);
        rma_req->callback = rma_read_callback;
        rma_req->data.long_msg = long_msg;

        /* Increment number of expected completions */
        long_msg->completion_count++;

        MACHSTATE5(3, "---> RMA Read lbuf %p rbuf %lu rmsg %lu len %ld chunk #%lu",
                   lbuf, rbuf, rmsg, chunk_size, long_msg->completion_count);


	OFI_RETRY(fi_read(context.ep,
                          lbuf,
                          chunk_size,
			    fi_mr_desc(long_msg->mr),
                          nodeNo,
                          rmsg,
                          rkey,
                          &rma_req->context));
	remaining  -= chunk_size;
	lbuf       += chunk_size;
	rbuf       += chunk_size;
    }
    MACHSTATE4(3, "---> RMA completed lbuf %p rbuf %lu len %lu comp %lu",
	       lbuf, rbuf, len, long_msg->completion_count);
#if CMK_SMP_TRACE_COMMTHREAD
    endT = CmiWallTimer();
    if (endT-startT>=TRACE_THRESHOLD) traceUserBracketEvent(event_process_long_recv, startT, endT);
#endif
}

static inline
void process_long_send_ack(struct fi_cq_tagged_entry *e, OFIRequest *req)
{
    /**
     * An OFIRmaAck was received; Close memory region and free original msg.
     */

    struct fid *mr=NULL;
    char *msg=NULL;
#if CMK_SMP_TRACE_COMMTHREAD
    double startT, endT;
    startT = CmiWallTimer();
#endif
    MACHSTATE(3, "OFI::process_long_send_ack");
    mr = (struct fid*)req->data.rma_ack->mr;
    CmiAssert(mr);
    //    fi_close(mr);

    msg = (char *)req->data.rma_ack->src_msg;
    MACHSTATE2(3, "OFI::process_long_send_ack for msg %p mr %p",msg, mr);
    CmiAssert(msg);

    MACHSTATE2(3, "--> Finished sending msg size=%i msg ptr %p", CMI_MSG_SIZE(msg), msg);

    CmiFree(msg);
#if CMK_SMP_TRACE_COMMTHREAD
    endT = CmiWallTimer();
    if (endT-startT>=TRACE_THRESHOLD) traceUserBracketEvent(event_process_long_send_ack, startT, endT);
#endif
}

static inline
void recv_callback(struct fi_cq_tagged_entry *e, OFIRequest *req)
{
    /**
     * Some data was received:
     *  - the tag tells us what type of message it is; process it
     *  - repost recv request
     */
    MACHSTATE(3, "OFI::recv_callback {");

    switch (e->tag) {
    case OFI_OP_SHORT:
        process_short_recv(e, req);
        break;
    case OFI_OP_LONG:
        process_long_recv(e, req);
        break;
    case OFI_OP_ACK:
        process_long_send_ack(e, req);
        break;
#if CMK_ONESIDED_IMPL
    case OFI_RDMA_DIRECT_REG_AND_PUT:
        process_onesided_reg_and_put(e, req);
        break;
    case OFI_RDMA_DIRECT_REG_AND_GET:
        process_onesided_reg_and_get(e, req);
        break;
    case OFI_RDMA_DIRECT_DEREG_AND_ACK:
        process_onesided_dereg_and_ack(e, req);
        break;
#endif
    default:
        MACHSTATE2(3, "--> unknown operation %lu len=%lu", e->tag, e->len);
        CmiAbort("!! Wrong operation !!");
    }
#if CMK_SMP_TRACE_COMMTHREAD
    double startT, endT;
    startT = CmiWallTimer();
#endif
    MACHSTATE2(3, "Reposting recv req %p buf=%p", req, req->data.recv_buffer);
    OFI_RETRY(fi_trecv(context.ep,
                       req->data.recv_buffer,
                       context.eager_maxsize,
                       NULL,
                       FI_ADDR_UNSPEC,
                       0,
                       OFI_OP_MASK,
                       &req->context));
#if CMK_SMP_TRACE_COMMTHREAD
    endT = CmiWallTimer();
    if (endT-startT>=TRACE_THRESHOLD) traceUserBracketEvent(event_recv_callback, startT, endT);
#endif
    MACHSTATE(3, "} OFI::recv_callback done");
}

static inline
int process_completion_queue()
{
    int ret;
    struct fi_cq_tagged_entry entries[context.cq_entries_count];
    struct fi_cq_err_entry error;
    OFIRequest *req=NULL;
#if CMK_SMP_TRACE_COMMTHREAD
    double startT, endT;
    startT = CmiWallTimer();
#endif
    MACHSTATE(3, "OFI::process_completion_queue");
    ret = fi_cq_read(context.cq, entries, context.cq_entries_count);
    if (ret > 0)
    {
        /* One or more completions were found */
        int idx;
        for (idx = 0; idx < ret; idx++)
        {
            struct fi_cq_tagged_entry* e = &(entries[idx]);
            CmiAssert(e->op_context != NULL);

            /* Retrieve request from context */
            req = container_of((e->op_context), OFIRequest, context);

            /* Execute request callback */
            if ((e->flags & FI_SEND) ||
                (e->flags & FI_RECV) ||
                (e->flags & FI_RMA))
            {
                req->callback(e, req);
            }
            else
            {
                MACHSTATE1(3, "Missed event with flags=%lu", e->flags);
                CmiAbort("!! Missed an event !!");
            }
        }
    }
    else if (ret == -FI_EAGAIN)
    {
        /* Completion Queue is empty */
        ret = 0;
    }
    else if (ret < 0)
    {
        MACHSTATE1(3, "POLL: Error %d\n", ret);
        CmiPrintf("POLL: Error %d\n", ret);
        if (ret == -FI_EAVAIL)
        {
            MACHSTATE(3, "POLL: error available\n");
            CmiPrintf("POLL: error available\n");
            ret = fi_cq_readerr(context.cq, &error, sizeof(error));
            if (ret < 0)
            {
                CmiAbort("can't retrieve error");
            }
            MACHSTATE4(3, "POLL: error is %d (ret=%d) len %lu tag %lu\n", error.err, ret, error.len, error.tag);
            CmiPrintf("POLL: error is %d (ret=%d) len %lu tag %lu\n", error.err, ret, error.len, error.tag);
            const char* strerror = fi_cq_strerror(context.cq, error.prov_errno, error.err_data, nullptr, 0);
            if (strerror == nullptr)
            {
                CmiAbort("can't retrieve error string");
            }
            MACHSTATE1(3, "POLL: error string is \"%s\"\n", strerror);
            CmiPrintf("POLL: error string is \"%s\"\n", strerror);
        }
        CmiAbort("Polling error");
    }
#if CMK_SMP_TRACE_COMMTHREAD
    endT = CmiWallTimer();
    if (endT-startT>=TRACE_THRESHOLD) traceUserBracketEvent(event_process_completion_queue, startT, endT);
#endif
    return ret;
}

#if CMK_SMP && CMK_SMP_SENDQ
static inline
int process_send_queue()
{
    OFIRequest *req=NULL;
    int ret = 0;
#if CMK_SMP_TRACE_COMMTHREAD
    double startT, endT;
    startT = CmiWallTimer();
#endif
    /**
     * Comm thread sends the next message that is waiting in the send_queue.
     */
    req = (OFIRequest*)PCQueuePop(context.send_queue);
    if (req)
    {
        MACHSTATE2(2, " --> (PE=%i) dequeuing message (queue depth: %i)",
                CmiMyPe(), PCQueueLength(context.send_queue));
        MACHSTATE5(2,
                " --> dequeuing destNode=%i destPE=%i size=%d msg=%p mode=%d",
                req->destNode, req->destPE, req->size, req->data, req->mode);
        sendMsg(req);
        ret = 1;
    }
#if CMK_SMP_TRACE_COMMTHREAD
    endT = CmiWallTimer();
    if (endT-startT>=TRACE_THRESHOLD) traceUserBracketEvent(event_process_send_queue, startT, endT);
#endif
    return ret;
}
#endif

#if USE_MEMPOOL

void *alloc_mempool_block(size_t *size, mem_handle_t *mem_hndl, int expand_flag)
{
    size_t alloc_size =  expand_flag ? context.mempool_expand_size : context.mempool_init_size;
    if (*size < alloc_size) *size = alloc_size;
    if (*size > context.mempool_max_size)
    {
        CmiPrintf("Error: there is attempt to allocate memory block with size %ld which is greater than the maximum mempool allowed %lld.\n"
                  "Please increase the maximum mempool size by using +ofi-mempool-max-size\n",
                  *size, context.mempool_max_size);
        CmiAbort("alloc_mempool_block");
    }

    void *pool;
    posix_memalign(&pool,ALIGNBUF,*size);
    ofi_reg_bind_enable(pool, *size, mem_hndl,&context);
    MACHSTATE4(3, "alloc_mempool_block ptr %p mr %p key %lu inkey %d\n", pool, *mem_hndl, fi_mr_key(*mem_hndl) , context.mr_counter-1);
#endif
    return pool;
}

void free_mempool_block(void *ptr, mem_handle_t mem_hndl)
{
    free(ptr);
}

#endif

void LrtsPreCommonInit(int everReturn)
{
    MACHSTATE(2, "OFI::LrtsPreCommonInit {");

    //    PRINT_THREAD_INFO();

#if USE_MEMPOOL
    CpvInitialize(mempool_type*, mempool);

    CpvAccess(mempool) = mempool_init(context.mempool_init_size,
                                      alloc_mempool_block,
                                      free_mempool_block,
                                      context.mempool_max_size);

    block_header* current = &(CpvAccess(mempool)->block_head);
    struct fid_mr* extractedmr  = (struct fid_mr *) MEMPOOL_GetBlockMemHndl(current);
    MACHSTATE2(3, "LrtsPreCommonInit mempool->block_head.mem_hndl %p extracted %p\n", CpvAccess(mempool)->block_head.mem_hndl, extractedmr );
#endif

    if (!CmiMyRank()) prepost_buffers();

    MACHSTATE(2, "} OFI::LrtsPreCommonInit");
}

void LrtsPostCommonInit(int everReturn)
{
    MACHSTATE(2, "OFI::LrtsPostCommonInit {");
#if CMI_MACH_TRACE_USEREVENTS && CMK_TRACE_ENABLED
    CpvInitialize(double, projTraceStart);
    /* only PE 0 needs to care about registration (to generate sts file). */
    //if (CmiMyPe() == 0)
    {
        registerMachineUserEventsFunction(&registerUserTraceEvents);
    }
#endif
    postInit=1;
    MACHSTATE(2, "} OFI::LrtsPostCommonInit");
}

void LrtsAdvanceCommunication(int whileidle)
{
    int processed_count;
    MACHSTATE(2, "OFI::LrtsAdvanceCommunication {");

    do
    {
        processed_count = 0;
        processed_count += process_completion_queue();
#if CMK_SMP && CMK_SMP_SENDQ
        processed_count += process_send_queue();
#endif
    } while (processed_count > 0);
    MACHSTATE(2, "} OFI::LrtsAdvanceCommunication done");
}

void LrtsDrainResources() /* used when exiting */
{
    int ret;
    MACHSTATE1(2, "OFI::LrtsDrainResources (PE=%i {", CmiMyPe());
    LrtsAdvanceCommunication(0);
    ret = runtime_barrier();
    if (ret) {
        MACHSTATE1(2, "runtime_barrier() returned %i", ret);
        CmiAbort("OFI::LrtsDrainResources failed");
    }
    MACHSTATE(2, "} OFI::LrtsDrainResources");
}

#if USE_MEMPOOL
/* useful for Onesided so that it can avoid per buffer overheads,
   which will otherwise totally dominate most micro benchmarks in an
   unpleasant way.  Basically same logic as LrtsAlloc, just no
   converse header */

void* LrtsPoolAlloc(int n_bytes)
{
  return(LrtsAlloc(n_bytes,0));
}
#endif

void* LrtsAlloc(int n_bytes, int header)
{
    char *ptr = NULL;
    size_t size = n_bytes + header;
    MACHSTATE(3, "OFI::LrtsAlloc");
#if USE_MEMPOOL
    if (size <= context.mempool_lb_size)
      {
	CmiAbort("OFI pool lower boundary violation");
      }
    else
      {
	CmiAssert(header+sizeof(mempool_header) <= ALIGNBUF);
	n_bytes=ALIGN64(n_bytes);
	if( n_bytes < BIG_MSG)
	  {
            char *res = (char *)mempool_malloc(CpvAccess(mempool), ALIGNBUF+n_bytes, 1);

	    // note CmiAlloc wrapper will move the pointer past the header
	    if (res) ptr = res;

	    MACHSTATE3(3, "OFI::LrtsAlloc ptr %p - header %d = %p", res, header, ptr);
	    size_t offset1=GetMemOffsetFromBase(ptr+header);
	    struct fid_mr* extractedmr  = (struct fid_mr *) GetMemHndl(ptr+header);
	    MACHSTATE5(3, "OFI::LrtsAlloc not big from pool ret %p ptr %p memhndl %p mempoolptrfromret %p offset %lu", res, ptr, extractedmr, MEMPOOL_GetMempoolPtr(MEMPOOL_GetMempoolHeader(ptr+header,sizeof(mempool_header)+header)), offset1);
	  }
	else
	  {
#if LARGEPAGE
	    n_bytes = ALIGNHUGEPAGE(n_bytes+ALIGNBUF);
	    char *res = (char *)my_get_huge_pages(n_bytes);
#else // not largepage
	    n_bytes = size+ sizeof(out_of_pool_header);
	    n_bytes = ALIGN64(n_bytes);
	    char *res;

	    MACHSTATE1(3, "OFI::LrtsAlloc unpooled RB big %d", n_bytes);
	    posix_memalign((void **)&res,ALIGNBUF, n_bytes);
	    out_of_pool_header *mptr= (out_of_pool_header*) res;
	    // construct the minimal version of the
	    // mempool_header+block_header like a memory pool message
	    // so that all messages can be handled the same way with
	    // the same macros and functions.  We need the mptr,
	    // block_ptr, and mem_hndl fields and can test the size to
	    // know to not put it back in the normal pool on free
#if CMK_CXI
	    struct fid_mr *mr;
	    ofi_reg_bind_enable(res, n_bytes, &mr,&context);
	    mptr->block_head.mem_hndl=mr;
#endif
	    mptr->block_head.mptr=(struct mempool_type*) res;
	    mptr->block.block_ptr=(struct block_header *)res;
	    ptr=(char *) res + (sizeof(out_of_pool_header));
	    //	    char *testptr = ptr+sizeof(CmiChunkHeader);
	    //	    CmiAssert(GetBaseAllocPtr(testptr)==mptr->block.block_ptr);
	    // MACHSTATE5(3, "OFI::LrtsAlloc unpooled base %p, msg %p, size %lu, mr %p macrooffset %lu", res, testptr, n_bytes, mr, GetMemOffsetFromBase(testptr));
#endif //LARGEPAGE

	  }
#else //not MEMPOOL
	n_bytes = ALIGN64(n_bytes);           /* make sure size if 4 aligned */
	char *res;
	posix_memalign((void **)&res, ALIGNBUF, n_bytes+ALIGNBUF);
#if CMK_CXI
	struct fid_mr *mr;
	ofi_reg_bind_enable(res, n_bytes+ALIGNBUF, &mr,&context);
	((block_header *)res)->mem_hndl = mr;
#endif // not cxi
	ptr = res;
#endif //mempool
#if USE_MEMPOOL
      }
#endif
    if (!ptr) CmiAbort("LrtsAlloc");
    return ptr;
}


void LrtsFree(void *msg)
{

  int headersize = sizeof(CmiChunkHeader);
  char *aligned_addr = (char *)msg + headersize - ALIGNBUF;
  CmiUInt4 size = SIZEFIELD((char*)msg+headersize);
  MACHSTATE1(3, "OFI::LrtsFree %p", msg);
#if USE_MEMPOOL
  if (size <= context.mempool_lb_size)
    CmiAbort("OFI: mempool lower boundary violation");
  else
    size = ALIGN64(size);
#endif
  if(size>=BIG_MSG)
    {
#if LARGEPAGE
      int s = ALIGNHUGEPAGE(size+ALIGNBUF);
      my_free_huge_pages(msg, s);
#else
#if CMK_CXI
      MACHSTATE1(3, "OFI::LrtsFree fi_close mr %p", (struct fid *)GetMemHndl( (char* )msg  +sizeof(CmiChunkHeader)));
      fi_close( (struct fid *)GetMemHndl( (char* )msg  +sizeof(CmiChunkHeader)));
      MACHSTATE2(3, "OFI::LrtsFree free msg next ptr %p vs ptr %p", GetBaseAllocPtr((char*)msg+sizeof(CmiChunkHeader)), (char *)msg-sizeof(out_of_pool_header));
      free((char *)msg-sizeof(out_of_pool_header));
#else
      free((char*)msg);
#endif
#endif
    }
  else
    {
#if USE_MEMPOOL
      // all this alignedbuf stuff is nuts, CmiFree gave us the right pointer
#if CMK_SMP
      //      mempool_free_thread(aligned_addr + sizeof(mempool_header));
      mempool_free_thread(msg);
#else
      //      mempool_free(CpvAccess(mempool), aligned_addr + sizeof(mempool_header));
      mempool_free(CpvAccess(mempool), msg);
#endif /* CMK_SMP */
#else
      free(aligned_addr);
#endif /* USE_MEMPOOL */
    }
}

void LrtsExit(int exitcode)
{
    int        ret;
    int        i;
    OFIRequest *req=NULL;

    MACHSTATE(2, "OFI::LrtsExit {");

    LrtsAdvanceCommunication(0);

    for (i = 0; i < context.num_recv_reqs; i++)
    {
        req = context.recv_reqs[i];
        ret = fi_cancel((fid_t)context.ep, (void *)&(req->context));
        if (ret < 0) CmiAbort("fi_cancel error");
	CmiFree(req->data.recv_buffer);
#if USE_OFIREQUEST_CACHE
        free_request(req);
#else
	CmiFree(req);
#endif
    }

#if CMK_SMP && CMK_SMP_SENDQ
    PCQueueDestroy(context.send_queue);
#endif

#if CMK_CXI
    if (context.recv_reqs)
      CmiFree(context.recv_reqs);
#else
    if (context.recv_reqs)
        free(context.recv_reqs);
#endif
    if (context.av)
        fi_close((struct fid *)(context.av));
    if (context.cq)
        fi_close((struct fid *)(context.cq));
    if (context.ep)
        fi_close((struct fid *)(context.ep));
    if (context.domain)
        fi_close((struct fid *)(context.domain));
    if (context.fabric)
        fi_close((struct fid *)(context.fabric));

#if USE_OFIREQUEST_CACHE
    destroy_request_cache(context.request_cache);
#endif

#if USE_MEMPOOL
    mempool_destroy(CpvAccess(mempool));
#endif

    if(!CharmLibInterOperate || userDrivenMode) {
        ret = runtime_barrier();
        if (ret) {
            MACHSTATE1(2, "runtime_barrier() returned %i", ret);
            CmiAbort("OFI::LrtsExit failed");
        }
        ret = runtime_fini();
        if (ret) {
            MACHSTATE1(2, "runtime_fini() returned %i", ret);
            CmiAbort("OFI::LrtsExit failed");
        }
        if (!userDrivenMode) {
          exit(exitcode);
        }
    }

    MACHSTATE(2, "} OFI::LrtsExit");
}

#if CMK_MACHINE_PROGRESS_DEFINED
void CmiMachineProgressImpl()
{
    MACHSTATE(2, "OFI::CmiMachineProgressImpl {");
    if (CmiMyRank() == CmiMyNodeSize()) {
        CommunicationServerThread(0);
    } else {
        MACHSTATE(2, "skipping");
    }
    MACHSTATE(2, "} OFI::CmiMachineProgressImpl");
}
#endif

/* In CMK_SMP, this is called by worker thread */
void LrtsPostNonLocal()
{
    MACHSTATE(2, "OFI::LrtsPostNonLocal {");
    MACHSTATE(2, "} OFI::LrtsPostNonLocal");
}

void LrtsAbort(const char *message)
{
    MACHSTATE1(2, "OFI::LrtsAbort '%s' {", message);
    exit(1);
    MACHSTATE(2, "} OFI::LrtsAbort");
    CMI_NORETURN_FUNCTION_END
}

void  LrtsNotifyIdle()
{
    MACHSTATE(2, "OFI::LrtsNotifyIdle {");
    MACHSTATE(2, "} OFI::LrtsNotifyIdle");
}

void  LrtsBeginIdle()
{
    MACHSTATE(2, "OFI::LrtsBeginIdle {");
    MACHSTATE(2, "} OFI::LrtsBeginIdle");
}

void  LrtsStillIdle()
{
    MACHSTATE(2, "OFI::LrtsStillIdle {");
    MACHSTATE(2, "} OFI::LrtsStillIdle");
}

void  LrtsBarrier()
{
    int ret;
    MACHSTATE(2, "OFI::LrtsBarrier {");
    ret = runtime_barrier();
    if (ret) {
        MACHSTATE1(2, "runtime_barrier() returned %i", ret);
        CmiAbort("OFI::LrtsBarrier failed");
    }
    MACHSTATE(2, "} OFI::LrtsBarrier");
}

/* Other assist function */

/**
 * fill_av_ofi() is used during LrtsInit to exchange all the EP names and to
 * insert them into the AV. The exchange is performed using both PMI and OFI.
 * This is used by default. See +ofi_runtime_tcp flag for other option.
 */
static
int fill_av_ofi(int myid,
                int nnodes,
                struct fid_ep *ep,
                struct fid_av *av,
                struct fid_cq *cq)
{
    char                       my_epname[FI_NAME_MAX];
    size_t                     epnamelen;
    int                        max_keylen;
    char                      *key;
    char                      *epnames;
    size_t                     epnameslen;
    struct fi_context         *epnames_contexts;
    struct fi_cq_tagged_entry  e;
    size_t                     nexpectedcomps;
    int                        ret;
    int                        i;

    /**
     * Get our EP name. This will be exchanged with the other nodes.
     */
    epnamelen = sizeof(my_epname);
    ret = fi_getname((fid_t)ep, &my_epname, &epnamelen);
    MACHSTATE1(3, "OFI::fill_av_ofi name %s", my_epname);
    CmiAssert(FI_NAME_MAX >= epnamelen);
    if (ret < 0) {
        CmiAbort("OFI::LrtsInit::fi_getname error");
    }

    /**
     * Publish our EP name.
     */
    ret = runtime_get_max_keylen(&max_keylen);
    if (ret) {
        CmiAbort("OFI::LrtsInit::runtime_get_max_keylen error");
    }

    key = (char *)malloc(max_keylen);
    CmiAssert(key);

    ret = snprintf(key, max_keylen, OFI_KEY_FORMAT_EPNAME, myid);
    if (ret < 0) {
        CmiAbort("OFI::LrtsInit::snprintf error");
    }

    ret = runtime_kvs_put(key, &my_epname, epnamelen);
    if (ret) {
        CmiAbort("OFI::LrtsInit::runtime_kvs_put error");
    }

    /**
     * Allocate buffer which will contain all the EP names ordered by Node Id.
     * Once all the names are exchanged, they will be inserted into the AV.
     */
    epnameslen = FI_NAME_MAX * nnodes;
    epnames = (char *)malloc(epnameslen);
    CmiAssert(epnames);
    memset(epnames, 0, epnameslen);

    if (myid != 0) {
        /**
         * Non-root nodes expect a message which contains the EP names.
         */
        epnames_contexts = (struct fi_context *)malloc(sizeof(struct fi_context));
        CmiAssert(epnames_contexts);

        ret = fi_trecv(ep,
                       epnames,
                       epnameslen,
                       NULL,
                       FI_ADDR_UNSPEC,
                       OFI_OP_NAMES,
                       0ULL,
                       &epnames_contexts[0]);

        /* Reap 1 recv completion */
        nexpectedcomps = 1;
    }

    /**
     * Wait for all the other nodes to publish their EP names.
     */
    ret = runtime_barrier();
    if (ret) {
        CmiAbort("OFI::LrtsInit::runtime_barrier error");
    }

    if (myid == 0) {
        /**
         * Root gathers all the epnames and sends them to the other nodes.
         */

        /* Retrieve all epnames */
        for (i=0; i<nnodes; ++i) {
            memset(key, 0, max_keylen);
            ret = snprintf(key, max_keylen, OFI_KEY_FORMAT_EPNAME, i);
            if (ret < 0) {
                CmiAbort("OFI::LrtsInit::snprintf error");
            }

            ret = runtime_kvs_get(key, epnames+(i*epnamelen), epnamelen, i);
            if (ret) {
                CmiAbort("OFI::LrtsInit::runtime_kvs_get error");
            }
        }

        /* AV insert */
        ret = fi_av_insert(av, epnames, nnodes, NULL, 0, NULL);
        if (ret < 0) {
            CmiAbort("OFI::LrtsInit::fi_av_insert error");
        }

        /* Send epnames to everyone */
        epnames_contexts = (struct fi_context *)malloc(nnodes * sizeof(struct fi_context));
        CmiAssert(epnames_contexts);
        for (i=1; i<nnodes; ++i) {
            ret = fi_tsend(ep,
                           epnames,
                           epnameslen,
                           NULL,
                           i,
                           OFI_OP_NAMES,
                           &epnames_contexts[i]);
            if (ret) {
                CmiAbort("OFI::LrtsInit::fi_tsend error (+ofi_runtime_tcp may be needed)");
            }
        }

        /* Reap 1 send completion per non-root node */
        nexpectedcomps = nnodes - 1;
    }

    while (nexpectedcomps > 0) {
        memset(&e, 0, sizeof e);
        ret = fi_cq_read(cq, &e, 1);
        if (ret > 0) {
            /* A completion was found */
            if (((e.flags & FI_SEND) && (myid != 0)) ||
                ((e.flags & FI_RECV) && (myid == 0))) {
                /* This message was received in error */
                CmiAbort("OFI::LrtsInit::fi_cq_read unexpected completion.");
            }
            nexpectedcomps--;
        } else if(ret == -FI_EAGAIN) {
            /* Completion Queue is empty */
            continue;
        } else if (ret < 0) {
           CmiAbort("OFI::LrtsInit::fi_cq_read error.");
        }
    }

    if (myid != 0) {
        /**
         * Non-root nodes
         */

        /* AV insert */
        ret = fi_av_insert(av, epnames, nnodes, NULL, 0, NULL);
        if (ret < 0) {
            CmiAbort("OFI::LrtsInit::fi_av_insert error");
        }
    }

    free(key);
    free(epnames);
    free(epnames_contexts);

    return 0;
}

/**
 * fill_av() is used during LrtsInit to exchange all the EP names and to insert
 * them into the AV. The exchange is performed using PMI only. Currently
 * enabled only if +ofi_runtime_tcp flag is set.
 */
static
int fill_av(int myid,
            int nnodes,
            struct fid_ep *ep,
            struct fid_av *av,
            struct fid_cq *cq)
{
    char    my_epname[FI_NAME_MAX];
    size_t  epnamelen;
    int     max_keylen;
    char   *key;
    char   *epnames;
    size_t  epnameslen;
    int     ret;
    int     i;

    /**
     * Get our EP name. This will be exchanged with the other nodes.
     */
    epnamelen = sizeof(my_epname);
    ret = fi_getname((fid_t)ep, &my_epname, &epnamelen);
    CmiAssert(FI_NAME_MAX >= epnamelen);
    if (ret < 0) {
        CmiAbort("OFI::LrtsInit::fi_getname error");
    }

    /**
     * Publish our EP name.
     */
    ret = runtime_get_max_keylen(&max_keylen);
    if (ret) {
        CmiAbort("OFI::LrtsInit::runtime_get_max_keylen error");
    }

    key = (char *)malloc(max_keylen);
    CmiAssert(key);

    ret = snprintf(key, max_keylen, OFI_KEY_FORMAT_EPNAME, myid);
    if (ret < 0) {
        CmiAbort("OFI::LrtsInit::snprintf error");
    }

    ret = runtime_kvs_put(key, &my_epname, epnamelen);
    if (ret) {
        CmiAbort("OFI::LrtsInit::runtime_kvs_put error");
    }

    /**
     * Allocate buffer which will contain all the EP names ordered by Node Id.
     * Once all the names are exchanged, they will be inserted into the AV.
     */
    epnameslen = FI_NAME_MAX * nnodes;
    epnames = (char *)malloc(epnameslen);
    CmiAssert(epnames);
    memset(epnames, 0, epnameslen);

    /**
     * Wait for all the other nodes to publish their EP names.
     */
    ret = runtime_barrier();
    if (ret) {
        CmiAbort("OFI::LrtsInit::runtime_barrier error");
    }

    /**
     * Retrieve all the EP names in order.
     */
    for (i=0; i<nnodes; ++i) {
        memset(key, 0, max_keylen);
        ret = snprintf(key, max_keylen, OFI_KEY_FORMAT_EPNAME, i);
        if (ret < 0) {
            CmiAbort("OFI::LrtsInit::snprintf error");
        }

        ret = runtime_kvs_get(key, epnames+(i*epnamelen), epnamelen, i);
        if (ret) {
            CmiAbort("OFI::LrtsInit::runtime_kvs_get error");
        }

    }

    /**
     * Insert all the EP names into the AV.
     */
    ret = fi_av_insert(av, epnames, nnodes, NULL, 0, NULL);
    if (ret < 0) {
        CmiAbort("OFI::LrtsInit::fi_av_insert error");
    }

    free(key);
    free(epnames);

    return 0;
}

//! convenience function to do registration, binding and enabling in one go
// primarily for OFI_CXI to support FI_MR_ENDPOINT, but it has no
// CXI specific dependencies
static int ofi_reg_bind_enable(const void *buf,
			       size_t len, struct fid_mr **mr, OFIContext *context)
{

        uint32_t  requested_key = __sync_fetch_and_add(&(context->mr_counter), 1);
#if CMK_SMP_TRACE_COMMTHREAD
	double startT, endT;
	startT = CmiWallTimer();
#endif
	/* Register new MR */
        int ret = fi_mr_reg(context->domain,        /* In:  domain object */
                        buf,                   /* In:  lower memory address */
                        len,                  /* In:  length */
			MR_ACCESS_PERMISSIONS, /* In:  access permissions */
                        0ULL,                  /* In:  offset (not used) */
                        requested_key,         /* In:  requested key */
                        0ULL,                  /* In:  flags */
                        mr,                   /* Out: memregion object */
                        NULL);                 /* In:  context (not used) */

	if (ret) {
            MACHSTATE1(3, "fi_mr_reg error: %d\n", ret);
	    char errstring[100];
	    snprintf(errstring, 100, "fi_mr_reg error: %d", ret);
            CmiAbort(errstring);
        }
	else{
	  MACHSTATE3(3, "fi_mr_reg success: %d buf %p mr %lu\n", ret, buf, fi_mr_key(*mr));
	}
	ret = fi_mr_bind(*mr, (struct fid *)context->ep, 0);
	if (ret) {
            MACHSTATE1(3, "fi_mr_bind error: %d\n", ret);
	    char errstring[100];
	    snprintf(errstring, 100, "fi_mr_bind error: %d", ret);
            CmiAbort(errstring);
        }
	else
	  {
	    MACHSTATE3(3, "fi_mr_bind success: %d ep %p mr %lu\n", ret, context->ep, fi_mr_key(*mr));
	  }

	ret = fi_mr_enable(*mr);
	if (ret) {
            MACHSTATE1(3, "fi_mr_enable error: %d\n", ret);
	    char errstring[100];
	    snprintf(errstring, 100, "fi_mr_enable error: %d", ret);
            CmiAbort(errstring);
        }
	else
	  {
	    MACHSTATE2(3, "fi_mr_enable success: %d mr %lu\n", ret, fi_mr_key(*mr));
	  }
#if CMK_SMP_TRACE_COMMTHREAD
	endT = CmiWallTimer();
	if (postInit==1 && ((endT-startT>=TRACE_THRESHOLD))) traceUserBracketEvent(event_reg_bind_enable, startT, endT);
#endif
	return(ret);
}

INLINE_KEYWORD void LrtsPrepareEnvelope(char *msg, int size)
{
    CmiSetMsgSize(msg, size);
    //    CMI_SET_CHECKSUM(msg, size);
}

#if CMK_ONESIDED_IMPL
#include "machine-onesided.C"
#endif

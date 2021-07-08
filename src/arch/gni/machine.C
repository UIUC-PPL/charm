
/** @file
 * GNI machine layer
 *
 * Author:   Yanhua Sun
             Gengbin Zheng
 * Date:   07-01-2011
 *
 *  Flow control by mem pool using environment variables:

    # CHARM_UGNI_MEMPOOL_MAX can be maximum_register_mem/number_of_processes
    # CHARM_UGNI_SEND_MAX can be half of CHARM_UGNI_MEMPOOL_MAX
    export CHARM_UGNI_MEMPOOL_INIT_SIZE=8M
    export CHARM_UGNI_MEMPOOL_MAX=20M
    export CHARM_UGNI_SEND_MAX=10M

    # limit on total mempool size allocated, this is to prevent mempool
    # uses too much memory
    export CHARM_UGNI_MEMPOOL_SIZE_LIMIT=512M

    other environment variables:

    export CHARM_UGNI_NO_DEADLOCK_CHECK=yes    # disable checking deadlock
    export CHARM_UGNI_MAX_MEMORY_ON_NODE=0.8G  # max memory per node for mempool
    export CHARM_UGNI_BIG_MSG_SIZE=4M          # set big message size protocol
    export CHARM_UGNI_BIG_MSG_PIPELINE_LEN=4   # set big message pipe len
    export CHARM_UGNI_RDMA_MAX=100             # max pending RDMA operations
 */
/*@{*/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <malloc.h>
#include <unistd.h>
#include <time.h>
#include <sys/dir.h>
#include <sys/stat.h>
#include <gni_pub.h>
#include <pmi.h>
#include <algorithm>
//#include <numatoolkit.h>

#include "converse.h"
#include "cmirdmautils.h"

#if REGULARPAGE
#define     LARGEPAGE              0
#else
#define     LARGEPAGE              1
#endif

#if CMK_SMP
#define MULTI_THREAD_SEND          0
#define COMM_THREAD_SEND           (!MULTI_THREAD_SEND)
#endif

#if MULTI_THREAD_SEND
#define CMK_WORKER_SINGLE_TASK     1
#endif

#define REMOTE_EVENT               1

#define CQWRITE                    0

#define CMI_EXERT_SEND_LARGE_CAP   0
#define CMI_EXERT_RECV_RDMA_CAP    0


#define CMI_SENDBUFFERSMSG_CAP            0
#define CMI_PUMPNETWORKSMSG_CAP           0
#define CMI_PUMPREMOTETRANSACTIONS_CAP    0
#define CMI_PUMPLOCALTRANSACTIONS_CAP     0

#if CMI_SENDBUFFERSMSG_CAP
int     SendBufferMsg_cap  = 20;
#endif

#if CMI_PUMPNETWORKSMSG_CAP
int     PumpNetworkSmsg_cap = 20;
#endif

#if CMI_PUMPREMOTETRANSACTIONS_CAP
int     PumpRemoteTransactions_cap = 20;
#endif

#if CMI_PUMPREMOTETRANSACTIONS_CAP
int     PumpLocalTransactions_cap = 15;
#endif

#if CMI_EXERT_SEND_LARGE_CAP
static int SEND_large_cap = 20;
static int SEND_large_pending = 0;
#endif

#if CMI_EXERT_RECV_RDMA_CAP
static int   RDMA_cap =   10;
static int   RDMA_pending = 0;
#endif

enum CMK_SMSG_TYPE : unsigned char {
  CHARM_SMSG=1,
  NONCHARM_SMSG,
  SMSG_DONT_FREE
};

#define USE_LRTS_MEMPOOL                  1

#define PRINT_SYH                         0

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

#if CMI_MACH_TRACE_USEREVENTS && CMK_TRACE_ENABLED && ! CMK_TRACE_IN_CHARM
CpvStaticDeclare(double, projTraceStart);
#define  START_EVENT()  CpvAccess(projTraceStart) = CmiWallTimer();
#define  END_EVENT(x)   traceUserBracketEvent(x, CpvAccess(projTraceStart), CmiWallTimer());
#define  EVENT_TIME()   CpvAccess(projTraceStart)
#else
#define  START_EVENT()
#define  END_EVENT(x)
#define  EVENT_TIME()   (0.0)
#endif

#if USE_LRTS_MEMPOOL

#define oneMB (1024ll*1024)
#define oneGB (1024ll*1024*1024)

static CmiInt8 _mempool_size = 8*oneMB;
static CmiInt8 _expand_mem =  4*oneMB;
static CmiInt8 _mempool_size_limit = 0;

static CmiInt8 _totalmem = 0.8*oneGB;

#if LARGEPAGE
static CmiInt8 BIG_MSG  =  16*oneMB;
static CmiInt8 ONE_SEG  =  4*oneMB;
#else
static CmiInt8 BIG_MSG  =  4*oneMB;
static CmiInt8 ONE_SEG  =  2*oneMB;
#endif
#if MULTI_THREAD_SEND
static int BIG_MSG_PIPELINE = 1;
#else
static int BIG_MSG_PIPELINE = 4;
#endif

// dynamic flow control
static CmiInt8 buffered_send_msg = 0;
static CmiInt8 register_memory_size = 0;

#if LARGEPAGE
static CmiInt8  MAX_BUFF_SEND  =  100000*oneMB;
static CmiInt8  MAX_REG_MEM    =  200000*oneMB;
static CmiInt8	register_count = 0;
#else
#if CMK_SMP && COMM_THREAD_SEND
static CmiInt8  MAX_BUFF_SEND  =  100*oneMB;
static CmiInt8  MAX_REG_MEM    =  200*oneMB;
#else
static CmiInt8  MAX_BUFF_SEND  =  16*oneMB;
static CmiInt8  MAX_REG_MEM    =  25*oneMB;
#endif


#endif

#endif     /* end USE_LRTS_MEMPOOL */

#if MULTI_THREAD_SEND
#define     CMI_GNI_LOCK(x)       CmiLock(x);
#define     CMI_GNI_TRYLOCK(x)       CmiTryLock(x)
#define     CMI_GNI_UNLOCK(x)        CmiUnlock(x);
#define     CMI_PCQUEUEPOP_LOCK(Q)   CmiLock((Q)->lock);
#define     CMI_PCQUEUEPOP_UNLOCK(Q)    CmiUnlock((Q)->lock);
#else
#define     CMI_GNI_LOCK(x)
#define     CMI_GNI_TRYLOCK(x)         (0)
#define     CMI_GNI_UNLOCK(x)
#define     CMI_PCQUEUEPOP_LOCK(Q)
#define     CMI_PCQUEUEPOP_UNLOCK(Q)
#endif

static int _tlbpagesize = 4096;

//static int _smpd_count  = 0;

static int   user_set_flag  = 0;

static int _checkProgress = 1;             /* check deadlock */
static int _detected_hang = 0;

#define             SMSG_ATTR_SIZE      sizeof(gni_smsg_attr_t)

// dynamic SMSG
static int useDynamicSMSG = 0;               /* dynamic smsgs setup */

static int avg_smsg_connection = 32;
static int                 *smsg_connected_flag= 0;
static gni_smsg_attr_t     **smsg_attr_vector_local;
static gni_smsg_attr_t     **smsg_attr_vector_remote;
static gni_ep_handle_t     ep_hndl_unbound;
static gni_smsg_attr_t     send_smsg_attr;
static gni_smsg_attr_t     recv_smsg_attr;

typedef struct _dynamic_smsg_mailbox{
   void     *mailbox_base;
   int      size;
   int      offset;
   gni_mem_handle_t  mem_hndl;
   struct      _dynamic_smsg_mailbox  *next;
}dynamic_smsg_mailbox_t;

static dynamic_smsg_mailbox_t  *mailbox_list;

static CmiUInt8  smsg_send_count = 0,  last_smsg_send_count = 0;
static CmiUInt8  smsg_recv_count = 0,  last_smsg_recv_count = 0;

#if PRINT_SYH
int         lrts_send_msg_id = 0;
int         lrts_local_done_msg = 0;
int         lrts_send_rdma_success = 0;
#endif

#include "machine.h"

#include "pcqueue.h"

#include "mempool.h"

#if CMK_PERSISTENT_COMM
#define PERSISTENT_GET_BASE 0
#if !PERSISTENT_GET_BASE
#define CMK_PERSISTENT_COMM_PUT 1
#endif
#include "machine-persistent.h"
#define  POST_HIGHPRIORITY_RDMA    STATS_SENDRDMAMSG_TIME(SendRdmaMsg(sendHighPriorBuf));
#else
#define  POST_HIGHPRIORITY_RDMA
#endif

#if REMOTE_EVENT && (CMK_USE_OOB || CMK_PERSISTENT_COMM_PUT)
#define  PUMP_REMOTE_HIGHPRIORITY    STATS_PUMPREMOTETRANSACTIONS_TIME(PumpRemoteTransactions(highpriority_rx_cqh) );
#else
#define  PUMP_REMOTE_HIGHPRIORITY
#endif

//#define  USE_ONESIDED 1
#ifdef USE_ONESIDED
//onesided implementation is wrong, since no place to restore omdh
#include "onesided.h"
onesided_hnd_t   onesided_hnd;
onesided_md_t    omdh;
#define MEMORY_REGISTER(handler, nic_hndl, msg, size, mem_hndl, myomdh)  omdh. onesided_mem_register(handler, (uint64_t)msg, size, 0, myomdh)

#define MEMORY_DEREGISTER(handler, nic_hndl, mem_hndl, myomdh) onesided_mem_deregister(handler, myomdh)

#else
uint8_t   onesided_hnd, omdh;

#if REMOTE_EVENT || CQWRITE
#define  MEMORY_REGISTER(handler, nic_hndl, msg, size, mem_hndl, myomdhh, cqh, status) \
    if(register_memory_size+size>= MAX_REG_MEM) { \
        status = GNI_RC_ERROR_NOMEM;} \
    else {status = GNI_MemRegister(nic_hndl, (uint64_t)msg,  (uint64_t)size, cqh,  GNI_MEM_READWRITE, -1, mem_hndl); \
        if(status == GNI_RC_SUCCESS) register_memory_size += size; }
#else
#define  MEMORY_REGISTER(handler, nic_hndl, msg, size, mem_hndl, myomdh, cqh, status ) \
        if (register_memory_size + size >= MAX_REG_MEM) { \
            status = GNI_RC_ERROR_NOMEM; \
        } else { status = GNI_MemRegister(nic_hndl, (uint64_t)msg,  (uint64_t)size, NULL,  GNI_MEM_READWRITE, -1, mem_hndl); \
            if(status == GNI_RC_SUCCESS) register_memory_size += size; }
#endif

#define  MEMORY_DEREGISTER(handler, nic_hndl, mem_hndl, myomdh, size)  \
    do { if (GNI_MemDeregister(nic_hndl, (mem_hndl) ) == GNI_RC_SUCCESS) \
             register_memory_size -= size; \
         else CmiAbort("MEM_DEregister");  \
    } while (0)
#endif

#define   GetMempoolBlockPtr(x)   MEMPOOL_GetBlockPtr(MEMPOOL_GetMempoolHeader(x,ALIGNBUF))
#define   GetMempoolPtr(x)        MEMPOOL_GetMempoolPtr(MEMPOOL_GetMempoolHeader(x,ALIGNBUF))
#define   GetMempoolsize(x)       MEMPOOL_GetSize(MEMPOOL_GetMempoolHeader(x,ALIGNBUF))
#define   GetMemHndl(x)           MEMPOOL_GetMemHndl(MEMPOOL_GetMempoolHeader(x,ALIGNBUF))
#define   IncreaseMsgInRecv(x)    MEMPOOL_IncMsgInRecv(MEMPOOL_GetMempoolHeader(x,ALIGNBUF))
#define   DecreaseMsgInRecv(x)    MEMPOOL_DecMsgInRecv(MEMPOOL_GetMempoolHeader(x,ALIGNBUF))
#define   IncreaseMsgInSend(x)    MEMPOOL_IncMsgInSend(MEMPOOL_GetMempoolHeader(x,ALIGNBUF))
#define   DecreaseMsgInSend(x)    MEMPOOL_DecMsgInSend(MEMPOOL_GetMempoolHeader(x,ALIGNBUF))
#define   NoMsgInSend(x)          MEMPOOL_GetMsgInSend(MEMPOOL_GetMempoolHeader(x,ALIGNBUF)) == 0
#define   NoMsgInRecv(x)          MEMPOOL_GetMsgInRecv(MEMPOOL_GetMempoolHeader(x,ALIGNBUF)) == 0
#define   NoMsgInFlight(x)        (NoMsgInSend(x) && NoMsgInRecv(x))
#define   IsMemHndlZero(x)        ((x).qword1 == 0 && (x).qword2 == 0)
#define   SetMemHndlZero(x)       do {(x).qword1 = 0;(x).qword2 = 0;} while (0)
#define   NotRegistered(x)        IsMemHndlZero(GetMemHndl(x))

#define   GetMemHndlFromBlockHeader(x) MEMPOOL_GetBlockMemHndl(x)
#define   GetSizeFromBlockHeader(x)    MEMPOOL_GetBlockSize(x)

#define CmiGetMsgSize(m)     ((CmiMsgHeaderExt*)m)->size
#define CmiSetMsgSize(m,s)   ((((CmiMsgHeaderExt*)m)->size)=(s))
#define CmiGetMsgSeq(m)      ((CmiMsgHeaderExt*)m)->seq
#define CmiSetMsgSeq(m, s)   ((((CmiMsgHeaderExt*)m)->seq) = (s))

#define ALIGNBUF                64

/* =======Beginning of Definitions of Performance-Specific Macros =======*/
/* If SMSG is not used */

#define FMA_PER_CORE  1024
#define FMA_BUFFER_SIZE 1024

/* If SMSG is used */
static int  SMSG_MAX_MSG = 1024;
#define SMSG_MAX_CREDIT    72

#define MSGQ_MAXSIZE       2048

/* large message transfer with FMA or BTE */
#if ! REMOTE_EVENT
#define LRTS_GNI_RDMA_THRESHOLD  1024
#else
   /* remote events only work with RDMA */
#define LRTS_GNI_RDMA_THRESHOLD  0
#endif

#if CMK_SMP
static int  REMOTE_QUEUE_ENTRIES=163840;
static int LOCAL_QUEUE_ENTRIES=163840;
#else
static int  REMOTE_QUEUE_ENTRIES=20480;
static int LOCAL_QUEUE_ENTRIES=20480;
#endif

#define BIG_MSG_TAG             0x26
#define PUT_DONE_TAG            0x28
#define ACK_TAG                 0x30
/* SMSG is data message */
#define SMALL_DATA_TAG          0x31
/* SMSG is a control message to initialize a BTE */
#define LMSG_INIT_TAG           0x33
#define LMSG_PERSISTENT_INIT_TAG           0x34
#define LMSG_OOB_INIT_TAG       0x35

#define RDMA_PUT_MD_DIRECT_TAG  0x36
#define RDMA_PUT_DONE_DIRECT_TAG 0x37
#define RDMA_DEREG_DIRECT_TAG    0x38

#define RDMA_REG_AND_PUT_MD_DIRECT_TAG  0x39
#define RDMA_REG_AND_GET_MD_DIRECT_TAG  0x40
#define RDMA_DEREG_AND_ACK_MD_DIRECT_TAG  0x41

#if CMK_SMP
#define RDMA_COMM_PERFORM_GET_TAG     0x42
#define RDMA_COMM_PERFORM_PUT_TAG     0x43
#endif

#define DEBUG
#ifdef GNI_RC_CHECK
#undef GNI_RC_CHECK
#endif
#ifdef DEBUG
#define GNI_RC_CHECK(msg,rc) do { if(rc != GNI_RC_SUCCESS) {           printf("[%d] %s; err=%s\n",CmiMyPe(),msg,gni_err_str[rc]); fflush(stdout); CmiAbort("GNI_RC_CHECK"); } } while(0)
#else
#define GNI_RC_CHECK(msg,rc)
#endif

#define ALIGN64(x)       (size_t)((~63)&((x)+63))
//#define ALIGN4(x)        (size_t)((~3)&((x)+3))
#define ALIGNHUGEPAGE(x)   (size_t)((~(_tlbpagesize-1))&((x)+_tlbpagesize-1))

static int useStaticMSGQ = 0;
static int useStaticFMA = 0;
static int mysize, myrank;
static gni_nic_handle_t   nic_hndl;

typedef struct {
    gni_mem_handle_t mdh;
    uint64_t addr;
} mdh_addr_t ;
// this is related to dynamic SMSG

typedef struct mdh_addr_list{
    gni_mem_handle_t mdh;
   void *addr;
    struct mdh_addr_list *next;
}mdh_addr_list_t;

static unsigned int         smsg_memlen;
gni_smsg_attr_t    **smsg_local_attr_vec = 0;
mdh_addr_t          setup_mem;
mdh_addr_t          *smsg_connection_vec = 0;
gni_mem_handle_t    smsg_connection_memhndl;
static int          smsg_expand_slots = 10;
static int          smsg_available_slot = 0;
static void         *smsg_mailbox_mempool = 0;
mdh_addr_list_t     *smsg_dynamic_list = 0;

static void             *smsg_mailbox_base;
gni_msgq_attr_t         msgq_attrs;
gni_msgq_handle_t       msgq_handle;
gni_msgq_ep_attr_t      msgq_ep_attrs;
gni_msgq_ep_attr_t      msgq_ep_attrs_size;

/* =====Beginning of Declarations of Machine Specific Variables===== */
static int cookie;
static int modes = 0;
static gni_cq_handle_t       smsg_rx_cqh = NULL;      // smsg send
static gni_cq_handle_t       default_tx_cqh = NULL;   // bind to endpoint
static gni_cq_handle_t       rdma_tx_cqh = NULL;      // rdma - local event
static gni_cq_handle_t       highprior_rdma_tx_cqh = NULL;      // rdma - local event
static gni_cq_handle_t       rdma_rx_cqh = NULL;      // mempool - remote event
static gni_cq_handle_t       highpriority_rx_cqh = NULL;      // mempool - remote event
static gni_ep_handle_t       *ep_hndl_array;

static CmiNodeLock           *ep_lock_array;
static CmiNodeLock           default_tx_cq_lock;
static CmiNodeLock           rdma_tx_cq_lock;
static CmiNodeLock           global_gni_lock;
static CmiNodeLock           rx_cq_lock;
static CmiNodeLock           smsg_mailbox_lock;
static CmiNodeLock           smsg_rx_cq_lock;
static CmiNodeLock           *mempool_lock;

#if CMK_ONESIDED_IMPL
static gni_cq_handle_t       rdma_onesided_cqh = NULL;
static CmiNodeLock           rdma_onesided_cq_lock;
#endif

//#define     CMK_WITH_STATS      1
typedef struct msg_list
{
    uint32_t destNode;
    uint32_t size;
    void *msg;
    uint8_t tag;
#if CMK_WITH_STATS
    double  creation_time;
#endif
}MSG_LIST;


typedef struct control_msg
{
    uint64_t            source_addr;    /* address from the start of buffer  */
    uint64_t            dest_addr;      /* address from the start of buffer */
    int                 total_length;   /* total length */
    int                 length;         /* length of this packet */
#if REMOTE_EVENT
    int                 ack_index;      /* index from integer to address */
#endif
    int     seq_id;         //big message   0 meaning single message
    gni_mem_handle_t    source_mem_hndl;
#if PERSISTENT_GET_BASE
    gni_mem_handle_t    dest_mem_hndl;
#endif
    struct control_msg  *next;
} CONTROL_MSG;

#define CONTROL_MSG_SIZE       (sizeof(CONTROL_MSG)-sizeof(void*))

typedef struct ack_msg
{
    uint64_t            source_addr;    /* address from the start of buffer  */
#if ! USE_LRTS_MEMPOOL
    gni_mem_handle_t    source_mem_hndl;
    int                 length;          /* total length */
#endif
    struct ack_msg     *next;
} ACK_MSG;

#define ACK_MSG_SIZE       (sizeof(ACK_MSG)-sizeof(void*))

typedef struct  rmda_msg
{
    int                   destNode;
#if REMOTE_EVENT
    int                   ack_index;
#endif
    gni_post_descriptor_t *pd;
}RDMA_REQUEST;


#if CMK_SMP
#define SMP_LOCKS                       1
#else
#define SMP_LOCKS                       0
#endif
#if CMK_LOCKLESS_QUEUE
#define ONE_SEND_QUEUE                  1
#else
#define ONE_SEND_QUEUE                  0
#endif

typedef PCQueue BufferList;
typedef struct  msg_list_index
{
    PCQueue       sendSmsgBuf;
#if  SMP_LOCKS
    CmiNodeLock   lock;
    int           pushed;
    int           destpe;
#endif
} MSG_LIST_INDEX;
char                *destpe_avail;
PCQueue sendRdmaBuf;
PCQueue sendHighPriorBuf;
// buffered send queue
#if ! ONE_SEND_QUEUE
typedef struct smsg_queue
{
    MSG_LIST_INDEX   *smsg_msglist_index;
    int               smsg_head_index;
#if  SMP_LOCKS
    PCQueue     nonEmptyQueues;
#endif
} SMSG_QUEUE;
#else
typedef struct smsg_queue
{
#if CMK_LOCKLESS_QUEUE
    MPMCQueue       sendMsgBuf;
#else
    PCQueue       sendMsgBuf;
#endif
}  SMSG_QUEUE;
#endif

SMSG_QUEUE                  smsg_queue;
#if CMK_USE_OOB
SMSG_QUEUE                  smsg_oob_queue;
#define SEND_OOB_SMSG(x)            SendBufferMsg(&x, NULL);
#define PUMP_LOCAL_HIGHPRIORITY    STATS_PUMPLOCALTRANSACTIONS_RDMA_TIME(PumpLocalTransactions(highprior_rdma_tx_cqh,  rdma_tx_cq_lock));
#else
#define SEND_OOB_SMSG(x)
#define PUMP_LOCAL_HIGHPRIORITY
#endif

#define FreeMsgList(d)   free(d);
#define MallocMsgList(d)  d = ((MSG_LIST*)malloc(sizeof(MSG_LIST)));

#define FreeControlMsg(d)      free(d);
#define MallocControlMsg(d)    d = ((CONTROL_MSG*)malloc(sizeof(CONTROL_MSG)));

#define FreeAckMsg(d)      free(d);
#define MallocAckMsg(d)    d = ((ACK_MSG*)malloc(sizeof(ACK_MSG)));

#define FreeRdmaRequest(d)       free(d);
#define MallocRdmaRequest(d)     d = ((RDMA_REQUEST*)malloc(sizeof(RDMA_REQUEST)));
/* reuse gni_post_descriptor_t */
static gni_post_descriptor_t *post_freelist=0;

#define FreePostDesc(d)     free(d);
#define MallocPostDesc(d)   d = ((gni_post_descriptor_t*)malloc(sizeof(gni_post_descriptor_t))); _MEMCHECK(d);


/* LrtsSent is called but message can not be sent by SMSGSend because of mailbox full or no credit */
static int      buffered_smsg_counter = 0;

/* SmsgSend return success but message sent is not confirmed by remote side */
static MSG_LIST *buffered_fma_head = 0;
static MSG_LIST *buffered_fma_tail = 0;

/* functions  */
#define IsFree(a,ind)  !( a& (1<<(ind) ))
#define SET_BITS(a,ind) a = ( a | (1<<(ind )) )
#define Reset(a,ind) a = ( a & (~(1<<(ind))) )

CpvDeclare(mempool_type*, mempool);

#if CMK_PERSISTENT_COMM_PUT
CpvDeclare(mempool_type*, persistent_mempool);
#endif

#if REMOTE_EVENT || CMK_SMSGS_FREE_AFTER_EVENT
struct IndexStruct {
void *addr;
int next;
int type;
};

typedef struct IndexPool {
    struct IndexStruct   *indexes;
    int                   size;
    int                   freehead;
    CmiNodeLock           lock;
    int                   maxsize;
} IndexPool;

#define  GetIndexType(pool, s)             (pool.indexes[s].type)
#define  GetIndexAddress(pool, s)          (pool.indexes[s].addr)

static void IndexPool_init(IndexPool *pool, int initsize, int maxsize)
{
    int i;
    pool->size = initsize;
    pool->maxsize = maxsize;
    pool->indexes = (struct IndexStruct *)malloc(pool->size*sizeof(struct IndexStruct));
    if(pool->indexes == NULL) {
        CmiAbort("malloc of pool is null\n");
    }
    for (i=0; i<pool->size-1; i++) {
        pool->indexes[i].next = i+1;
    }
    pool->indexes[i].next = -1;
    pool->freehead = 0;
#if MULTI_THREAD_SEND || CMK_PERSISTENT_COMM_PUT
    pool->lock  = CmiCreateLock();
#else
    pool->lock  = 0;
#endif
}

static int IndexPool_getslot(IndexPool *pool, void *addr, int type)
{
    int s, i;
#if MULTI_THREAD_SEND
    CmiLock(pool->lock);
#endif
    s = pool->freehead;
    if (s == -1) {
        int newsize = pool->size * 2;
        //printf("[%d] IndexPool_getslot %p expand to: %d\n", myrank, pool, newsize);
        if (newsize > pool->maxsize) {
            static int indexpool_overflow = 0;
            if(!indexpool_overflow){
                printf("[%d] Warning: IndexPool_getslot %p overflow when expanding to: %d\n", myrank, pool, newsize);
                indexpool_overflow = 1;
            }
            return -1;
        }
        struct IndexStruct *old_ackpool = pool->indexes;
        pool->indexes = (struct IndexStruct *)malloc(newsize*sizeof(struct IndexStruct));
        if(pool->indexes == NULL) {
            CmiAbort("malloc of pool is null\n");
        }
        memcpy(pool->indexes, old_ackpool, pool->size*sizeof(struct IndexStruct));
        for (i=pool->size; i<newsize-1; i++) {
            pool->indexes[i].next = i+1;
        }
        pool->indexes[i].next = -1;
        pool->freehead = pool->size;
        s = pool->size;
        pool->size = newsize;
        free(old_ackpool);
    }
    pool->freehead = pool->indexes[s].next;
    pool->indexes[s].addr = addr;
    pool->indexes[s].type = type;
#if MULTI_THREAD_SEND
    CmiUnlock(pool->lock);
#endif
    return s;
}

static void IndexPool_freeslot(IndexPool *pool, int s)
{
    CmiAssert(s>=0 && s<pool->size);
#if MULTI_THREAD_SEND
    CmiLock(pool->lock);
#endif
    pool->indexes[s].next = pool->freehead;
    pool->freehead = s;
#if MULTI_THREAD_SEND
    CmiUnlock(pool->lock);
#endif
}

#endif

#if CMK_SMSGS_FREE_AFTER_EVENT
/*
  SMSGS pool
  the pool is to buffer sending smsgs until it can be free'ed .
*/
static IndexPool smsgsPool;
#endif

#if REMOTE_EVENT
/* ack pool for remote events */

static int  SHIFT   =           18;
#define INDEX_MASK              ((1<<(32-SHIFT-1)) - 1)
#define RANK_MASK               ((1<<SHIFT) - 1)
#define ACK_EVENT(idx)          ((((idx) & INDEX_MASK)<<SHIFT) | myrank)

#define GET_TYPE(evt)           (((evt) >> 31) & 1)
#define GET_RANK(evt)           ((evt) & RANK_MASK)
#define GET_INDEX(evt)          (((evt) >> SHIFT) & INDEX_MASK)

#define PERSIST_EVENT(idx)      ( (1<<31) | (((idx) & INDEX_MASK)<<SHIFT) | myrank)
#define DIRECT_EVENT(idx)      ( (1<<31) | (((idx) & INDEX_MASK)<<SHIFT) | myrank)

#if CMK_SMP
#define POOL_INIT_SIZE                4096
#else
#define POOL_INIT_SIZE                1024
#endif

static IndexPool  ackPool;
#if CMK_PERSISTENT_COMM_PUT
static IndexPool  persistPool;
#else
#define persistPool ackPool
#endif

#endif



/* =====Beginning of Definitions of Message-Corruption Related Macros=====*/
#define CMI_MAGIC(msg)                   ((CmiMsgHeaderBasic *)msg)->magic
#define CHARM_MAGIC_NUMBER               126

#if CMK_ERROR_CHECKING
unsigned char computeCheckSum(unsigned char *data, int len);
static int checksum_flag = 0;
#define CMI_SET_CHECKSUM(msg, len)      \
        if (checksum_flag)  {   \
          ((CmiMsgHeaderBasic *)msg)->cksum = 0;        \
          ((CmiMsgHeaderBasic *)msg)->cksum = computeCheckSum((unsigned char*)msg, len);        \
        }
#define CMI_CHECK_CHECKSUM(msg, len)    \
        if (checksum_flag)      \
          if (computeCheckSum((unsigned char*)msg, len) != 0)   \
            CmiAbort("Fatal error: checksum doesn't agree!\n");
#else
#define CMI_SET_CHECKSUM(msg, len)
#define CMI_CHECK_CHECKSUM(msg, len)
#endif
/* =====End of Definitions of Message-Corruption Related Macros=====*/

static int print_stats = 0;
static int stats_off = 0;
void CmiTurnOnStats(void)
{
    stats_off = 0;
    //CmiPrintf("[%d][%d:%d]+++++++++++ turning on stats \n", CmiMyNode(), CmiMyPe(), CmiMyRank());
}

void CmiTurnOffStats(void)
{
    stats_off = 1;
}

#define IS_PUT(type)    (type == GNI_POST_FMA_PUT || type == GNI_POST_RDMA_PUT)

#if CMK_WITH_STATS
FILE *counterLog = NULL;
typedef struct comm_thread_stats
{
    uint64_t  smsg_data_count;
    uint64_t  lmsg_init_count;
    uint64_t  ack_count;
    uint64_t  big_msg_ack_count;
    uint64_t  smsg_count;
    uint64_t  put_done_count;
    //times of calling SmsgSend
    uint64_t  try_smsg_data_count;
    uint64_t  try_lmsg_init_count;
    uint64_t  try_ack_count;
    uint64_t  try_big_msg_ack_count;
    uint64_t  try_put_done_count;
    uint64_t  try_smsg_count;

    double    max_time_in_send_buffered_smsg;
    double    all_time_in_send_buffered_smsg;

    uint64_t  rdma_get_count, rdma_put_count;
    uint64_t  try_rdma_get_count, try_rdma_put_count;
    double    max_time_from_control_to_rdma_init;
    double    all_time_from_control_to_rdma_init;

    double    max_time_from_rdma_init_to_rdma_done;
    double    all_time_from_rdma_init_to_rdma_done;

    int      count_in_PumpNetwork;
    double   time_in_PumpNetwork;
    double   max_time_in_PumpNetwork;
    int      count_in_SendBufferMsg_smsg;
    double   time_in_SendBufferMsg_smsg;
    double   max_time_in_SendBufferMsg_smsg;
    int      count_in_SendRdmaMsg;
    double   time_in_SendRdmaMsg;
    double   max_time_in_SendRdmaMsg;
    int      count_in_PumpRemoteTransactions;
    double   time_in_PumpRemoteTransactions;
    double   max_time_in_PumpRemoteTransactions;
    int      count_in_PumpLocalTransactions_rdma;
    double   time_in_PumpLocalTransactions_rdma;
    double   max_time_in_PumpLocalTransactions_rdma;
    int      count_in_PumpDatagramConnection;
    double   time_in_PumpDatagramConnection;
    double   max_time_in_PumpDatagramConnection;
} Comm_Thread_Stats;

static Comm_Thread_Stats   comm_stats;

static char *counters_dirname = "counters";

static void init_comm_stats(void)
{
  memset(&comm_stats, 0, sizeof(Comm_Thread_Stats));
  if (print_stats){
      char ln[200];
      int code = mkdir(counters_dirname, 00777);
      sprintf(ln,"%s/statistics.%d.%d", counters_dirname, mysize, myrank);
      counterLog=fopen(ln,"w");
      if (counterLog == NULL) CmiAbort("Counter files open failed");
  }
}

#define SMSG_CREATION( x ) if(print_stats) { x->creation_time = CmiWallTimer(); }

#define SMSG_SENT_DONE(creation_time, tag)  \
        if (print_stats && !stats_off) {   if( tag == SMALL_DATA_TAG) comm_stats.smsg_data_count++;  \
            else  if( tag == LMSG_INIT_TAG || tag == LMSG_OOB_INIT_TAG) comm_stats.lmsg_init_count++;  \
            else  if( tag == ACK_TAG) comm_stats.ack_count++;  \
            else  if( tag == BIG_MSG_TAG) comm_stats.big_msg_ack_count++;  \
            else  if( tag == PUT_DONE_TAG ) comm_stats.put_done_count++;  \
            comm_stats.smsg_count++; \
            double inbuff_time = CmiWallTimer() - creation_time;   \
            if(inbuff_time > comm_stats.max_time_in_send_buffered_smsg) comm_stats.max_time_in_send_buffered_smsg= inbuff_time; \
            comm_stats.all_time_in_send_buffered_smsg += inbuff_time;  \
        }

#define SMSG_TRY_SEND(tag)  \
        if (print_stats && !stats_off){   if( tag == SMALL_DATA_TAG) comm_stats.try_smsg_data_count++;  \
            else  if( tag == LMSG_INIT_TAG || tag == LMSG_OOB_INIT_TAG) comm_stats.try_lmsg_init_count++;  \
            else  if( tag == ACK_TAG) comm_stats.try_ack_count++;  \
            else  if( tag == BIG_MSG_TAG) comm_stats.try_big_msg_ack_count++;  \
            else  if( tag == PUT_DONE_TAG ) comm_stats.try_put_done_count++;  \
            comm_stats.try_smsg_count++; \
         }

#define  RDMA_TRY_SEND(type)        if (print_stats && !stats_off) {IS_PUT(type)?comm_stats.try_rdma_put_count++:comm_stats.try_rdma_get_count++;}

#define  RDMA_TRANS_DONE(x)      \
         if (print_stats && !stats_off) {  double rdma_trans_time = CmiWallTimer() - x ; \
             if(rdma_trans_time > comm_stats.max_time_from_rdma_init_to_rdma_done) comm_stats.max_time_from_rdma_init_to_rdma_done = rdma_trans_time; \
             comm_stats.all_time_from_rdma_init_to_rdma_done += rdma_trans_time; \
         }

#define  RDMA_TRANS_INIT(type, x)      \
         if (print_stats && !stats_off) {   IS_PUT(type)?comm_stats.rdma_put_count++:comm_stats.rdma_get_count++;  \
             double rdma_trans_time = CmiWallTimer() - x ; \
             if(rdma_trans_time > comm_stats.max_time_from_control_to_rdma_init) comm_stats.max_time_from_control_to_rdma_init = rdma_trans_time; \
             comm_stats.all_time_from_control_to_rdma_init += rdma_trans_time; \
         }

#define STATS_PUMPNETWORK_TIME(x)   \
        { double t = CmiWallTimer(); \
          x;        \
          t = CmiWallTimer() - t;          \
          comm_stats.count_in_PumpNetwork++;        \
          comm_stats.time_in_PumpNetwork += t;   \
          if (t>comm_stats.max_time_in_PumpNetwork)      \
              comm_stats.max_time_in_PumpNetwork = t;    \
        }

#define STATS_PUMPREMOTETRANSACTIONS_TIME(x)   \
        { double t = CmiWallTimer(); \
          x;        \
          t = CmiWallTimer() - t;          \
          comm_stats.count_in_PumpRemoteTransactions ++;        \
          comm_stats.time_in_PumpRemoteTransactions += t;   \
          if (t>comm_stats.max_time_in_PumpRemoteTransactions)      \
              comm_stats.max_time_in_PumpRemoteTransactions = t;    \
        }

#define STATS_PUMPLOCALTRANSACTIONS_RDMA_TIME(x)   \
        { double t = CmiWallTimer(); \
          x;        \
          t = CmiWallTimer() - t;          \
          comm_stats.count_in_PumpLocalTransactions_rdma ++;        \
          comm_stats.time_in_PumpLocalTransactions_rdma += t;   \
          if (t>comm_stats.max_time_in_PumpLocalTransactions_rdma)      \
              comm_stats.max_time_in_PumpLocalTransactions_rdma = t;    \
        }

#define STATS_SEND_SMSGS_TIME(x)   \
        { double t = CmiWallTimer(); \
          x;        \
          t = CmiWallTimer() - t;          \
          comm_stats.count_in_SendBufferMsg_smsg ++;        \
          comm_stats.time_in_SendBufferMsg_smsg += t;   \
          if (t>comm_stats.max_time_in_SendBufferMsg_smsg)      \
              comm_stats.max_time_in_SendBufferMsg_smsg = t;    \
        }

#define STATS_SENDRDMAMSG_TIME(x)   \
        { double t = CmiWallTimer(); \
          x;        \
          t = CmiWallTimer() - t;          \
          comm_stats.count_in_SendRdmaMsg ++;        \
          comm_stats.time_in_SendRdmaMsg += t;   \
          if (t>comm_stats.max_time_in_SendRdmaMsg)      \
              comm_stats.max_time_in_SendRdmaMsg = t;    \
        }

#define STATS_PUMPDATAGRAMCONNECTION_TIME(x)   \
        { double t = CmiWallTimer(); \
          x;        \
          t = CmiWallTimer() - t;          \
          comm_stats.count_in_PumpDatagramConnection ++;        \
          comm_stats.time_in_PumpDatagramConnection += t;   \
          if (t>comm_stats.max_time_in_PumpDatagramConnection)      \
              comm_stats.max_time_in_PumpDatagramConnection = t;    \
        }

static void print_comm_stats(void)
{
    fprintf(counterLog, "Node[%d] SMSG time in buffer\t[total:%f\tmax:%f\tAverage:%f](milisecond)\n", myrank, 1000.0*comm_stats.all_time_in_send_buffered_smsg, 1000.0*comm_stats.max_time_in_send_buffered_smsg, 1000.0*comm_stats.all_time_in_send_buffered_smsg/comm_stats.smsg_count);
    fprintf(counterLog, "Node[%d] Smsg  Msgs  \t[Total:%lld\t Data:%lld\t Lmsg_Init:%lld\t ACK:%lld\t BIG_MSG_ACK:%lld Persistent_put_done:%lld]\n", myrank,
            comm_stats.smsg_count, comm_stats.smsg_data_count, comm_stats.lmsg_init_count,
            comm_stats.ack_count, comm_stats.big_msg_ack_count, comm_stats.put_done_count);

    fprintf(counterLog, "Node[%d] SmsgSendCalls\t[Total:%lld\t Data:%lld\t Lmsg_Init:%lld\t ACK:%lld\t BIG_MSG_ACK:%lld Persistent_put_done:%lld]\n\n", myrank,
            comm_stats.try_smsg_count, comm_stats.try_smsg_data_count, comm_stats.try_lmsg_init_count,
            comm_stats.try_ack_count, comm_stats.try_big_msg_ack_count, comm_stats.try_put_done_count);

    fprintf(counterLog, "Node[%d] Rdma Transaction [count (GET/PUT):%lld %lld\t calls (GET/PUT):%lld %lld]\n", myrank, comm_stats.rdma_get_count, comm_stats.rdma_put_count, comm_stats.try_rdma_get_count, comm_stats.try_rdma_put_count);
    fprintf(counterLog, "Node[%d] Rdma time from control arrives to rdma init [Total:%f\tMAX:%f\t Average:%f](milisecond)\n", myrank, 1000.0*comm_stats.all_time_from_control_to_rdma_init, 1000.0*comm_stats.max_time_from_control_to_rdma_init, 1000.0*comm_stats.all_time_from_control_to_rdma_init/(comm_stats.rdma_get_count+comm_stats.rdma_put_count));
    fprintf(counterLog, "Node[%d] Rdma time from init to rdma done [Total:%f\tMAX:%f\t Average:%f](milisecond)\n\n", myrank,1000.0*comm_stats.all_time_from_rdma_init_to_rdma_done, 1000.0*comm_stats.max_time_from_rdma_init_to_rdma_done, 1000.0*comm_stats.all_time_from_rdma_init_to_rdma_done/(comm_stats.rdma_get_count+comm_stats.rdma_put_count));


    fprintf(counterLog, "                             count\ttotal(s)\tmax(s)\taverage(us)\n");
    fprintf(counterLog, "PumpNetworkSmsg:              %d\t%.6f\t%.6f\t%.6f\n", comm_stats.count_in_PumpNetwork, comm_stats.time_in_PumpNetwork, comm_stats.max_time_in_PumpNetwork, comm_stats.time_in_PumpNetwork*1e6/comm_stats.count_in_PumpNetwork);
    fprintf(counterLog, "PumpRemoteTransactions:       %d\t%.6f\t%.6f\t%.6f\n", comm_stats.count_in_PumpRemoteTransactions, comm_stats.time_in_PumpRemoteTransactions, comm_stats.max_time_in_PumpRemoteTransactions, comm_stats.time_in_PumpRemoteTransactions*1e6/comm_stats.count_in_PumpRemoteTransactions);
    fprintf(counterLog, "PumpLocalTransactions(RDMA):  %d\t%.6f\t%.6f\t%.6f\n", comm_stats.count_in_PumpLocalTransactions_rdma, comm_stats.time_in_PumpLocalTransactions_rdma, comm_stats.max_time_in_PumpLocalTransactions_rdma, comm_stats.time_in_PumpLocalTransactions_rdma*1e6/comm_stats.count_in_PumpLocalTransactions_rdma);
    fprintf(counterLog, "SendBufferMsg (SMSG):         %d\t%.6f\t%.6f\t%.6f\n",  comm_stats.count_in_SendBufferMsg_smsg, comm_stats.time_in_SendBufferMsg_smsg, comm_stats.max_time_in_SendBufferMsg_smsg, comm_stats.time_in_SendBufferMsg_smsg*1e6/comm_stats.count_in_SendBufferMsg_smsg);
    fprintf(counterLog, "SendRdmaMsg:                  %d\t%.6f\t%.6f\t%.6f\n",  comm_stats.count_in_SendRdmaMsg, comm_stats.time_in_SendRdmaMsg, comm_stats.max_time_in_SendRdmaMsg, comm_stats.time_in_SendRdmaMsg*1e6/comm_stats.count_in_SendRdmaMsg);
    if (useDynamicSMSG)
    fprintf(counterLog, "PumpDatagramConnection:                  %d\t%.6f\t%.6f\t%.6f\n",  comm_stats.count_in_PumpDatagramConnection, comm_stats.time_in_PumpDatagramConnection, comm_stats.max_time_in_PumpDatagramConnection, comm_stats.time_in_PumpDatagramConnection*1e6/comm_stats.count_in_PumpDatagramConnection);

    fclose(counterLog);
}

#else
#define STATS_PUMPNETWORK_TIME(x)                  x
#define STATS_SEND_SMSGS_TIME(x)                   x
#define STATS_PUMPREMOTETRANSACTIONS_TIME(x)       x
#define STATS_PUMPLOCALTRANSACTIONS_RDMA_TIME(x)   x
#define STATS_SENDRDMAMSG_TIME(x)                  x
#define STATS_PUMPDATAGRAMCONNECTION_TIME(x)       x
#endif

static void
allgather(void *in,void *out, int len)
{
    static int *ivec_ptr=NULL,already_called=0,job_size=0;
    int i,rc;
    int my_rank;
    char *tmp_buf,*out_ptr;

    if(!already_called) {

        rc = PMI_Get_size(&job_size);
        CmiAssert(rc == PMI_SUCCESS);
        rc = PMI_Get_rank(&my_rank);
        CmiAssert(rc == PMI_SUCCESS);

        ivec_ptr = (int *)malloc(sizeof(int) * job_size);
        CmiAssert(ivec_ptr != NULL);

        rc = PMI_Allgather(&my_rank,ivec_ptr,sizeof(int));
        CmiAssert(rc == PMI_SUCCESS);

        already_called = 1;

    }

    tmp_buf = (char *)malloc(job_size * len);
    CmiAssert(tmp_buf);

    rc = PMI_Allgather(in,tmp_buf,len);
    CmiAssert(rc == PMI_SUCCESS);

    out_ptr = (char *)out;

    for(i=0;i<job_size;i++) {

        memcpy(&out_ptr[len * ivec_ptr[i]],&tmp_buf[i * len],len);

    }

    free(tmp_buf);
}

static void
allgather_2(void *in,void *out, int len)
{
    //PMI_Allgather is out of order
    int i,rc, extend_len;
    int  rank_index;
    char *out_ptr, *out_ref;
    char *in2;

    extend_len = sizeof(int) + len;
    in2 = (char*)malloc(extend_len);

    memcpy(in2, &myrank, sizeof(int));
    memcpy(in2+sizeof(int), in, len);

    out_ptr = (char*)malloc(mysize*extend_len);

    rc = PMI_Allgather(in2, out_ptr, extend_len);
    GNI_RC_CHECK("allgather", rc);

    out_ref = (char *)out;

    for(i=0;i<mysize;i++) {
        //rank index
        memcpy(&rank_index, &(out_ptr[extend_len*i]), sizeof(int));
        //copy to the rank index slot
        memcpy(&out_ref[rank_index*len], &out_ptr[extend_len*i+sizeof(int)], len);
    }

    free(out_ptr);
    free(in2);

}

static unsigned int get_gni_nic_address(int device_id)
{
    unsigned int address, cpu_id;
    gni_return_t status;
    int i, alps_dev_id=-1,alps_address=-1;
    char *token, *p_ptr;

    p_ptr = getenv("PMI_GNI_DEV_ID");
    if (!p_ptr) {
        status = GNI_CdmGetNicAddress(device_id, &address, &cpu_id);

        GNI_RC_CHECK("GNI_CdmGetNicAddress", status);
    } else {
        while ((token = strtok(p_ptr,":")) != NULL) {
            alps_dev_id = atoi(token);
            if (alps_dev_id == device_id) {
                break;
            }
            p_ptr = NULL;
        }
        CmiAssert(alps_dev_id != -1);
        p_ptr = getenv("PMI_GNI_LOC_ADDR");
        CmiAssert(p_ptr != NULL);
        i = 0;
        while ((token = strtok(p_ptr,":")) != NULL) {
            if (i == alps_dev_id) {
                alps_address = atoi(token);
                break;
            }
            p_ptr = NULL;
            ++i;
        }
        CmiAssert(alps_address != -1);
        address = alps_address;
    }
    return address;
}

static uint8_t get_ptag(void)
{
    char *p_ptr, *token;
    uint8_t ptag;

    p_ptr = getenv("PMI_GNI_PTAG");
    CmiAssert(p_ptr != NULL);
    token = strtok(p_ptr, ":");
    ptag = (uint8_t)atoi(token);
    return ptag;

}

static uint32_t get_cookie(void)
{
    uint32_t cookie;
    char *p_ptr, *token;

    p_ptr = getenv("PMI_GNI_COOKIE");
    CmiAssert(p_ptr != NULL);
    token = strtok(p_ptr, ":");
    cookie = (uint32_t)atoi(token);

    return cookie;
}

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

/* =====Beginning of Definitions of Message-Corruption Related Macros=====*/
/* TODO: add any that are related */
/* =====End of Definitions of Message-Corruption Related Macros=====*/


#include "machine-lrts.h"

#include "machine-common-core.C"

#include "machine-rdma.h"


static int SendBufferMsg(SMSG_QUEUE *queue, SMSG_QUEUE *urgent_queue);
static void SendRdmaMsg(PCQueue );
static void PumpNetworkSmsg(void);
static void PumpLocalTransactions(gni_cq_handle_t tx_cqh, CmiNodeLock cq_lock);
#if CQWRITE
static void PumpCqWriteTransactions(void);
#endif
#if REMOTE_EVENT
static void PumpRemoteTransactions(gni_cq_handle_t);
#endif

#if MACHINE_DEBUG_LOG
static CmiInt8 buffered_recv_msg = 0;
int         lrts_smsg_success = 0;
int         lrts_received_msg = 0;
#endif

static void sweep_mempool(mempool_type *mptr)
{
    int n = 0;
    block_header *current = &(mptr->block_head);

    printf("[n %d %d] sweep_mempool slot START.\n", myrank, n++);
    while( current!= NULL) {
        printf("[n %d %d] sweep_mempool slot %p size: %lld used: %d (%d %d) %lld %lld.\n", myrank, n++, current, current->size, 1<<current->used, current->msgs_in_send, current->msgs_in_recv, current->mem_hndl.qword1, current->mem_hndl.qword2);
        current = current->block_next?(block_header *)((char*)mptr+current->block_next):NULL;
    }
    printf("[n %d] sweep_mempool slot END.\n", myrank);
}


static INLINE_KEYWORD  gni_return_t deregisterMemory(mempool_type *mptr, block_header **from)
{
    block_header *current = *from;

    //while(register_memory_size>= MAX_REG_MEM)
    //{
        while( current!= NULL && ((current->msgs_in_send+current->msgs_in_recv)>0 || IsMemHndlZero(current->mem_hndl) ))
            current = current->block_next?(block_header *)((char*)mptr+current->block_next):NULL;

        *from = current;
        if(current == NULL) return GNI_RC_ERROR_RESOURCE;
        MEMORY_DEREGISTER(onesided_hnd, nic_hndl, &(GetMemHndlFromBlockHeader(current)) , &omdh, GetSizeFromBlockHeader(current));
        SetMemHndlZero(GetMemHndlFromBlockHeader(current));
    //}
    return GNI_RC_SUCCESS;
}

static gni_return_t registerFromMempool(mempool_type *mptr, void *blockaddr, size_t size, gni_mem_handle_t  *memhndl, gni_cq_handle_t cqh )
{
    gni_return_t status = GNI_RC_SUCCESS;
    //int size = GetMempoolsize(msg);
    //void *blockaddr = GetMempoolBlockPtr(msg);
    //gni_mem_handle_t  *memhndl =   &(GetMemHndl(msg));

    block_header *current = &(mptr->block_head);
    while(register_memory_size>= MAX_REG_MEM)
    {
        status = deregisterMemory(mptr, &current);
        if (status != GNI_RC_SUCCESS) break;
    }
    if(register_memory_size>= MAX_REG_MEM) return status;

    MACHSTATE3(8, "mempool (%lld,%lld,%d) \n", buffered_send_msg, buffered_recv_msg, register_memory_size);
    while(1)
    {
        MEMORY_REGISTER(onesided_hnd, nic_hndl, blockaddr, size, memhndl, &omdh, cqh, status);
        if(status == GNI_RC_SUCCESS)
        {
            break;
        }
        else if (status == GNI_RC_INVALID_PARAM || status == GNI_RC_PERMISSION_ERROR)
        {
            GNI_RC_CHECK("registerFromMempool", status);
        }
        else
        {
            status = deregisterMemory(mptr, &current);
            if (status != GNI_RC_SUCCESS) break;
        }
    };
    return status;
}

INLINE_KEYWORD
static gni_return_t registerMemory(void *msg, size_t size, gni_mem_handle_t *t, gni_cq_handle_t cqh )
{
    static int rank = -1;
    int i;
    gni_return_t status;
    mempool_type *mptr1 = CpvAccess(mempool);//mempool_type*)GetMempoolPtr(msg);
    //mempool_type *mptr1 = (mempool_type*)GetMempoolPtr(msg);
    mempool_type *mptr;

    status = registerFromMempool(mptr1, msg, size, t, cqh);
    if (status == GNI_RC_SUCCESS) return status;
#if CMK_SMP
    for (i=0; i<CmiMyNodeSize()+1; i++) {
      rank = (rank+1)%(CmiMyNodeSize()+1);
      mptr = CpvAccessOther(mempool, rank);
      if (mptr == mptr1) continue;
      status = registerFromMempool(mptr, msg, size, t, cqh);
      if (status == GNI_RC_SUCCESS) return status;
    }
#endif
    return  GNI_RC_ERROR_RESOURCE;
}

#if CMK_ONESIDED_IMPL
#include "machine-onesided.h"
#endif

INLINE_KEYWORD
static void buffer_small_msgs(SMSG_QUEUE *queue, void *msg, int size, int destNode, uint8_t tag)
{
    MSG_LIST        *msg_tmp;
    MallocMsgList(msg_tmp);
    msg_tmp->destNode = destNode;
    msg_tmp->size   = size;
    msg_tmp->msg    = msg;
    msg_tmp->tag    = tag;
#if CMK_WITH_STATS
    SMSG_CREATION(msg_tmp)
#endif

#if ONE_SEND_QUEUE
#if CMK_LOCKLESS_QUEUE
    MPMCQueuePush(queue->sendMsgBuf, (char*)msg_tmp);
#else
    PCQueuePush(queue->sendMsgBuf, (char*)msg_tmp);
#endif
#else
#if SMP_LOCKS
    CmiLock(queue->smsg_msglist_index[destNode].lock);
    if(queue->smsg_msglist_index[destNode].pushed == 0)
    {
        PCQueuePush(queue->nonEmptyQueues, (char*)&(queue->smsg_msglist_index[destNode]));
    }
    PCQueuePush(queue->smsg_msglist_index[destNode].sendSmsgBuf, (char*)msg_tmp);
    CmiUnlock(queue->smsg_msglist_index[destNode].lock);
#else
    PCQueuePush(queue->smsg_msglist_index[destNode].sendSmsgBuf, (char*)msg_tmp);
#endif
#endif

#if PRINT_SYH
    buffered_smsg_counter++;
#endif
}

INLINE_KEYWORD static void print_smsg_attr(gni_smsg_attr_t     *a)
{
    printf("type=%d\n, credit=%d\n, size=%d\n, buf=%p, offset=%d\n", a->msg_type, a->mbox_maxcredit, a->buff_size, a->msg_buffer, a->mbox_offset);
}
#if 0
INLINE_KEYWORD
static void setup_smsg_connection(int destNode)
{
    mdh_addr_list_t  *new_entry = 0;
    gni_post_descriptor_t *pd;
    gni_smsg_attr_t      *smsg_attr;
    gni_return_t status = GNI_RC_NOT_DONE;
    RDMA_REQUEST        *rdma_request_msg;

    if(smsg_available_slot == smsg_expand_slots)
    {
        new_entry = (mdh_addr_list_t*)malloc(sizeof(mdh_addr_list_t));
        new_entry->addr = memalign(64, smsg_memlen*smsg_expand_slots);
        memset(new_entry->addr, 0, smsg_memlen*smsg_expand_slots);

        status = GNI_MemRegister(nic_hndl, (uint64_t)new_entry->addr,
            smsg_memlen*smsg_expand_slots, smsg_rx_cqh,
            GNI_MEM_READWRITE,
            -1,
            &(new_entry->mdh));
        smsg_available_slot = 0;
        new_entry->next = smsg_dynamic_list;
        smsg_dynamic_list = new_entry;
    }
    smsg_attr = (gni_smsg_attr_t*) malloc (sizeof(gni_smsg_attr_t));
    smsg_attr->msg_type = GNI_SMSG_TYPE_MBOX_AUTO_RETRANSMIT;
    smsg_attr->mbox_maxcredit = SMSG_MAX_CREDIT;
    smsg_attr->msg_maxsize = SMSG_MAX_MSG;
    smsg_attr->mbox_offset = smsg_available_slot * smsg_memlen;
    smsg_attr->buff_size = smsg_memlen;
    smsg_attr->msg_buffer = smsg_dynamic_list->addr;
    smsg_attr->mem_hndl = smsg_dynamic_list->mdh;
    smsg_local_attr_vec[destNode] = smsg_attr;
    smsg_available_slot++;
    MallocPostDesc(pd);
    pd->type            = GNI_POST_FMA_PUT;
    pd->cq_mode         = GNI_CQMODE_GLOBAL_EVENT ;
    pd->dlvr_mode       = GNI_DLVMODE_PERFORMANCE;
    pd->length          = sizeof(gni_smsg_attr_t);
    pd->local_addr      = (uint64_t) smsg_attr;
    pd->remote_addr     = (uint64_t)&((((gni_smsg_attr_t*)(smsg_connection_vec[destNode].addr))[myrank]));
    pd->remote_mem_hndl = smsg_connection_vec[destNode].mdh;
    pd->src_cq_hndl     = 0;

    pd->rdma_mode       = 0;
    status = GNI_PostFma(ep_hndl_array[destNode],  pd);
    print_smsg_attr(smsg_attr);
    if(status == GNI_RC_ERROR_RESOURCE )
    {
        MallocRdmaRequest(rdma_request_msg);
        rdma_request_msg->destNode = destNode;
        rdma_request_msg->pd = pd;
        /* buffer this request */
    }
#if PRINT_SYH
    if(status != GNI_RC_SUCCESS)
       printf("[%d=%d] send post FMA %s\n", myrank, destNode, gni_err_str[status]);
    else
        printf("[%d=%d]OK send post FMA \n", myrank, destNode);
#endif
}
#endif
/* useDynamicSMSG */
INLINE_KEYWORD
static void alloc_smsg_attr( gni_smsg_attr_t *local_smsg_attr)
{
    gni_return_t status = GNI_RC_NOT_DONE;

    if(mailbox_list->offset == mailbox_list->size)
    {
        dynamic_smsg_mailbox_t *new_mailbox_entry;
        new_mailbox_entry = (dynamic_smsg_mailbox_t*)malloc(sizeof(dynamic_smsg_mailbox_t));
        new_mailbox_entry->size = smsg_memlen*avg_smsg_connection;
        new_mailbox_entry->mailbox_base = malloc(new_mailbox_entry->size);
        memset(new_mailbox_entry->mailbox_base, 0, new_mailbox_entry->size);
        new_mailbox_entry->offset = 0;

        status = GNI_MemRegister(nic_hndl, (uint64_t)new_mailbox_entry->mailbox_base,
            new_mailbox_entry->size, smsg_rx_cqh,
            GNI_MEM_READWRITE,
            -1,
            &(new_mailbox_entry->mem_hndl));

        GNI_RC_CHECK("register", status);
        new_mailbox_entry->next = mailbox_list;
        mailbox_list = new_mailbox_entry;
    }
    local_smsg_attr->msg_type = GNI_SMSG_TYPE_MBOX_AUTO_RETRANSMIT;
    local_smsg_attr->mbox_maxcredit = SMSG_MAX_CREDIT;
    local_smsg_attr->msg_maxsize = SMSG_MAX_MSG;
    local_smsg_attr->mbox_offset = mailbox_list->offset;
    mailbox_list->offset += smsg_memlen;
    local_smsg_attr->buff_size = smsg_memlen;
    local_smsg_attr->msg_buffer = mailbox_list->mailbox_base;
    local_smsg_attr->mem_hndl = mailbox_list->mem_hndl;
}

/* useDynamicSMSG */
INLINE_KEYWORD
static int connect_to(int destNode)
{
    gni_return_t status = GNI_RC_NOT_DONE;
    CmiAssert(smsg_connected_flag[destNode] == 0);
    CmiAssert (smsg_attr_vector_local[destNode] == NULL);
    smsg_attr_vector_local[destNode] = (gni_smsg_attr_t*) malloc (sizeof(gni_smsg_attr_t));
    alloc_smsg_attr(smsg_attr_vector_local[destNode]);
    smsg_attr_vector_remote[destNode] = (gni_smsg_attr_t*) malloc (sizeof(gni_smsg_attr_t));

    CMI_GNI_LOCK(global_gni_lock)
    status = GNI_EpPostDataWId (ep_hndl_array[destNode], smsg_attr_vector_local[destNode], sizeof(gni_smsg_attr_t),smsg_attr_vector_remote[destNode] ,sizeof(gni_smsg_attr_t), destNode+mysize);
    CMI_GNI_UNLOCK(global_gni_lock)
    if (status == GNI_RC_ERROR_RESOURCE) {
      /* possibly destNode is making connection at the same time */
      free(smsg_attr_vector_local[destNode]);
      smsg_attr_vector_local[destNode] = NULL;
      free(smsg_attr_vector_remote[destNode]);
      smsg_attr_vector_remote[destNode] = NULL;
      mailbox_list->offset -= smsg_memlen;
#if PRINT_SYH
    printf("[%d] send connect_to request to %d failed\n", myrank, destNode);
#endif
      return 0;
    }
    GNI_RC_CHECK("GNI_Post", status);
    smsg_connected_flag[destNode] = 1;
#if PRINT_SYH
    printf("[%d] send connect_to request to %d done\n", myrank, destNode);
#endif
    return 1;
}

INLINE_KEYWORD
static gni_return_t send_smsg_message(SMSG_QUEUE *queue, int destNode, void *msg, int size, uint8_t tag, int inbuff, MSG_LIST *ptr, int smsgType, int useHeader)
{
    unsigned int          remote_address;
    uint32_t              remote_id;
    gni_return_t          status = GNI_RC_ERROR_RESOURCE;
    gni_smsg_attr_t       *smsg_attr;
    gni_post_descriptor_t *pd;
    gni_post_state_t      post_state;
    char                  *real_data;
    int                   msgid = 0;

    if (useDynamicSMSG) {
        switch (smsg_connected_flag[destNode]) {
        case 0:
            connect_to(destNode);         /* continue to case 1 */
        case 1:                           /* pending connection, do nothing */
            status = GNI_RC_NOT_DONE;
            if(inbuff ==0)
                buffer_small_msgs(queue, msg, size, destNode, tag);
            return status;
        }
    }
#if ! ONE_SEND_QUEUE
    if(PCQueueEmpty(queue->smsg_msglist_index[destNode].sendSmsgBuf) || inbuff==1)
#endif
    {
        //CMI_GNI_LOCK(smsg_mailbox_lock)
        CMI_GNI_LOCK(default_tx_cq_lock)
#if CMK_SMP_TRACE_COMMTHREAD
        int oldpe = -1;
        int oldeventid = -1;
        if(tag == SMALL_DATA_TAG || tag == LMSG_INIT_TAG || tag == LMSG_OOB_INIT_TAG || tag == LMSG_PERSISTENT_INIT_TAG)
        {
            START_EVENT();
            if ( tag == SMALL_DATA_TAG)
                real_data = (char*)msg;
            else
                real_data = (char*)(((CONTROL_MSG*)msg)->source_addr);
            TRACE_COMM_GET_MSGID(real_data, &oldpe, &oldeventid);
            TRACE_COMM_SET_COMM_MSGID(real_data);
        }
#endif
#if REMOTE_EVENT
        if (tag == LMSG_INIT_TAG || tag == LMSG_OOB_INIT_TAG || tag == LMSG_PERSISTENT_INIT_TAG) {
            CONTROL_MSG *control_msg_tmp = (CONTROL_MSG*)msg;
            if (control_msg_tmp->seq_id <= 0 && control_msg_tmp->ack_index == -1)
            {
                control_msg_tmp->ack_index = IndexPool_getslot(&ackPool, (void*)control_msg_tmp->source_addr, 1);
                if (control_msg_tmp->ack_index == -1) {    /* table overflow */
                    status = GNI_RC_NOT_DONE;
                    if (inbuff ==0)
                        buffer_small_msgs(queue, msg, size, destNode, tag);
                    return status;
                }
            }
        }
#endif
#if     CMK_WITH_STATS
        SMSG_TRY_SEND(tag)
#endif
#if CMK_WITH_STATS
        double              creation_time;
        if (ptr == NULL)
            creation_time = CmiWallTimer();
        else
            creation_time = ptr->creation_time;
#endif

#if CMK_SMSGS_FREE_AFTER_EVENT
        msgid = IndexPool_getslot(&smsgsPool, msg, smsgType);
        if(msgid == -1)
            CmiAbort("IndexPool for SMSG overflows.");
#endif
        if(!useHeader)
          status = GNI_SmsgSendWTag(ep_hndl_array[destNode], NULL, 0, msg, size, msgid, tag);
        else
          status = GNI_SmsgSendWTag(ep_hndl_array[destNode], msg, size, NULL, 0, msgid, tag);
#if CMK_SMP_TRACE_COMMTHREAD
        if (oldpe != -1)  TRACE_COMM_SET_MSGID(real_data, oldpe, oldeventid);
#endif
        CMI_GNI_UNLOCK(default_tx_cq_lock)
        //CMI_GNI_UNLOCK(smsg_mailbox_lock)
        if(status == GNI_RC_SUCCESS)
        {
#if     CMK_WITH_STATS
            SMSG_SENT_DONE(creation_time,tag)
#endif
#if CMK_SMP_TRACE_COMMTHREAD
            if(tag == SMALL_DATA_TAG || tag == LMSG_INIT_TAG || tag == LMSG_OOB_INIT_TAG || tag == LMSG_PERSISTENT_INIT_TAG )
            {
                TRACE_COMM_CREATION(CpvAccess(projTraceStart), real_data);
            }
#endif
        }else {
            status = GNI_RC_ERROR_RESOURCE;
#if CMK_SMSGS_FREE_AFTER_EVENT
            IndexPool_freeslot(&smsgsPool, msgid);
#endif
        }
    }
    if(status != GNI_RC_SUCCESS && inbuff ==0)
        buffer_small_msgs(queue, msg, size, destNode, tag);
    return status;
}

INLINE_KEYWORD
static CONTROL_MSG* construct_control_msg(int size, char *msg, int seqno)
{
    /* construct a control message and send */
    CONTROL_MSG         *control_msg_tmp;
    MallocControlMsg(control_msg_tmp);
    control_msg_tmp->source_addr = (uint64_t)msg;
    control_msg_tmp->seq_id    = seqno;
    control_msg_tmp->total_length = control_msg_tmp->length = ALIGN64(size); //for GET 4 bytes aligned
#if REMOTE_EVENT
    control_msg_tmp->ack_index    =  -1;
#endif
#if     USE_LRTS_MEMPOOL
    if(size < BIG_MSG)
    {
        control_msg_tmp->source_mem_hndl = GetMemHndl(msg);
    }
    else
    {
        SetMemHndlZero(control_msg_tmp->source_mem_hndl);
        control_msg_tmp->length = size - (seqno-1)*ONE_SEG;
        if (control_msg_tmp->length > ONE_SEG) control_msg_tmp->length = ONE_SEG;
    }
#else
    SetMemHndlZero(control_msg_tmp->source_mem_hndl);
#endif
    return control_msg_tmp;
}

#define BLOCKING_SEND_CONTROL    0

// Large message, send control to receiver, receiver register memory and do a GET,
// return 1 - send no success
INLINE_KEYWORD static gni_return_t send_large_messages(SMSG_QUEUE *queue, int destNode, CONTROL_MSG  *control_msg_tmp, int inbuff, MSG_LIST *smsg_ptr, uint8_t lmsg_tag)
{
    gni_return_t        status  =  GNI_RC_ERROR_NOMEM;
    uint32_t            vmdh_index  = -1;
    int                 size;
    int                 offset = 0;
    uint64_t            source_addr;
    int                 register_size;
    void                *msg;

    size    =   control_msg_tmp->total_length;
    source_addr = control_msg_tmp->source_addr;
    register_size = control_msg_tmp->length;

#if  USE_LRTS_MEMPOOL
    if( control_msg_tmp->seq_id <=0 ){
#if BLOCKING_SEND_CONTROL
        if (inbuff == 0 && IsMemHndlZero(GetMemHndl(source_addr))) {
            while (IsMemHndlZero(GetMemHndl(source_addr)) && buffered_send_msg + GetMempoolsize((void*)source_addr) >= MAX_BUFF_SEND)
                LrtsAdvanceCommunication(0);
        }
#endif
        if(IsMemHndlZero(GetMemHndl(source_addr))) //it is in mempool, it is possible to be de-registered by others
        {
            msg = (void*)source_addr;
            if(buffered_send_msg + GetMempoolsize(msg) >= MAX_BUFF_SEND)
            {
                if(!inbuff)
                    buffer_small_msgs(queue, control_msg_tmp, CONTROL_MSG_SIZE, destNode, lmsg_tag);
                return GNI_RC_ERROR_NOMEM;
            }
            //register the corresponding mempool
            status = registerMemory(GetMempoolBlockPtr(msg), GetMempoolsize(msg), &(GetMemHndl(msg)), rdma_rx_cqh);
            if(status == GNI_RC_SUCCESS)
            {
                control_msg_tmp->source_mem_hndl = GetMemHndl(source_addr);
            }
        }else
        {
            control_msg_tmp->source_mem_hndl = GetMemHndl(source_addr);
            status = GNI_RC_SUCCESS;
        }
        if(NoMsgInSend(source_addr))
            register_size = GetMempoolsize((void*)(source_addr));
        else
            register_size = 0;
    }else if(control_msg_tmp->seq_id >0)    // BIG_MSG
    {
        int offset = ONE_SEG*(control_msg_tmp->seq_id-1);
        source_addr += offset;
        size = control_msg_tmp->length;
#if BLOCKING_SEND_CONTROL
        if (inbuff == 0 && IsMemHndlZero(control_msg_tmp->source_mem_hndl)) {
            while (IsMemHndlZero(control_msg_tmp->source_mem_hndl) && buffered_send_msg + size >= MAX_BUFF_SEND)
                LrtsAdvanceCommunication(0);
        }
#endif
        if (IsMemHndlZero(control_msg_tmp->source_mem_hndl)) {
            if(buffered_send_msg + size >= MAX_BUFF_SEND)
            {
                if(!inbuff)
                    buffer_small_msgs(queue, control_msg_tmp, CONTROL_MSG_SIZE, destNode, lmsg_tag);
                return GNI_RC_ERROR_NOMEM;
            }
            status = registerMemory((void*)source_addr, ALIGN64(size), &(control_msg_tmp->source_mem_hndl), NULL);
            if(status == GNI_RC_SUCCESS) buffered_send_msg += ALIGN64(size);
        }
        else
        {
            status = GNI_RC_SUCCESS;
        }
        register_size = 0;
    }

#if CMI_EXERT_SEND_LARGE_CAP
    if(SEND_large_pending >= SEND_large_cap)
    {
        status = GNI_RC_ERROR_NOMEM;
    }
#endif

    if(status == GNI_RC_SUCCESS)
    {
       status = send_smsg_message( queue, destNode, control_msg_tmp, CONTROL_MSG_SIZE, lmsg_tag, inbuff, smsg_ptr, NONCHARM_SMSG, 0);
        if(status == GNI_RC_SUCCESS)
        {
#if CMI_EXERT_SEND_LARGE_CAP
            SEND_large_pending++;
#endif
            buffered_send_msg += register_size;
            if(control_msg_tmp->seq_id == 0)
            {
                IncreaseMsgInSend(source_addr);
            }
#if ! CMK_SMSGS_FREE_AFTER_EVENT
            FreeControlMsg(control_msg_tmp);
#endif
            MACHSTATE5(8, "GO SMSG LARGE to %d (%d,%d,%d) tag=%d\n", destNode, buffered_send_msg, buffered_recv_msg, register_memory_size, lmsg_tag);
        }else
            status = GNI_RC_ERROR_RESOURCE;

    } else if (status == GNI_RC_INVALID_PARAM || status == GNI_RC_PERMISSION_ERROR)
    {
        CmiAbort("Memory registor for large msg\n");
    }else
    {
        status = GNI_RC_ERROR_NOMEM;
        if(!inbuff)
            buffer_small_msgs(queue, control_msg_tmp, CONTROL_MSG_SIZE, destNode, lmsg_tag);
    }
    return status;
#else
    MEMORY_REGISTER(onesided_hnd, nic_hndl,msg, ALIGN64(size), &(control_msg_tmp->source_mem_hndl), &omdh, NULL, status)
    if(status == GNI_RC_SUCCESS)
    {
        status = send_smsg_message(queue, destNode, control_msg_tmp, CONTROL_MSG_SIZE, lmsg_tag, 0, NULL, NONCHARM_SMSG, 0);
#if ! CMK_SMSGS_FREE_AFTER_EVENT
        if(status == GNI_RC_SUCCESS)
        {
            FreeControlMsg(control_msg_tmp);
        }
#endif
    } else if (status == GNI_RC_INVALID_PARAM || status == GNI_RC_PERMISSION_ERROR)
    {
        CmiAbort("Memory registor for large msg\n");
    }else
    {
        buffer_small_msgs(queue, control_msg_tmp, CONTROL_MSG_SIZE, destNode, lmsg_tag);
    }
    return status;
#endif
}

INLINE_KEYWORD void LrtsBeginIdle(void) {}

INLINE_KEYWORD void LrtsStillIdle(void) {}

INLINE_KEYWORD void LrtsNotifyIdle(void) {}

INLINE_KEYWORD void LrtsPrepareEnvelope(char *msg, int size)
{
    CmiSetMsgSize(msg, size);
    CMI_SET_CHECKSUM(msg, size);
}

CmiCommHandle LrtsSendFunc(int destNode, int destPE, int size, char *msg, int mode)
{
    gni_return_t        status  =   GNI_RC_SUCCESS;
    uint8_t tag;
    CONTROL_MSG         *control_msg_tmp;
    int                 oob = ( mode & OUT_OF_BAND);
    SMSG_QUEUE          *queue;

    MACHSTATE5(8, "GO LrtsSendFn %d(%d) (%d,%d, %d) \n", destNode, size, buffered_send_msg, buffered_recv_msg, register_memory_size);
#if CMK_USE_OOB
    queue = oob? &smsg_oob_queue : &smsg_queue;
    tag = oob? LMSG_OOB_INIT_TAG: LMSG_INIT_TAG;
#else
    queue = &smsg_queue;
    tag = LMSG_INIT_TAG;
#endif

    LrtsPrepareEnvelope(msg, size);

#if PRINT_SYH
    printf("LrtsSendFn %d==>%d, size=%d\n", myrank, destNode, size);
#endif

#if CMK_SMP
    if(size <= SMSG_MAX_MSG)
        buffer_small_msgs(queue, msg, size, destNode, SMALL_DATA_TAG);
    else if (size < BIG_MSG) {
        control_msg_tmp =  construct_control_msg(size, msg, 0);
        buffer_small_msgs(queue, control_msg_tmp, CONTROL_MSG_SIZE, destNode, tag);
    }
    else {
        CmiSetMsgSeq(msg, 0);
        control_msg_tmp =  construct_control_msg(size, msg, 1);
        buffer_small_msgs(queue, control_msg_tmp, CONTROL_MSG_SIZE, destNode, tag);
    }
#else   //non-smp, smp(worker sending)
    if(size <= SMSG_MAX_MSG)
    {
        if (GNI_RC_SUCCESS == send_smsg_message(queue, destNode,  msg, size, SMALL_DATA_TAG, 0, NULL, CHARM_SMSG, 0))
        {
#if !CMK_SMSGS_FREE_AFTER_EVENT
            CmiFree(msg);
#endif
        }
    }
    else if (size < BIG_MSG) {
        control_msg_tmp =  construct_control_msg(size, msg, 0);
        send_large_messages(queue, destNode, control_msg_tmp, 0, NULL, tag);
    }
    else {
#if     USE_LRTS_MEMPOOL
        CmiSetMsgSeq(msg, 0);
        control_msg_tmp =  construct_control_msg(size, msg, 1);
        send_large_messages(queue, destNode, control_msg_tmp, 0, NULL, tag);
#else
        control_msg_tmp =  construct_control_msg(size, msg, 0);
        send_large_messages(queue, destNode, control_msg_tmp, 0, NULL, tag);
#endif
    }
#endif
    return 0;
}

static void    PumpDatagramConnection(void);
static      int         event_SetupConnect = 111;
static      int         event_PumpSmsg = 222 ;
static      int         event_PumpTransaction = 333;
static      int         event_PumpRdmaTransaction = 444;
static      int         event_SendBufferSmsg = 484;
static      int         event_SendFmaRdmaMsg = 555;
static      int         event_AdvanceCommunication = 666;

static void registerUserTraceEvents(void) {
#if CMI_MACH_TRACE_USEREVENTS && CMK_TRACE_ENABLED && !CMK_TRACE_IN_CHARM
    event_SetupConnect = traceRegisterUserEvent("setting up connections", -1 );
    event_PumpSmsg = traceRegisterUserEvent("Pump network small msgs", -1);
    event_PumpTransaction = traceRegisterUserEvent("Pump FMA/RDMA local transaction" , -1);
    event_PumpRdmaTransaction = traceRegisterUserEvent("Pump RDMA remote event" , -1);
    event_SendBufferSmsg = traceRegisterUserEvent("Sending buffered small msgs", -1);
    event_SendFmaRdmaMsg = traceRegisterUserEvent("Sending buffered fma/rdma transactions", -1);
    event_AdvanceCommunication = traceRegisterUserEvent("Worker thread in sending/receiving", -1);
#endif
}

static void ProcessDeadlock(void)
{
    static CmiUInt8 *ptr = NULL;
    static CmiUInt8  last = 0, mysum, sum;
    static int count = 0;
    gni_return_t status;
    int i;

//printf("[%d] comm thread detected hang %d %d %d\n", CmiMyPe(), smsg_send_count, smsg_recv_count, count);
//sweep_mempool(CpvAccess(mempool));
    if (ptr == NULL) ptr = (CmiUInt8*)malloc(mysize * sizeof(CmiUInt8));
    mysum = smsg_send_count + smsg_recv_count;
    MACHSTATE5(9,"Before allgather Progress Deadlock (%d,%d)  (%d,%d)(%d)\n", buffered_send_msg, register_memory_size, last, sum, count);
    status = (gni_return_t)PMI_Allgather(&mysum,ptr,sizeof(CmiUInt8));
    GNI_RC_CHECK("PMI_Allgather", status);
    sum = 0;
    for (i=0; i<mysize; i++)  sum+= ptr[i];
    if (last == 0 || sum == last)
        count++;
    else
        count = 0;
    last = sum;
    MACHSTATE5(9,"Progress Deadlock (%d,%d)  (%d,%d)(%d)\n", buffered_send_msg, register_memory_size, last, sum, count);
    if (count == 2) {
        /* detected twice, it is a real deadlock */
        if (myrank == 0)  {
            CmiPrintf("Charm++> Network progress engine appears to have stalled, possibly because registered memory limits have been exceeded or are too low.  Try adjusting environment variables CHARM_UGNI_MEMPOOL_MAX and CHARM_UGNI_SEND_MAX (current limits are %lld and %lld).\n", MAX_REG_MEM, MAX_BUFF_SEND);
            CmiAbort("Fatal> Deadlock detected.");
        }

    }
    _detected_hang = 0;
}

static void CheckProgress(void)
{
    if (smsg_send_count == last_smsg_send_count &&
        smsg_recv_count == last_smsg_recv_count )
    {
        _detected_hang = 1;
#if !CMK_SMP
        if (_detected_hang) ProcessDeadlock();
#endif

    }
    else {
        //MACHSTATE5(9,"--Check Progress %d(%d, %d) (%d,%d)\n", mycount, buffered_send_msg, register_memory_size, smsg_send_count, smsg_recv_count);
        last_smsg_send_count = smsg_send_count;
        last_smsg_recv_count = smsg_recv_count;
        _detected_hang = 0;
    }
}

static void set_limit(void)
{
    //if (!user_set_flag && CmiMyRank() == 0) {
    if (CmiMyRank() == 0) {
        int mynode = CmiPhysicalNodeID(CmiMyPe());
        int numpes = CmiNumPesOnPhysicalNode(mynode);
        int numprocesses = numpes / CmiMyNodeSize();
        MAX_REG_MEM  = _totalmem / numprocesses;
        MAX_BUFF_SEND = MAX_REG_MEM / 2;
        if (CmiMyPe() == 0)
           printf("mem_max = %.2fM, send_max =%.2fM\n", MAX_REG_MEM/1024.0/1024, MAX_BUFF_SEND/1024./1024);
        if(CmiMyPe() == 0 && (smsg_memlen*mysize + _expand_mem > MAX_BUFF_SEND ||  smsg_memlen*mysize + _mempool_size > MAX_BUFF_SEND))
        {
             printf("Charm++> FATAL ERROR your program has risk of hanging \n please try large page or use Dynamic smsg +useDynamicSmsg or contact Charm++ developers\n");
             CmiAbort("memory registration\n");
        }
    }
}

void LrtsPostCommonInit(int everReturn)
{
#if CMI_MACH_TRACE_USEREVENTS && CMK_TRACE_ENABLED && !CMK_TRACE_IN_CHARM
    CpvInitialize(double, projTraceStart);
    /* only PE 0 needs to care about registration (to generate sts file). */
    //if (CmiMyPe() == 0)
    {
        registerMachineUserEventsFunction(&registerUserTraceEvents);
    }
#endif

#if !CMK_SMP
    if (useDynamicSMSG)
    CcdCallOnConditionKeep(CcdPERIODIC_10ms, (CcdVoidFn) PumpDatagramConnection, NULL);
#endif

#if ! LARGEPAGE
    if (_checkProgress)
#if CMK_SMP
    if (CmiMyRank() == 0)
#endif
    CcdCallOnConditionKeep(CcdPERIODIC_2minute, (CcdVoidFn) CheckProgress, NULL);
#endif

#if !LARGEPAGE
    CcdCallOnCondition(CcdTOPOLOGY_AVAIL, (CcdVoidFn)set_limit, NULL);
#endif
}

/* this is called by worker thread */
void LrtsPostNonLocal(void)
{
#if 1

#if CMK_SMP_TRACE_COMMTHREAD
    double startT, endT;
#endif

#if MULTI_THREAD_SEND
    if(mysize == 1) return;

    if (CmiMyRank() % 6 != 3) return;

#if CMK_SMP_TRACE_COMMTHREAD
    traceEndIdle();
    startT = CmiWallTimer();
#endif

    CmiMachineProgressImpl();

#if CMK_SMP_TRACE_COMMTHREAD
    endT = CmiWallTimer();
    traceUserBracketEvent(event_AdvanceCommunication, startT, endT);
    traceBeginIdle();
#endif

#endif
#endif
}

/* Network progress function is used to poll the network when for
   messages. This flushes receive buffers on some  implementations*/
#if CMK_MACHINE_PROGRESS_DEFINED
void CmiMachineProgressImpl(void) {
#if ! CMK_SMP || MULTI_THREAD_SEND

    STATS_PUMPNETWORK_TIME(PumpNetworkSmsg());
    SEND_OOB_SMSG(smsg_oob_queue)
    PUMP_REMOTE_HIGHPRIORITY
    PUMP_LOCAL_HIGHPRIORITY
    POST_HIGHPRIORITY_RDMA

#if 0
#if CMK_WORKER_SINGLE_TASK
    if (CmiMyRank() % 6 == 0)
#endif
    PumpNetworkSmsg();

#if CMK_WORKER_SINGLE_TASK
    if (CmiMyRank() % 6 == 1)
#endif
    PumpLocalTransactions(default_tx_cqh, default_tx_cq_lock);

#if CMK_WORKER_SINGLE_TASK
    if (CmiMyRank() % 6 == 2)
#endif
    PumpLocalTransactions(rdma_tx_cqh, rdma_tx_cq_lock);

#if REMOTE_EVENT
#if CMK_WORKER_SINGLE_TASK
    if (CmiMyRank() % 6 == 3)
#endif
    PumpRemoteTransactions(rdma_rx_cqh);         // rdma_rx_cqh
#endif

#if CMK_WORKER_SINGLE_TASK
    if (CmiMyRank() % 6 == 4)
#endif
    {
#if CMK_USE_OOB
    SendBufferMsg(&smsg_oob_queue, NULL);
    SendBufferMsg(&smsg_queue, &smsg_oob_queue);
#else
    SendBufferMsg(&smsg_queue, NULL);
#endif
    }

#if CMK_WORKER_SINGLE_TASK
    if (CmiMyRank() % 6 == 5)
#endif
#if CMK_SMP
    STATS_SENDRDMAMSG_TIME(SendRdmaMsg(sendRdmaBuf));
#else
    STATS_SENDRDMAMSG_TIME(SendRdmaMsg());
#endif

#endif
#endif
}
#endif


/* useDynamicSMSG */
static void    PumpDatagramConnection(void)
{
    uint32_t          remote_address;
    uint32_t          remote_id;
    gni_return_t status;
    gni_post_state_t  post_state;
    uint64_t          datagram_id;
    int i;

   while ((status = GNI_PostDataProbeById(nic_hndl, &datagram_id)) == GNI_RC_SUCCESS)
   {
       if (datagram_id >= mysize) {           /* bound endpoint */
           int pe = datagram_id - mysize;
           CMI_GNI_LOCK(global_gni_lock)
           status = GNI_EpPostDataTestById( ep_hndl_array[pe], datagram_id, &post_state, &remote_address, &remote_id);
           CMI_GNI_UNLOCK(global_gni_lock)
           if(status == GNI_RC_SUCCESS && post_state == GNI_POST_COMPLETED)
           {
               CmiAssert(remote_id == pe);
               status = GNI_SmsgInit(ep_hndl_array[pe], smsg_attr_vector_local[pe], smsg_attr_vector_remote[pe]);
               GNI_RC_CHECK("Dynamic SMSG Init", status);
#if PRINT_SYH
               printf("[%d] ++ Dynamic SMSG setup [%d===>%d] done\n", myrank, myrank, pe);
#endif
	       CmiAssert(smsg_connected_flag[pe] == 1);
               smsg_connected_flag[pe] = 2;
           }
       }
       else {         /* unbound ep */
           status = GNI_EpPostDataTestById( ep_hndl_unbound, datagram_id, &post_state, &remote_address, &remote_id);
           if(status == GNI_RC_SUCCESS && post_state == GNI_POST_COMPLETED)
           {
               CmiAssert(remote_id<mysize);
	       CmiAssert(smsg_connected_flag[remote_id] <= 0);
               status = GNI_SmsgInit(ep_hndl_array[remote_id], &send_smsg_attr, &recv_smsg_attr);
               GNI_RC_CHECK("Dynamic SMSG Init", status);
#if PRINT_SYH
               printf("[%d] ++ Dynamic SMSG setup2 [%d===>%d] done\n", myrank, myrank, remote_id);
#endif
               smsg_connected_flag[remote_id] = 2;

               alloc_smsg_attr(&send_smsg_attr);
               status = GNI_EpPostDataWId (ep_hndl_unbound, &send_smsg_attr,  SMSG_ATTR_SIZE, &recv_smsg_attr, SMSG_ATTR_SIZE, myrank);
               GNI_RC_CHECK("post unbound datagram", status);
           }
       }
   }
}

/* pooling CQ to receive network message */
static void PumpNetworkRdmaMsgs(void)
{
    gni_cq_entry_t      event_data;
    gni_return_t        status;

}

INLINE_KEYWORD
static void bufferRdmaMsg(PCQueue bufferqueue, int inst_id, gni_post_descriptor_t *pd, int ack_index)
{
    RDMA_REQUEST        *rdma_request_msg;
    MallocRdmaRequest(rdma_request_msg);
    rdma_request_msg->destNode = inst_id;
    rdma_request_msg->pd = pd;
#if REMOTE_EVENT
    rdma_request_msg->ack_index = ack_index;
#endif
    PCQueuePush(bufferqueue, (char*)rdma_request_msg);
}

static void getLargeMsgRequest(void* header, uint64_t inst_id,  uint8_t tag, PCQueue);
static void getPersistentMsgRequest(void* header, uint64_t inst_id,  uint8_t tag, PCQueue);
static void PRINT_CONTROL(void *header)
{
    CONTROL_MSG *control_msg = (CONTROL_MSG *) header;

    printf(" length=%d , seq_id = %d, addr = %lld:%lld:%lld  \n", control_msg->length, control_msg->seq_id, control_msg->source_addr, (control_msg->source_mem_hndl).qword1, (control_msg->source_mem_hndl).qword2 );
#if PERSISTENT_GET_BASE
    printf(" memhdl = %lld:%lld:%lld \n", control_msg->dest_addr, (control_msg->dest_mem_hndl).qword1, (control_msg->dest_mem_hndl).qword2);
#endif
}
static void PumpNetworkSmsg()
{
    uint64_t            inst_id;
    gni_cq_entry_t      event_data;
    gni_return_t        status, deregStatus;
    void                *header;
    uint8_t             msg_tag;
    int                 msg_nbytes;
    void                *msg_data;
    gni_mem_handle_t    msg_mem_hndl;
    gni_smsg_attr_t     *smsg_attr;
    gni_smsg_attr_t     *remote_smsg_attr;
    int                 init_flag;
    CONTROL_MSG         *control_msg_tmp, *header_tmp;
    uint64_t            source_addr;
    SMSG_QUEUE         *queue = &smsg_queue;
    PCQueue             tmp_queue;
    int                 recvInfoSize;
#if CMI_PUMPNETWORKSMSG_CAP
    int                  recv_cnt = 0;
    while(recv_cnt< PumpNetworkSmsg_cap) {
#else
    while(1) {
#endif
        CMI_GNI_LOCK(smsg_rx_cq_lock)
        status =GNI_CqGetEvent(smsg_rx_cqh, &event_data);
        CMI_GNI_UNLOCK(smsg_rx_cq_lock)
        if(status != GNI_RC_SUCCESS) break;

        inst_id = GNI_CQ_GET_REM_INST_ID(event_data);
#if REMOTE_EVENT
        inst_id = GET_RANK(inst_id);      /* important */
#endif
        // GetEvent returns success but GetNext return not_done. caused by Smsg out-of-order transfer
#if PRINT_SYH
        printf("[%d] %d PumpNetworkMsgs is received from PE: %d,  status=%s\n", myrank, CmiMyRank(), inst_id,  gni_err_str[status]);
#endif
        if (useDynamicSMSG) {
            /* subtle: smsg may come before connection is setup */
            while (smsg_connected_flag[inst_id] != 2)
               PumpDatagramConnection();
        }
        msg_tag = GNI_SMSG_ANY_TAG;
        while(1) {
            CMI_GNI_LOCK(smsg_mailbox_lock)
            status = GNI_SmsgGetNextWTag(ep_hndl_array[inst_id], &header, &msg_tag);
            if (status != GNI_RC_SUCCESS)
            {
                CMI_GNI_UNLOCK(smsg_mailbox_lock)
                break;
            }
#if         CMI_PUMPNETWORKSMSG_CAP
            recv_cnt++;
#endif
#if PRINT_SYH
            printf("[%d] from %d smsg msg is received, messageid: tag=%d\n", myrank, inst_id, msg_tag);
#endif
            /* copy msg out and then put into queue (small message) */
            switch (msg_tag) {
            case SMALL_DATA_TAG:
            {
                msg_nbytes = CmiGetMsgSize(header);
                CmiAssert(msg_nbytes > 0);
                msg_data    = CmiAlloc(msg_nbytes);
                memcpy(msg_data, (char*)header, msg_nbytes);
                GNI_SmsgRelease(ep_hndl_array[inst_id]);
                CMI_GNI_UNLOCK(smsg_mailbox_lock)
                CMI_CHECK_CHECKSUM(msg_data, msg_nbytes);
                handleOneRecvedMsg(msg_nbytes, (char *)msg_data);
                break;
            }
            case LMSG_PERSISTENT_INIT_TAG:
            {   CMI_GNI_UNLOCK(smsg_mailbox_lock)
                getPersistentMsgRequest(header, inst_id, msg_tag, sendRdmaBuf);
                GNI_SmsgRelease(ep_hndl_array[inst_id]);
                break;
            }
            case LMSG_INIT_TAG:
            case LMSG_OOB_INIT_TAG:
            {
                tmp_queue = (msg_tag == LMSG_INIT_TAG)? sendRdmaBuf : sendHighPriorBuf;
#if MULTI_THREAD_SEND
                MallocControlMsg(control_msg_tmp);
                memcpy(control_msg_tmp, header, CONTROL_MSG_SIZE);
                GNI_SmsgRelease(ep_hndl_array[inst_id]);
                CMI_GNI_UNLOCK(smsg_mailbox_lock)
                getLargeMsgRequest(control_msg_tmp, inst_id, msg_tag, tmp_queue);
                FreeControlMsg(control_msg_tmp);
#else
                CMI_GNI_UNLOCK(smsg_mailbox_lock)
                getLargeMsgRequest(header, inst_id, msg_tag, tmp_queue);
                GNI_SmsgRelease(ep_hndl_array[inst_id]);
#endif
                break;
            }
#if !REMOTE_EVENT && !CQWRITE
            case ACK_TAG:   //msg fit into mempool
            {
                /* Get is done, release message . Now put is not used yet*/
                void *msg = (void*)(((ACK_MSG *)header)->source_addr);
                GNI_SmsgRelease(ep_hndl_array[inst_id]);
                CMI_GNI_UNLOCK(smsg_mailbox_lock)
#if ! USE_LRTS_MEMPOOL
                MEMORY_DEREGISTER(onesided_hnd, nic_hndl, &(((ACK_MSG *)header)->source_mem_hndl), &omdh, ((ACK_MSG *)header)->length);
#else
                DecreaseMsgInSend(msg);
#endif
                if(NoMsgInSend(msg))
                    buffered_send_msg -= GetMempoolsize(msg);
                MACHSTATE5(8, "GO send done to %d (%d,%d, %d) tag=%d\n", inst_id, buffered_send_msg, buffered_recv_msg, register_memory_size, msg_tag);
                CmiFree(msg);
#if CMI_EXERT_SEND_LARGE_CAP
                SEND_large_pending--;
#endif
                break;
            }
#endif
#if CMK_ONESIDED_IMPL
            case RDMA_PUT_MD_DIRECT_TAG:
            {
                NcpyOperationInfo *ncpyOpInfo = (NcpyOperationInfo *)header;

                // copy into a new object
                NcpyOperationInfo *newNcpyOpInfo = (NcpyOperationInfo *)CmiAlloc(ncpyOpInfo->ncpyOpInfoSize);
                memcpy(newNcpyOpInfo, ncpyOpInfo, ncpyOpInfo->ncpyOpInfoSize);

                GNI_SmsgRelease(ep_hndl_array[inst_id]);
                CMI_GNI_UNLOCK(smsg_mailbox_lock)

                resetNcpyOpInfoPointers(newNcpyOpInfo);

                post_rdma((uint64_t)newNcpyOpInfo->destPtr,
                          ((CmiGNIRzvRdmaPtr_t *)((char *)(newNcpyOpInfo->destLayerInfo) + CmiGetRdmaCommonInfoSize()))->mem_hndl,
                          (uint64_t)newNcpyOpInfo->srcPtr,
                          ((CmiGNIRzvRdmaPtr_t *)((char *)(newNcpyOpInfo->srcLayerInfo) + CmiGetRdmaCommonInfoSize()))->mem_hndl,
                          std::min(newNcpyOpInfo->srcSize, newNcpyOpInfo->destSize),
                          (uint64_t)newNcpyOpInfo,
                          CmiNodeOf(newNcpyOpInfo->destPe),
                          GNI_POST_RDMA_PUT,
                          DIRECT_SEND_RECV);

                break;
            }
           case RDMA_PUT_DONE_DIRECT_TAG:
            {
                // Direct API when PUT is used instead of a GET
                // This tag implies the completion of an indirect PUT operation used for the Direct API
                CmiGNIRzvRdmaDirectInfo_t *putOp = (CmiGNIRzvRdmaDirectInfo_t *)header;
                void *token = (void *)putOp->ref;
                GNI_SmsgRelease(ep_hndl_array[inst_id]);
                CMI_GNI_UNLOCK(smsg_mailbox_lock)

                // Invoke the ack handler function
                CmiInvokeNcpyAck(token);
                break;
            }
            case RDMA_DEREG_DIRECT_TAG:
            {
                // Direct API
                // This tag implies a request to free the memory handle local to this node
                gni_mem_handle_t *memhndl = (gni_mem_handle_t *)header;
                status = GNI_MemDeregister(nic_hndl, memhndl);
                GNI_RC_CHECK("GNI_MemDeregister Failed!", status);
                GNI_SmsgRelease(ep_hndl_array[inst_id]);
                CMI_GNI_UNLOCK(smsg_mailbox_lock)
                break;
            }
            case RDMA_REG_AND_PUT_MD_DIRECT_TAG:
            {
                NcpyOperationInfo *ncpyOpInfo = (NcpyOperationInfo *)header;

                // copy into a new object
                NcpyOperationInfo *newNcpyOpInfo = (NcpyOperationInfo *)CmiAlloc(ncpyOpInfo->ncpyOpInfoSize);
                memcpy(newNcpyOpInfo, ncpyOpInfo, ncpyOpInfo->ncpyOpInfoSize);

                GNI_SmsgRelease(ep_hndl_array[inst_id]);
                CMI_GNI_UNLOCK(smsg_mailbox_lock)

                resetNcpyOpInfoPointers(newNcpyOpInfo);

                // Free the NcpyOperationInfo in a reverse API operation
                newNcpyOpInfo->freeMe = CMK_FREE_NCPYOPINFO;

                // Register source buffer
                ((CmiGNIRzvRdmaPtr_t *)((char *)(newNcpyOpInfo->srcLayerInfo) + CmiGetRdmaCommonInfoSize()))->mem_hndl =
                                            registerDirectMem(newNcpyOpInfo->srcPtr,
                                                                newNcpyOpInfo->srcSize,
                                                                GNI_MEM_READ_ONLY);

                newNcpyOpInfo->isSrcRegistered = 1; // Set isSrcRegistered to 1 after registration

                post_rdma((uint64_t)newNcpyOpInfo->destPtr,
                          ((CmiGNIRzvRdmaPtr_t *)((char *)(newNcpyOpInfo->destLayerInfo) + CmiGetRdmaCommonInfoSize()))->mem_hndl,
                          (uint64_t)newNcpyOpInfo->srcPtr,
                          ((CmiGNIRzvRdmaPtr_t *)((char *)(newNcpyOpInfo->srcLayerInfo) + CmiGetRdmaCommonInfoSize()))->mem_hndl,
                          std::min(newNcpyOpInfo->srcSize, newNcpyOpInfo->destSize),
                          (uint64_t)newNcpyOpInfo,
                          CmiNodeOf(newNcpyOpInfo->destPe),
                          GNI_POST_RDMA_PUT,
                          DIRECT_SEND_RECV);

                break;
            }
            case RDMA_REG_AND_GET_MD_DIRECT_TAG:
            {
                NcpyOperationInfo *ncpyOpInfo = (NcpyOperationInfo *)header;

                // copy into a new object
                NcpyOperationInfo *newNcpyOpInfo = (NcpyOperationInfo *)CmiAlloc(ncpyOpInfo->ncpyOpInfoSize);
                memcpy(newNcpyOpInfo, ncpyOpInfo, ncpyOpInfo->ncpyOpInfoSize);

                GNI_SmsgRelease(ep_hndl_array[inst_id]);
                CMI_GNI_UNLOCK(smsg_mailbox_lock)

                resetNcpyOpInfoPointers(newNcpyOpInfo);

                ((CmiGNIRzvRdmaPtr_t *)((char *)(newNcpyOpInfo->destLayerInfo) + CmiGetRdmaCommonInfoSize()))->mem_hndl =
                                              registerDirectMem(newNcpyOpInfo->destPtr,
                                                                newNcpyOpInfo->destSize,
                                                                GNI_MEM_READWRITE);

                newNcpyOpInfo->isDestRegistered = 1; // Set isDestRegistered to 1 after registration

                post_rdma((uint64_t)newNcpyOpInfo->srcPtr,
                          ((CmiGNIRzvRdmaPtr_t *)((char *)(newNcpyOpInfo->srcLayerInfo) + CmiGetRdmaCommonInfoSize()))->mem_hndl,
                          (uint64_t)newNcpyOpInfo->destPtr,
                          ((CmiGNIRzvRdmaPtr_t *)((char *)(newNcpyOpInfo->destLayerInfo) + CmiGetRdmaCommonInfoSize()))->mem_hndl,
                          std::min(newNcpyOpInfo->srcSize, newNcpyOpInfo->destSize),
                          (uint64_t)newNcpyOpInfo,
                          CmiNodeOf(newNcpyOpInfo->srcPe),
                          GNI_POST_RDMA_GET,
                          DIRECT_SEND_RECV);

                 break;
            }
            case RDMA_DEREG_AND_ACK_MD_DIRECT_TAG:
            {
                NcpyOperationInfo *ncpyOpInfo = (NcpyOperationInfo *)header;

                // copy into a new object
                NcpyOperationInfo *newNcpyOpInfo = (NcpyOperationInfo *)CmiAlloc(ncpyOpInfo->ncpyOpInfoSize);
                memcpy(newNcpyOpInfo, ncpyOpInfo, ncpyOpInfo->ncpyOpInfoSize);

                GNI_SmsgRelease(ep_hndl_array[inst_id]);
                CMI_GNI_UNLOCK(smsg_mailbox_lock)

                resetNcpyOpInfoPointers(newNcpyOpInfo);

                if(CmiMyNode() == CmiNodeOf(newNcpyOpInfo->srcPe)) {
                  LrtsDeregisterMem(newNcpyOpInfo->srcPtr,
                                    (char *)(newNcpyOpInfo->srcLayerInfo) + CmiGetRdmaCommonInfoSize(),
                                    newNcpyOpInfo->srcPe,
                                    newNcpyOpInfo->srcRegMode);

                  newNcpyOpInfo->isSrcRegistered = 0; // Set isSrcRegistered to 0 after de-registration

                  // Invoke source ack
                  newNcpyOpInfo->opMode = CMK_EM_API_SRC_ACK_INVOKE;

                } else if(CmiMyNode() == CmiNodeOf(newNcpyOpInfo->destPe)) {
                  // Deregister the destination buffer
                  LrtsDeregisterMem(newNcpyOpInfo->destPtr,
                                    (char *)newNcpyOpInfo->destLayerInfo + CmiGetRdmaCommonInfoSize(),
                                    newNcpyOpInfo->destPe,
                                    newNcpyOpInfo->destRegMode);

                  newNcpyOpInfo->isDestRegistered = 0; // Set isDestRegistered to 0 after de-registration

                  // Invoke destination ack
                  newNcpyOpInfo->opMode = CMK_EM_API_DEST_ACK_INVOKE;

                } else {

                  CmiAbort(" Cannot de-register on a different node than the source or destinaton");
                }

                newNcpyOpInfo->freeMe = CMK_FREE_NCPYOPINFO;
                CmiInvokeNcpyAck(newNcpyOpInfo);
                break;
            }

#endif
            case BIG_MSG_TAG:  //big msg, de-register, transfer next seg
            {
#if MULTI_THREAD_SEND
                MallocControlMsg(header_tmp);
                memcpy(header_tmp, header, CONTROL_MSG_SIZE);
                GNI_SmsgRelease(ep_hndl_array[inst_id]);
#else
                header_tmp = (CONTROL_MSG *) header;
#endif
                CMI_GNI_UNLOCK(smsg_mailbox_lock)
#if CMI_EXERT_SEND_LARGE_CAP
                SEND_large_pending--;
#endif
                void *msg = (void*)(header_tmp->source_addr);
                int cur_seq = CmiGetMsgSeq(msg);
                int offset = ONE_SEG*(cur_seq+1);
                MEMORY_DEREGISTER(onesided_hnd, nic_hndl, &(header_tmp->source_mem_hndl), &omdh, header_tmp->length);
                buffered_send_msg -= header_tmp->length;
                int remain_size = CmiGetMsgSize(msg) - header_tmp->length;
                if (remain_size < 0) remain_size = 0;
                CmiSetMsgSize(msg, remain_size);
                if(remain_size <= 0) //transaction done
                {
                    CmiFree(msg);
                }else if (header_tmp->total_length > offset)
                {
                    CmiSetMsgSeq(msg, cur_seq+1);
                    control_msg_tmp = construct_control_msg(header_tmp->total_length, (char *)msg, cur_seq+1+1);
                    control_msg_tmp->dest_addr = header_tmp->dest_addr;
                    //send next seg
                    send_large_messages( queue, inst_id, control_msg_tmp, 0, NULL, LMSG_INIT_TAG);
                         // pipelining
                    if (header_tmp->seq_id == 1) {
                      int i;
                      for (i=1; i<BIG_MSG_PIPELINE; i++) {
                        int seq = cur_seq+i+2;
                        CmiSetMsgSeq(msg, seq-1);
                        control_msg_tmp =  construct_control_msg(header_tmp->total_length, (char *)msg, seq);
                        control_msg_tmp->dest_addr = header_tmp->dest_addr;
                        send_large_messages( queue, inst_id, control_msg_tmp, 0, NULL, LMSG_INIT_TAG);
                        if (header_tmp->total_length <= ONE_SEG*seq) break;
                      }
                    }
                }
#if MULTI_THREAD_SEND
                FreeControlMsg(header_tmp);
#else
                GNI_SmsgRelease(ep_hndl_array[inst_id]);
#endif
                break;
            }
#if CMK_PERSISTENT_COMM_PUT && !REMOTE_EVENT && !CQWRITE
            case PUT_DONE_TAG:  {   //persistent message
                void *msg = (void *)(((CONTROL_MSG *) header)->source_addr);
                int size = ((CONTROL_MSG *) header)->length;
                GNI_SmsgRelease(ep_hndl_array[inst_id]);
                CMI_GNI_UNLOCK(smsg_mailbox_lock)
                CmiReference(msg);
                CMI_CHECK_CHECKSUM(msg, size);
                handleOneRecvedMsg(size, msg);
#if PRINT_SYH
                printf("[%d] PUT_DONE_TAG hand over one message, size: %d. \n", myrank, size);
#endif
                break;
            }
#endif
            default:
                GNI_SmsgRelease(ep_hndl_array[inst_id]);
                CMI_GNI_UNLOCK(smsg_mailbox_lock)
                printf("weird tag problem %d \n", msg_tag);
                CmiAbort("Unknown tag\n");
            }               // end switch
#if PRINT_SYH
            printf("[%d] from %d after switch request for smsg is received, messageid: tag=%d\n", myrank, inst_id, msg_tag);
#endif
            smsg_recv_count ++;
            msg_tag = GNI_SMSG_ANY_TAG;
        } //endwhile GNI_SmsgGetNextWTag
    }   //end while GetEvent
    if(status == GNI_RC_ERROR_RESOURCE)
    {
        printf("charm> Please use +useRecvQueue %d in your command line, if the error comes again, increase this number\n", REMOTE_QUEUE_ENTRIES*2);
        GNI_RC_CHECK("Smsg_rx_cq full", status);
    }
}

static void printDesc(gni_post_descriptor_t *pd)
{
    printf(" Descriptor (%p===>%p)(%d)\n", pd->local_addr, pd->remote_addr, pd->length);
}

#if CQWRITE
static void sendCqWrite(int destNode, uint64_t data, gni_mem_handle_t mem_hndl)
{
    gni_post_descriptor_t *pd;
    gni_return_t        status = GNI_RC_SUCCESS;

    MallocPostDesc(pd);
    pd->type = GNI_POST_CQWRITE;
    pd->cq_mode = GNI_CQMODE_SILENT;
    //pd->cq_mode = GNI_CQMODE_GLOBAL_EVENT | GNI_CQMODE_REMOTE_EVENT ;
    pd->dlvr_mode = GNI_DLVMODE_PERFORMANCE;
    pd->cqwrite_value = data;
    pd->remote_mem_hndl = mem_hndl;
    status = GNI_PostCqWrite(ep_hndl_array[destNode], pd);
    GNI_RC_CHECK("GNI_PostCqWrite", status);
}
#endif

// register memory for a message
// return mem handle
static gni_return_t  registerMessage(void *msg, int size, int seqno, gni_mem_handle_t *memh)
{
    gni_return_t status = GNI_RC_SUCCESS;

    if (!IsMemHndlZero(*memh)) return GNI_RC_SUCCESS;

#if CMK_PERSISTENT_COMM_PUT
      // persistent message is always registered
      // BIG_MSG small pieces do not have malloc chunk header
    if (IS_PERSISTENT_MEMORY(msg)) {
        *memh = GetMemHndl(msg);
        return GNI_RC_SUCCESS;
    }
#endif
    if(seqno == 0
#if CMK_PERSISTENT_COMM_PUT
         || seqno == PERSIST_SEQ
#endif
      )
    {
        if(IsMemHndlZero((GetMemHndl(msg))))
        {
            msg = (void*)(msg);
            status = registerMemory(GetMempoolBlockPtr(msg), GetMempoolsize(msg), &(GetMemHndl(msg)), rdma_rx_cqh);
            if(status == GNI_RC_SUCCESS)
                *memh = GetMemHndl(msg);
        }
        else {
            *memh = GetMemHndl(msg);
        }
    }
    else {
        //big msg, can not fit into memory pool, or persistent message
        status = registerMemory(msg, size, memh, NULL);
    }
    return status;
}

static void getPersistentMsgRequest(void* header, uint64_t inst_id, uint8_t tag, PCQueue bufferRdmaQueue )
{
#if   PERSISTENT_GET_BASE
    CONTROL_MSG         *request_msg;
    gni_return_t        status;
    gni_post_descriptor_t *pd;
    request_msg = (CONTROL_MSG *) header;

    MallocPostDesc(pd);
    pd->cqwrite_value = request_msg->seq_id;
    pd->first_operand = ALIGN64(request_msg->length); //  total length
    if(request_msg->length <= LRTS_GNI_RDMA_THRESHOLD)
        pd->type            = GNI_POST_FMA_GET;
    else
        pd->type            = GNI_POST_RDMA_GET;
    pd->cq_mode         = GNI_CQMODE_GLOBAL_EVENT ;
    pd->dlvr_mode       = GNI_DLVMODE_PERFORMANCE;
    pd->length          = ALIGN64(request_msg->length);
    pd->local_addr      = (uint64_t) request_msg->dest_addr;
    pd->local_mem_hndl  = request_msg->dest_mem_hndl;
    pd->remote_addr     = (uint64_t) request_msg->source_addr;
    pd->remote_mem_hndl = request_msg->source_mem_hndl;
    pd->src_cq_hndl     = 0;
    pd->rdma_mode       = 0;
    pd->amo_cmd         = (gni_fma_cmd_type_t)0;
#if REMOTE_EVENT
    bufferRdmaMsg(bufferRdmaQueue, inst_id, pd, request_msg->ack_index);
#else
    bufferRdmaMsg(bufferRdmaQueue, inst_id, pd, -1);
#endif

#endif
}

// for BIG_MSG called on receiver side for receiving control message
// LMSG_INIT_TAG
static void getLargeMsgRequest(void* header, uint64_t inst_id, uint8_t tag, PCQueue bufferRdmaQueue )
{
#if     USE_LRTS_MEMPOOL
    CONTROL_MSG         *request_msg;
    gni_return_t        status = GNI_RC_SUCCESS;
    void                *msg_data;
    gni_post_descriptor_t *pd;
    gni_mem_handle_t    msg_mem_hndl;
    int                 size, transaction_size, offset = 0;
    size_t              register_size = 0;

    // initial a get to transfer data from the sender side */
    request_msg = (CONTROL_MSG *) header;
    size = request_msg->total_length;
    MACHSTATE4(8, "GO Get request from %d (%d,%d, %d) \n", inst_id, buffered_send_msg, buffered_recv_msg, register_memory_size);
    MallocPostDesc(pd);
#if CMK_WITH_STATS
    pd->sync_flag_addr = 1000000 * CmiWallTimer(); //microsecond
#endif
    if(request_msg->seq_id < 2)   {
        MACHSTATE2(8, "%d seq id in get large msg requrest %d\n", CmiMyRank(), request_msg->seq_id);
#if CMK_SMP_TRACE_COMMTHREAD
        pd->sync_flag_addr = 1000000 * CmiWallTimer(); //microsecond
#endif
        msg_data = CmiAlloc(size);
        CmiSetMsgSeq(msg_data, 0);
        _MEMCHECK(msg_data);
    }
    else {
        offset = ONE_SEG*(request_msg->seq_id-1);
        msg_data = (char*)request_msg->dest_addr + offset;
    }

    pd->cqwrite_value = request_msg->seq_id;

    transaction_size = request_msg->seq_id == 0? ALIGN64(size) : ALIGN64(request_msg->length);
    SetMemHndlZero(pd->local_mem_hndl);
    status = registerMessage(msg_data, transaction_size, request_msg->seq_id, &pd->local_mem_hndl);
    if (status == GNI_RC_SUCCESS && request_msg->seq_id == 0) {
        if(NoMsgInRecv( (void*)(msg_data)))
            register_size = GetMempoolsize((void*)(msg_data));
    }

    pd->first_operand = ALIGN64(size);                   //  total length

    if(request_msg->total_length <= LRTS_GNI_RDMA_THRESHOLD)
        pd->type            = GNI_POST_FMA_GET;
    else
        pd->type            = GNI_POST_RDMA_GET;
    pd->cq_mode         = GNI_CQMODE_GLOBAL_EVENT;
    pd->dlvr_mode       = GNI_DLVMODE_PERFORMANCE;
    pd->length          = transaction_size;
    pd->local_addr      = (uint64_t) msg_data;
    pd->remote_addr     = request_msg->source_addr + offset;
    pd->remote_mem_hndl = request_msg->source_mem_hndl;

    if (tag == LMSG_OOB_INIT_TAG)
        pd->src_cq_hndl     = highprior_rdma_tx_cqh;
    else
    {
#if MULTI_THREAD_SEND
        pd->src_cq_hndl     = rdma_tx_cqh;
#else
        pd->src_cq_hndl     = 0;
#endif
    }

    pd->rdma_mode       = (gni_fma_cmd_type_t)0;
    pd->amo_cmd         = (gni_fma_cmd_type_t)0;
#if CMI_EXERT_RECV_RDMA_CAP
    if(status == GNI_RC_SUCCESS && RDMA_pending >= RDMA_cap ) status = GNI_RC_ERROR_RESOURCE;
#endif
    //memory registration success
    if(status == GNI_RC_SUCCESS && tag == LMSG_OOB_INIT_TAG )
    {
        CmiNodeLock lock = pd->type == GNI_POST_RDMA_GET?rdma_tx_cq_lock:default_tx_cq_lock;
        CMI_GNI_LOCK(lock)
#if REMOTE_EVENT
        if( request_msg->seq_id == 0)
        {
            pd->cq_mode |= GNI_CQMODE_REMOTE_EVENT;
            int sts = GNI_EpSetEventData(ep_hndl_array[inst_id], inst_id, ACK_EVENT(request_msg->ack_index));
            GNI_RC_CHECK("GNI_EpSetEventData", sts);
        }
#endif

#if CMK_WITH_STATS
        RDMA_TRY_SEND(pd->type)
#endif
        if(pd->type == GNI_POST_RDMA_GET)
        {
            status = GNI_PostRdma(ep_hndl_array[inst_id], pd);
        }
        else
        {
            status = GNI_PostFma(ep_hndl_array[inst_id],  pd);
        }
        CMI_GNI_UNLOCK(lock)

        if(status == GNI_RC_SUCCESS )
        {
#if CMI_EXERT_RECV_RDMA_CAP
            RDMA_pending++;
#endif
            if(pd->cqwrite_value == 0)
            {
#if MACHINE_DEBUG_LOG
                buffered_recv_msg += register_size;
                MACHSTATE4(8, "GO request from %d (%d,%d, %d)\n", inst_id, buffered_send_msg, buffered_recv_msg, register_memory_size);
#endif
                IncreaseMsgInRecv(msg_data);
#if CMK_SMP_TRACE_COMMTHREAD
                pd->sync_flag_value = 1000000 * CmiWallTimer(); //microsecond
#endif
            }
#if  CMK_WITH_STATS
            pd->sync_flag_value = 1000000 * CmiWallTimer(); //microsecond
            RDMA_TRANS_INIT(pd->type, pd->sync_flag_addr/1000000.0)
#endif
        }
    }else if (status != GNI_RC_SUCCESS)
    {
        SetMemHndlZero((pd->local_mem_hndl));
    }
        if(status == GNI_RC_ERROR_RESOURCE|| status == GNI_RC_ERROR_NOMEM || tag != LMSG_OOB_INIT_TAG)
    {
#if REMOTE_EVENT
        bufferRdmaMsg(bufferRdmaQueue, inst_id, pd, request_msg->ack_index);
#else
        bufferRdmaMsg(bufferRdmaQueue, inst_id, pd, -1);
#endif
    }else if (status != GNI_RC_SUCCESS) {
        // printf("source: %d pd:(%p,%p)(%p,%p) len:%d local:%x remote:%x\n", (int)inst_id, (pd->local_mem_hndl).qword1, (pd->local_mem_hndl).qword2, (pd->remote_mem_hndl).qword1, (pd->remote_mem_hndl).qword2, pd->length, pd->local_addr, pd->remote_addr);
        GNI_RC_CHECK("GetLargeAFter posting", status);
    }
#else
    CONTROL_MSG         *request_msg;
    gni_return_t        status;
    void                *msg_data;
    gni_post_descriptor_t *pd;
    RDMA_REQUEST        *rdma_request_msg;
    gni_mem_handle_t    msg_mem_hndl;
    //int source;
    // initial a get to transfer data from the sender side */
    request_msg = (CONTROL_MSG *) header;
    msg_data = CmiAlloc(request_msg->length);
    _MEMCHECK(msg_data);

    MEMORY_REGISTER(onesided_hnd, nic_hndl, msg_data, request_msg->length, &msg_mem_hndl, &omdh, NULL,  status)

    if (status == GNI_RC_INVALID_PARAM || status == GNI_RC_PERMISSION_ERROR)
    {
        GNI_RC_CHECK("Invalid/permission Mem Register in post", status);
    }

    MallocPostDesc(pd);
    if(request_msg->length <= LRTS_GNI_RDMA_THRESHOLD)
        pd->type            = GNI_POST_FMA_GET;
    else
        pd->type            = GNI_POST_RDMA_GET;
    pd->cq_mode         = GNI_CQMODE_GLOBAL_EVENT;// |  GNI_CQMODE_REMOTE_EVENT;
    pd->dlvr_mode       = GNI_DLVMODE_PERFORMANCE;
    pd->length          = ALIGN64(request_msg->length);
    pd->local_addr      = (uint64_t) msg_data;
    pd->remote_addr     = request_msg->source_addr;
    pd->remote_mem_hndl = request_msg->source_mem_hndl;
    if (tag == LMSG_OOB_INIT_TAG)
        pd->src_cq_hndl     = highprior_rdma_tx_cqh;
    else
    {
#if MULTI_THREAD_SEND
        pd->src_cq_hndl     = rdma_tx_cqh;
#else
        pd->src_cq_hndl     = 0;
#endif
    }
    pd->rdma_mode       = 0;
    pd->amo_cmd         = 0;

    //memory registration successful
    if(status == GNI_RC_SUCCESS)
    {
        pd->local_mem_hndl  = msg_mem_hndl;

        if(pd->type == GNI_POST_RDMA_GET)
        {
            CMI_GNI_LOCK(rdma_tx_cq_lock)
            status = GNI_PostRdma(ep_hndl_array[inst_id], pd);
            CMI_GNI_UNLOCK(rdma_tx_cq_lock)
        }
        else
        {
            CMI_GNI_LOCK(default_tx_cq_lock)
            status = GNI_PostFma(ep_hndl_array[inst_id],  pd);
            CMI_GNI_UNLOCK(default_tx_cq_lock)
        }

    }else
    {
        SetMemHndlZero(pd->local_mem_hndl);
    }
    if(status == GNI_RC_ERROR_RESOURCE|| status == GNI_RC_ERROR_NOMEM )
    {
        MallocRdmaRequest(rdma_request_msg);
        rdma_request_msg->next = 0;
        rdma_request_msg->destNode = inst_id;
        rdma_request_msg->pd = pd;
        PCQueuePush(sendRdmaBuf, (char*)rdma_request_msg);
    }else {
        GNI_RC_CHECK("AFter posting", status);
    }
#endif
}

#if CQWRITE
static void PumpCqWriteTransactions(void)
{

    gni_cq_entry_t          ev;
    gni_return_t            status;
    void                    *msg;
    int                     msg_size;
    while(1) {
        //CMI_GNI_LOCK(my_cq_lock)
        status = GNI_CqGetEvent(rdma_rx_cqh, &ev);
        //CMI_GNI_UNLOCK(my_cq_lock)
        if(status != GNI_RC_SUCCESS) break;
        msg = (void*) ( GNI_CQ_GET_DATA(ev) & 0xFFFFFFFFFFFFL);
#if CMK_PERSISTENT_COMM_PUT
#if PRINT_SYH
        printf(" %d CQ write event %p\n", myrank, msg);
#endif
        if (!IsMemHndlZero(MEMHFIELD(msg))) {
#if PRINT_SYH
            printf(" %d Persistent CQ write event %p\n", myrank, msg);
#endif
            CmiReference(msg);
            msg_size = CmiGetMsgSize(msg);
            CMI_CHECK_CHECKSUM(msg, msg_size);
            handleOneRecvedMsg(msg_size, msg);
            continue;
        }
#endif
#if ! USE_LRTS_MEMPOOL
       // MEMORY_DEREGISTER(onesided_hnd, nic_hndl, &(((ACK_MSG *)header)->source_mem_hndl), &omdh, ((ACK_MSG *)header)->length);
#else
        DecreaseMsgInSend(msg);
#endif
        if(NoMsgInSend(msg))
            buffered_send_msg -= GetMempoolsize(msg);
        CmiFree(msg);
    };
    if(status == GNI_RC_ERROR_RESOURCE)
    {
        GNI_RC_CHECK("rdma_rx_cq full too many ack", status);
    }
}
#endif

#if REMOTE_EVENT
static void PumpRemoteTransactions(gni_cq_handle_t rx_cqh)
{
    gni_cq_entry_t          ev;
    gni_return_t            status;
    void                    *msg;
    int                     inst_id, index, type, size;

#if CMI_PUMPREMOTETRANSACTIONS_CAP
    int                     pump_count = 0;
#endif
    while(1) {
#if CMI_PUMPREMOTETRANSACTIONS_CAP
        if (pump_count > PumpRemoteTransactions_cap) break;
#endif
        CMI_GNI_LOCK(global_gni_lock)
//        CMI_GNI_LOCK(rdma_tx_cq_lock)
        status = GNI_CqGetEvent(rx_cqh, &ev);
//        CMI_GNI_UNLOCK(rdma_tx_cq_lock)
        CMI_GNI_UNLOCK(global_gni_lock)

        if(status != GNI_RC_SUCCESS) break;

#if CMI_PUMPREMOTETRANSACTIONS_CAP
        pump_count ++;
#endif

        inst_id = GNI_CQ_GET_REM_INST_ID(ev);
        index = GET_INDEX(inst_id);
        type = GET_TYPE(inst_id);
        switch (type) {
        case 0:    // ACK
            CmiAssert(index>=0 && index<ackPool.size);
            CMI_GNI_LOCK(ackPool.lock);
            //CmiAssert(GetIndexType(ackPool, index) == 1);
            msg = GetIndexAddress(ackPool, index);
            CMI_GNI_UNLOCK(ackPool.lock);
#if PRINT_SYH
            MACHSTATE4(8,"[%d] PumpRemoteTransactions: ack: %lld index: %d type: %d.\n", myrank, msg, index, type);
#endif
#if ! USE_LRTS_MEMPOOL
           // MEMORY_DEREGISTER(onesided_hnd, nic_hndl, &(((ACK_MSG *)header)->source_mem_hndl), &omdh, ((ACK_MSG *)header)->length);
#else
            DecreaseMsgInSend(msg);
#endif
            if(NoMsgInSend(msg))
                buffered_send_msg -= GetMempoolsize(msg);
            CmiFree(msg);
            IndexPool_freeslot(&ackPool, index);
#if CMI_EXERT_SEND_LARGE_CAP
            SEND_large_pending--;
#endif
            break;
#if CMK_PERSISTENT_COMM_PUT
        case 1:  {    // PERSISTENT
            CmiLock(persistPool.lock);
            CmiAssert(GetIndexType(persistPool, index) == NONCHARM_SMSG);
            PersistentReceivesTable *slot = (PersistentReceivesTable *)GetIndexAddress(persistPool, index);
            CmiUnlock(persistPool.lock);
            msg = slot->destBuf[slot->addrIndex].destAddress;
            size = CmiGetMsgSize(msg);
            CmiReference(msg);
            CMI_CHECK_CHECKSUM(msg, size);
            handleOneRecvedMsg(size, (char*)msg);
            break;
            }
#endif
        default:
            fprintf(stderr, "[%d] PumpRemoteTransactions: unknown type: %d\n", myrank, type);
            CmiAbort("PumpRemoteTransactions: unknown type");
        }
    }
    if(status == GNI_RC_ERROR_RESOURCE)
    {
        printf("charm> Please use +useRecvQueue %d in your command line, if the error comes again, increase this number\n", REMOTE_QUEUE_ENTRIES*2);
        GNI_RC_CHECK("PumpRemoteTransactions: rx_cqh full", status);
    }
}
#endif

/* This code overlaps with code in machine-onesided.C in PumpOneSidedRDMATransactions() */
static void PumpLocalTransactions(gni_cq_handle_t my_tx_cqh, CmiNodeLock my_cq_lock)
{
    gni_cq_entry_t          ev;
    gni_return_t            status;
    uint64_t                type, inst_id;
    gni_post_descriptor_t   *tmp_pd;
    MSG_LIST                *ptr;
    CONTROL_MSG             *ack_msg_tmp;
    ACK_MSG                 *ack_msg;
    uint8_t                 msg_tag;
    SMSG_QUEUE         *queue = &smsg_queue;
#if CMI_PUMPLOCALTRANSACTIONS_CAP
    int         pump_count = 0;
    while(pump_count < PumpLocalTransactions_cap) {
        pump_count++;
#else
    while(1) {
#endif
        CMI_GNI_LOCK(my_cq_lock)
        status = GNI_CqGetEvent(my_tx_cqh, &ev);
        CMI_GNI_UNLOCK(my_cq_lock)
        if(status != GNI_RC_SUCCESS) break;

        type = GNI_CQ_GET_TYPE(ev);
        if (type == GNI_CQ_EVENT_TYPE_POST)
        {

#if CMI_EXERT_RECV_RDMA_CAP
            if(RDMA_pending <=0) CmiAbort(" pending error\n");
            RDMA_pending--;
#endif
            inst_id     = GNI_CQ_GET_INST_ID(ev);
#if PRINT_SYH
            printf("[%d] LocalTransactions localdone=%d\n", myrank,  lrts_local_done_msg);
#endif
            CMI_GNI_LOCK(my_cq_lock)
            status = GNI_GetCompleted(my_tx_cqh, ev, &tmp_pd);
            CMI_GNI_UNLOCK(my_cq_lock)

            switch (tmp_pd->type) {
#if CMK_PERSISTENT_COMM_PUT
            case GNI_POST_RDMA_PUT:
#if CMK_PERSISTENT_COMM_PUT && ! USE_LRTS_MEMPOOL
                MEMORY_DEREGISTER(onesided_hnd, nic_hndl, &tmp_pd->local_mem_hndl, &omdh, tmp_pd->length);
#endif
            case GNI_POST_FMA_PUT:
                if(tmp_pd->amo_cmd != 1) {
                    CmiFree((void *)tmp_pd->local_addr);
#if REMOTE_EVENT
                    FreePostDesc(tmp_pd);
                    continue;
#elif CQWRITE
                    sendCqWrite(inst_id, tmp_pd->remote_addr, tmp_pd->remote_mem_hndl);
                    FreePostDesc(tmp_pd);
                    continue;
#else
                    MallocControlMsg(ack_msg_tmp);
                    ack_msg_tmp->source_addr = tmp_pd->remote_addr;
                    ack_msg_tmp->source_mem_hndl    = tmp_pd->remote_mem_hndl;
                    ack_msg_tmp->length  = tmp_pd->length;
                    msg_tag = PUT_DONE_TAG;
#endif
                }
                break;
#endif
            case GNI_POST_RDMA_GET:
            case GNI_POST_FMA_GET:  {
                MACHSTATE2(8, "PumpLocal Get done %lld=>%lld\n", tmp_pd->local_addr, tmp_pd->remote_addr);
#if  ! USE_LRTS_MEMPOOL
                MallocControlMsg(ack_msg_tmp);
                ack_msg_tmp->source_addr = tmp_pd->remote_addr;
                ack_msg_tmp->source_mem_hndl    = tmp_pd->remote_mem_hndl;
                MEMORY_DEREGISTER(onesided_hnd, nic_hndl, &tmp_pd->local_mem_hndl, &omdh, tmp_pd->length)
                msg_tag = ACK_TAG;
#else
#if CMK_WITH_STATS
                RDMA_TRANS_DONE(tmp_pd->sync_flag_value/1000000.0)
#endif
                int seq_id = tmp_pd->cqwrite_value;
                if(seq_id > 0)      // BIG_MSG
                {
                    MEMORY_DEREGISTER(onesided_hnd, nic_hndl, &tmp_pd->local_mem_hndl, &omdh, tmp_pd->length);
                    MallocControlMsg(ack_msg_tmp);
                    ack_msg_tmp->source_addr = tmp_pd->remote_addr;
                    ack_msg_tmp->source_mem_hndl    = tmp_pd->remote_mem_hndl;
                    ack_msg_tmp->seq_id = seq_id;
                    ack_msg_tmp->dest_addr = tmp_pd->local_addr - ONE_SEG*(ack_msg_tmp->seq_id-1);
                    ack_msg_tmp->source_addr -= ONE_SEG*(ack_msg_tmp->seq_id-1);
                    ack_msg_tmp->length = tmp_pd->length;
                    ack_msg_tmp->total_length = tmp_pd->first_operand;     // total size
                    msg_tag = BIG_MSG_TAG;
                }
                else
                {
                    if(seq_id < 0)
                        CmiReference((void*)tmp_pd->local_addr);
                    msg_tag = ACK_TAG;
#if  !REMOTE_EVENT && !CQWRITE
                    MallocAckMsg(ack_msg);
                    ack_msg->source_addr = tmp_pd->remote_addr;
#endif
                }
#endif
                break;
            }
            case  GNI_POST_CQWRITE:
                   FreePostDesc(tmp_pd);
                   continue;
            default:
                CmiPrintf("type=%d\n", tmp_pd->type);
                CmiAbort("PumpLocalTransactions: unknown type!");
            }      /* end of switch */

            if (msg_tag == ACK_TAG) {
#if !REMOTE_EVENT
#if   !CQWRITE
                status = send_smsg_message(queue, inst_id, ack_msg, ACK_MSG_SIZE, msg_tag, 0, NULL, NONCHARM_SMSG, 0);
#if !CMK_SMSGS_FREE_AFTER_EVENT
                if (status == GNI_RC_SUCCESS) FreeAckMsg(ack_msg);
#endif
#else
                sendCqWrite(inst_id, tmp_pd->remote_addr, tmp_pd->remote_mem_hndl);
#endif
#endif
            }
            else {
                status = send_smsg_message(queue, inst_id, ack_msg_tmp, CONTROL_MSG_SIZE, msg_tag, 0, NULL, NONCHARM_SMSG, 0);
#if !CMK_SMSGS_FREE_AFTER_EVENT
                if (status == GNI_RC_SUCCESS) FreeControlMsg(ack_msg_tmp);
#endif
            }
#if CMK_PERSISTENT_COMM_PUT
            if (tmp_pd->type == GNI_POST_RDMA_GET || tmp_pd->type == GNI_POST_FMA_GET)
#endif
            {
                if( msg_tag == ACK_TAG){    //msg fit in mempool
#if PRINT_SYH
                    printf("PumpLocalTransactions: Normal msg transaction PE:%d==>%d\n", myrank, inst_id);
#endif
                    TRACE_COMM_CONTROL_CREATION((double)(tmp_pd->sync_flag_addr/1000000.0), (double)((tmp_pd->sync_flag_addr+1)/1000000.0), (double)((tmp_pd->sync_flag_addr+1)/1000000.0), (char*)tmp_pd->local_addr);
                    TRACE_COMM_CONTROL_CREATION((double)(tmp_pd->sync_flag_value/1000000.0), (double)((tmp_pd->sync_flag_value+1)/1000000.0), (double)((tmp_pd->sync_flag_value+1)/1000000.0), (char*)tmp_pd->local_addr);

                    //CmiAssert(SIZEFIELD((void*)(tmp_pd->local_addr)) <= tmp_pd->length);
                    DecreaseMsgInRecv((void*)tmp_pd->local_addr);
#if MACHINE_DEBUG_LOG
                    if(NoMsgInRecv((void*)(tmp_pd->local_addr)))
                        buffered_recv_msg -= GetMempoolsize((void*)(tmp_pd->local_addr));
                    MACHSTATE5(8, "GO Recv done ack send from %d (%d,%d, %d) tag=%d\n", inst_id, buffered_send_msg, buffered_recv_msg, register_memory_size, msg_tag);
#endif
                    CMI_CHECK_CHECKSUM((void*)tmp_pd->local_addr, CmiGetMsgSize(tmp_pd->local_addr));
                    handleOneRecvedMsg(CmiGetMsgSize(tmp_pd->local_addr), (char *)tmp_pd->local_addr);
                }else if(msg_tag == BIG_MSG_TAG){
                    void *msg = (char*)tmp_pd->local_addr-(tmp_pd->cqwrite_value-1)*ONE_SEG;
                    CmiSetMsgSeq(msg, CmiGetMsgSeq(msg)+1);
                    if (tmp_pd->first_operand <= ONE_SEG*CmiGetMsgSeq(msg)) {
                        START_EVENT();
#if PRINT_SYH
                        printf("Pipeline msg done [%d]\n", myrank);
#endif
#if     CMK_SMP_TRACE_COMMTHREAD
                        if( tmp_pd->cqwrite_value == 1)
                            TRACE_COMM_CONTROL_CREATION((double)(tmp_pd->sync_flag_addr/1000000.0), (double)((tmp_pd->sync_flag_addr+1)/1000000.0), (double)((tmp_pd->sync_flag_addr+2)/1000000.0), (char*)tmp_pd->local_addr);
#endif
                        CMI_CHECK_CHECKSUM(msg, CmiGetMsgSize(msg));
                        handleOneRecvedMsg(CmiGetMsgSize(msg), (char *)msg);
                    }
                }
            }
            FreePostDesc(tmp_pd);
        }
#if CMK_SMSGS_FREE_AFTER_EVENT
        else if  (type == GNI_CQ_EVENT_TYPE_SMSG) {
            // a SmsgsSend is done
            int msgid = GNI_CQ_GET_MSG_ID(ev);
            int type = GetIndexType(smsgsPool, msgid);
            void *addr = GetIndexAddress(smsgsPool, msgid);
            switch (type) {
            case CHARM_SMSG:
                CmiFree(addr);
                break;
            case NONCHARM_SMSG:
                free(addr);
                break;
            case SMSG_DONT_FREE:
                break;
            default:
                CmiAbort("Invalid SmsgsIndex");
            }
            IndexPool_freeslot(&smsgsPool, msgid);
        }
#endif
    } //end while
    if(status == GNI_RC_ERROR_RESOURCE)
    {
        printf("charm> Please use +useSendQueue 204800 in your command line, if the error comes again, increase this number\n");
        GNI_RC_CHECK("Smsg_tx_cq full", status);
    }
}

static void  SendRdmaMsg( BufferList sendqueue)
{
    gni_return_t            status = GNI_RC_SUCCESS;
    gni_mem_handle_t        msg_mem_hndl;
    RDMA_REQUEST            *ptr = 0, *tmp_ptr;
    RDMA_REQUEST            *pre = 0;
    uint64_t                register_size = 0;
    void                    *msg;
    int                     i;

    int len = PCQueueLength(sendqueue);
    for (i=0; i<len; i++)
    {
#if CMI_EXERT_RECV_RDMA_CAP
        if( RDMA_pending >= RDMA_cap) break;
#endif
        CMI_PCQUEUEPOP_LOCK( sendqueue)
        ptr = (RDMA_REQUEST*)PCQueuePop(sendqueue);
        CMI_PCQUEUEPOP_UNLOCK( sendqueue)
        if (ptr == NULL) break;

        gni_post_descriptor_t *pd = ptr->pd;

        msg = (void*)(pd->local_addr);
        status = registerMessage(msg, pd->length, pd->cqwrite_value, &pd->local_mem_hndl);
        register_size = 0;
        if(pd->cqwrite_value == 0) {
            if(NoMsgInRecv(msg))
                register_size = GetMempoolsize(msg);
        }

        if(status == GNI_RC_SUCCESS)        //mem register good
        {
            int destNode = ptr->destNode;
            CmiNodeLock lock = (pd->type == GNI_POST_RDMA_GET || pd->type == GNI_POST_RDMA_PUT) ? rdma_tx_cq_lock:default_tx_cq_lock;
            CMI_GNI_LOCK(lock);
#if REMOTE_EVENT
            if( pd->cqwrite_value == 0 || pd->cqwrite_value == -1) {
                pd->cq_mode |= GNI_CQMODE_REMOTE_EVENT;
                int sts = GNI_EpSetEventData(ep_hndl_array[destNode], destNode, ACK_EVENT(ptr->ack_index));
                GNI_RC_CHECK("GNI_EpSetEventData", sts);
            }
#if CMK_PERSISTENT_COMM_PUT
            else if (pd->cqwrite_value == PERSIST_SEQ) {
                pd->cq_mode |= GNI_CQMODE_REMOTE_EVENT;
                int sts = GNI_EpSetEventData(ep_hndl_array[destNode], destNode, PERSIST_EVENT(ptr->ack_index));
                GNI_RC_CHECK("GNI_EpSetEventData", sts);
            }
#endif
#endif

#if CMK_WITH_STATS
            RDMA_TRY_SEND(pd->type)
#endif
#if CMK_SMP_TRACE_COMMTHREAD
            if(IS_PUT(pd->type))
            {
                 START_EVENT();
                 TRACE_COMM_CREATION(EVENT_TIME(), (char*)pd->local_addr);//based on assumption, post always succeeds on first try
            }
#endif
            if(pd->type == GNI_POST_RDMA_GET || pd->type == GNI_POST_RDMA_PUT)
            {
                status = GNI_PostRdma(ep_hndl_array[destNode], pd);
            }
            else
            {
                status = GNI_PostFma(ep_hndl_array[destNode],  pd);
            }
            CMI_GNI_UNLOCK(lock);

            if(status == GNI_RC_SUCCESS)    //post good
            {
                MACHSTATE4(8, "post noempty-rdma  %d (%lld==%lld,%d) \n", ptr->destNode, pd->local_addr, pd->remote_addr,  register_memory_size);
#if CMI_EXERT_RECV_RDMA_CAP
                RDMA_pending ++;
#endif
                if(pd->cqwrite_value <= 0)
                {
#if CMK_SMP_TRACE_COMMTHREAD
                    pd->sync_flag_value = 1000000 * CmiWallTimer(); //microsecond
#endif
                    IncreaseMsgInRecv(((void*)(pd->local_addr)));
                }
#if  CMK_WITH_STATS
                pd->sync_flag_value = 1000000 * CmiWallTimer(); //microsecond
                RDMA_TRANS_INIT(pd->type, pd->sync_flag_addr/1000000.0)
#endif
#if MACHINE_DEBUG_LOG
                buffered_recv_msg += register_size;
                MACHSTATE(8, "GO request from buffered\n");
#endif
#if PRINT_SYH
                printf("[%d] SendRdmaMsg: post succeed. seqno: %d\n", myrank, pd->cqwrite_value);
#endif
                FreeRdmaRequest(ptr);
            }else           // cannot post
            {
                PCQueuePush(sendRdmaBuf, (char*)ptr);
#if PRINT_SYH
                printf("[%d] SendRdmaMsg: post failed. seqno: %x dest: %d local mhdl: %lld %lld remote mhdl: %lld %lld connect: %d\n", myrank, pd->cqwrite_value, destNode, pd->local_mem_hndl.qword1, pd->local_mem_hndl.qword2, pd->remote_mem_hndl.qword1, pd->remote_mem_hndl.qword2, smsg_connected_flag[destNode]);
#endif
                break;
            }
        } else          //memory registration fails
        {
            PCQueuePush(sendqueue, (char*)ptr);
        }
    } //end while
}

static
INLINE_KEYWORD gni_return_t _sendOneBufferedSmsg(SMSG_QUEUE *queue, MSG_LIST *ptr)
{
    CONTROL_MSG         *control_msg_tmp;
    gni_return_t        status = GNI_RC_ERROR_RESOURCE;
    int                 numRdmaOps, recvInfoSize, msgSize, msgMode;
    NcpyOperationInfo *ncpyOpInfo;

    MACHSTATE5(8, "noempty-smsg  %d (%d,%d,%d) tag=%d \n", ptr->destNode, buffered_send_msg, buffered_recv_msg, register_memory_size, ptr->tag);
    if (useDynamicSMSG && smsg_connected_flag[ptr->destNode] != 2) {
            /* connection not exists yet */
#if CMK_SMP
            /* non-smp case, connect is issued in send_smsg_message */
        if (smsg_connected_flag[ptr->destNode] == 0)
            connect_to(ptr->destNode);
#endif
    }
    else
    switch(ptr->tag)
    {
    case SMALL_DATA_TAG:
        status = send_smsg_message(queue, ptr->destNode,  ptr->msg, ptr->size, ptr->tag, 1, ptr, CHARM_SMSG, 0);
#if !CMK_SMSGS_FREE_AFTER_EVENT
        if(status == GNI_RC_SUCCESS)
        {
            CmiFree(ptr->msg);
        }
#endif
        break;
    case LMSG_PERSISTENT_INIT_TAG:
    case LMSG_INIT_TAG:
    case LMSG_OOB_INIT_TAG:
        control_msg_tmp = (CONTROL_MSG*)ptr->msg;
        status = send_large_messages(queue, ptr->destNode, control_msg_tmp, 1, ptr, ptr->tag);
        break;
#if !REMOTE_EVENT && !CQWRITE
    case ACK_TAG:
        status = send_smsg_message(queue, ptr->destNode, ptr->msg, ptr->size, ptr->tag, 1, ptr, NONCHARM_SMSG, 0);
#if !CMK_SMSGS_FREE_AFTER_EVENT
        if(status == GNI_RC_SUCCESS) FreeAckMsg((ACK_MSG*)ptr->msg);
#endif
        break;
#endif
    case BIG_MSG_TAG:
        status = send_smsg_message(queue, ptr->destNode, ptr->msg, ptr->size, ptr->tag, 1, ptr, NONCHARM_SMSG, 0);
#if !CMK_SMSGS_FREE_AFTER_EVENT
        if(status == GNI_RC_SUCCESS)
        {
            FreeControlMsg((CONTROL_MSG*)ptr->msg);
        }
#endif
        break;
#if CMK_PERSISTENT_COMM_PUT && !REMOTE_EVENT && !CQWRITE
    case PUT_DONE_TAG:
        status = send_smsg_message(queue, ptr->destNode, ptr->msg, ptr->size, ptr->tag, 1, ptr, NONCHARM_SMSG, 0);
#if !CMK_SMSGS_FREE_AFTER_EVENT
        if(status == GNI_RC_SUCCESS)
        {
            FreeControlMsg((CONTROL_MSG*)ptr->msg);
        }
#endif
        break;
#endif

     case RDMA_PUT_MD_DIRECT_TAG:
     case RDMA_REG_AND_GET_MD_DIRECT_TAG:
     case RDMA_REG_AND_PUT_MD_DIRECT_TAG:
     case RDMA_DEREG_AND_ACK_MD_DIRECT_TAG:
        ncpyOpInfo = (NcpyOperationInfo *)(ptr->msg);
        msgMode = (ncpyOpInfo->freeMe == CMK_FREE_NCPYOPINFO) ? CHARM_SMSG : SMSG_DONT_FREE;
        status = send_smsg_message(queue, ptr->destNode, ptr->msg, ncpyOpInfo->ncpyOpInfoSize, ptr->tag, 1, ptr, msgMode, 1);
#if !CMK_SMSGS_FREE_AFTER_EVENT
        if(status == GNI_RC_SUCCESS && ncpyOpInfo->freeMe == CMK_FREE_NCPYOPINFO) {
          CmiFree(ptr->msg);
        }
#endif
        break;

    default:
        printf("Weird tag: %d\n", ptr->tag);
        CmiAbort("should not happen\n");
    }       // end switch
    return status;
}

// return 1 if all messages are sent

#if ONE_SEND_QUEUE

static int SendBufferMsg(SMSG_QUEUE *queue, SMSG_QUEUE *prio_queue)
{
    MSG_LIST            *ptr, *tmp_ptr, *pre=0, *current_head;
    CONTROL_MSG         *control_msg_tmp;
    gni_return_t        status;
    int                 done = 1;
    uint64_t            register_size;
    void                *register_addr;
    int                 index_previous = -1;
#if     CMI_SENDBUFFERSMSG_CAP
    int                 sent_length = 0;
#endif
    int          index = 0;
    memset(destpe_avail, 0, mysize * sizeof(char));
    for (index=0; index<1; index++)
    {
#if CMK_DEBUG
        CmiPrintf("[%d] Called SendBufferMsg\n", CmiMyPe());
#endif
        int i, len;
#if CMK_LOCKLESS_QUEUE
        len = MPMCQueueLength(queue->sendMsgBuf);
#else
        len = PCQueueLength(queue->sendMsgBuf);
#endif
        for (i=0; i<len; i++)
        {
#if CMK_LOCKLESS_QUEUE
            ptr = (MSG_LIST*)MPMCQueuePop(queue->sendMsgBuf);
#else
            CMI_PCQUEUEPOP_LOCK(queue->sendMsgBuf)
            ptr = (MSG_LIST*)PCQueuePop(queue->sendMsgBuf);
            CMI_PCQUEUEPOP_UNLOCK(queue->sendMsgBuf)
#endif
            if(ptr == NULL) break;
            if (destpe_avail[ptr->destNode] == 1) {       /* can't send to this pe */
#if CMK_LOCKLESS_QUEUE
                MPMCQueuePush(queue->sendMsgBuf, (char*)ptr);
#else
                PCQueuePush(queue->sendMsgBuf, (char*)ptr);
#endif
                continue;
            }
#if CMK_SMP
            if (ptr->tag == RDMA_COMM_PERFORM_GET_TAG) {
                // Comm thread performing GET on behalf of worker thread for Direct API
                _performOneRgetForWorkerThread(ptr);
                FreeMsgList(ptr);
                continue;
            }
            else if (ptr->tag == RDMA_COMM_PERFORM_PUT_TAG) {
                // Comm thread performing PUT on behalf of worker thread for Direct API
                _performOneRputForWorkerThread(ptr);
                FreeMsgList(ptr);
                continue;
            }
#endif
            status = _sendOneBufferedSmsg(queue, ptr);
#if CMI_SENDBUFFERSMSG_CAP
            sent_length++;
#endif
            if(status == GNI_RC_SUCCESS)
            {
#if PRINT_SYH
                buffered_smsg_counter--;
                printf("[%d==>%d] buffered smsg sending done\n", myrank, ptr->destNode);
#endif
                FreeMsgList(ptr);
            }else {
#if CMK_LOCKLESS_QUEUE
                MPMCQueuePush(queue->sendMsgBuf, (char*)ptr);
#else
                PCQueuePush(queue->sendMsgBuf, (char*)ptr);
#endif
                done = 0;
                if(status == GNI_RC_ERROR_RESOURCE)
                {
                    destpe_avail[ptr->destNode] = 1;
                }
            }
        } //end while
    }   // end pooling for all cores
    return done;
}

#else   /*  ! ONE_SEND_QUEUE  */

static int SendBufferMsg(SMSG_QUEUE *queue, SMSG_QUEUE *prio_queue)
{
    MSG_LIST            *ptr;
    gni_return_t        status;
    int                 done = 1;
#if     CMI_SENDBUFFERSMSG_CAP
    int                 sent_length = 0;
#endif
    int idx;
#if SMP_LOCKS
    int          index = -1;
    int nonempty = PCQueueLength(queue->nonEmptyQueues);
    for(idx =0; idx<nonempty; idx++)
    {
        index++;  if (index >= nonempty) index = 0;
#if CMI_SENDBUFFERSMSG_CAP
        if ( sent_length >= SendBufferMsg_cap) { done = 0; return done;}
#endif
        CMI_PCQUEUEPOP_LOCK(queue->nonEmptyQueues)
        MSG_LIST_INDEX *current_list = (MSG_LIST_INDEX *)PCQueuePop(queue->nonEmptyQueues);
        CMI_PCQUEUEPOP_UNLOCK(queue->nonEmptyQueues)
        if(current_list == NULL) break;
        if (prio_queue && PCQueueLength(prio_queue->smsg_msglist_index[current_list->destpe].sendSmsgBuf) != 0) {
            PCQueuePush(queue->nonEmptyQueues, (char*)current_list);
            continue;
        }
        PCQueue current_queue= current_list->sendSmsgBuf;
        CmiLock(current_list->lock);
        int i, len = PCQueueLength(current_queue);
        current_list->pushed = 0;
        CmiUnlock(current_list->lock);
#else      /* ! SMP_LOCKS */
    static int          index = -1;
    for(idx =0; idx<mysize; idx++)
    {
        index++;  if (index == mysize) index = 0;
#if CMI_SENDBUFFERSMSG_CAP
        if ( sent_length >= SendBufferMsg_cap) { done = 0; return done;}
#endif
        if (prio_queue && PCQueueLength(prio_queue->smsg_msglist_index[index].sendSmsgBuf) != 0) continue;             // check urgent queue
        //if (index == myrank) continue;
        PCQueue current_queue = queue->smsg_msglist_index[index].sendSmsgBuf;
        int i, len = PCQueueLength(current_queue);
#endif
        for (i=0; i<len; i++)  {
            CMI_PCQUEUEPOP_LOCK(current_queue)
            ptr = (MSG_LIST*)PCQueuePop(current_queue);
            CMI_PCQUEUEPOP_UNLOCK(current_queue)
            if (ptr == 0) break;
#if CMK_SMP && CMK_ONESIDED_IMPL
            if (ptr->tag == RDMA_COMM_PERFORM_GET_TAG) {
                // Comm thread performing GET on behalf of worker thread for Direct API
                _performOneRgetForWorkerThread(ptr);
                FreeMsgList(ptr);
                continue;
            }
            else if (ptr->tag == RDMA_COMM_PERFORM_PUT_TAG) {
                // Comm thread performing PUT on behalf of worker thread for Direct API
                _performOneRputForWorkerThread(ptr);
                FreeMsgList(ptr);
                continue;
            }
#endif
            status = _sendOneBufferedSmsg(queue, ptr);
#if CMI_SENDBUFFERSMSG_CAP
            sent_length++;
#endif
            if(status == GNI_RC_SUCCESS)
            {
#if PRINT_SYH
                buffered_smsg_counter--;
                printf("[%d==>%d] buffered smsg sending done\n", myrank, ptr->destNode);
#endif
                FreeMsgList(ptr);
            }else {
                PCQueuePush(current_queue, (char*)ptr);
                done = 0;
                if(status == GNI_RC_ERROR_RESOURCE)
                {
                    break;
                }
            }
        } //end for i
#if SMP_LOCKS
        CmiLock(current_list->lock);
        if(!PCQueueEmpty(current_queue) && current_list->pushed == 0)
        {
            current_list->pushed = 1;
            PCQueuePush(queue->nonEmptyQueues, (char*)current_list);
        }
        CmiUnlock(current_list->lock);
#endif
    }   // end pooling for all cores
    return done;
}

#endif

static void ProcessDeadlock(void);
void LrtsAdvanceCommunication(int whileidle)
{
    static int count = 0;
    /*  Receive Msg first */
#if CMK_SMP_TRACE_COMMTHREAD
    double startT, endT;
#endif
    if (useDynamicSMSG && whileidle)
    {
#if CMK_SMP_TRACE_COMMTHREAD
        startT = CmiWallTimer();
#endif
        STATS_PUMPDATAGRAMCONNECTION_TIME(PumpDatagramConnection());
#if CMK_SMP_TRACE_COMMTHREAD
        endT = CmiWallTimer();
        if (endT-startT>=TRACE_THRESHOLD) traceUserBracketEvent(event_SetupConnect, startT, endT);
#endif
    }

    SEND_OOB_SMSG(smsg_oob_queue)
    PUMP_REMOTE_HIGHPRIORITY
    PUMP_LOCAL_HIGHPRIORITY
    POST_HIGHPRIORITY_RDMA
    // Receiving small messages and persistent
#if CMK_SMP_TRACE_COMMTHREAD
    startT = CmiWallTimer();
#endif
    STATS_PUMPNETWORK_TIME(PumpNetworkSmsg());
#if CMK_SMP_TRACE_COMMTHREAD
    endT = CmiWallTimer();
    if (endT-startT>=TRACE_THRESHOLD) traceUserBracketEvent(event_PumpSmsg, startT, endT);
#endif

    SEND_OOB_SMSG(smsg_oob_queue)
    PUMP_REMOTE_HIGHPRIORITY
    PUMP_LOCAL_HIGHPRIORITY
    POST_HIGHPRIORITY_RDMA

    ///* Send buffered Message */
#if CMK_SMP_TRACE_COMMTHREAD
    startT = CmiWallTimer();
#endif
#if CMK_USE_OOB
    STATS_SEND_SMSGS_TIME(SendBufferMsg(&smsg_queue, &smsg_oob_queue));
#else
    STATS_SEND_SMSGS_TIME(SendBufferMsg(&smsg_queue, NULL));
#endif
#if CMK_SMP_TRACE_COMMTHREAD
    endT = CmiWallTimer();
    if (endT-startT>=TRACE_THRESHOLD) traceUserBracketEvent(event_SendBufferSmsg, startT, endT);
#endif

    SEND_OOB_SMSG(smsg_oob_queue)
    PUMP_REMOTE_HIGHPRIORITY
    PUMP_LOCAL_HIGHPRIORITY
    POST_HIGHPRIORITY_RDMA

    //Pump Get messages or PUT messages
#if CMK_SMP_TRACE_COMMTHREAD
    startT = CmiWallTimer();
#endif
    PumpLocalTransactions(default_tx_cqh, default_tx_cq_lock);
#if MULTI_THREAD_SEND
    STATS_PUMPLOCALTRANSACTIONS_RDMA_TIME(PumpLocalTransactions(rdma_tx_cqh,  rdma_tx_cq_lock));
#endif
#if CMK_SMP_TRACE_COMMTHREAD
    endT = CmiWallTimer();
    if (endT-startT>=TRACE_THRESHOLD) traceUserBracketEvent(event_PumpTransaction, startT, endT);
#endif

    SEND_OOB_SMSG(smsg_oob_queue)
    PUMP_REMOTE_HIGHPRIORITY
    PUMP_LOCAL_HIGHPRIORITY
    POST_HIGHPRIORITY_RDMA
    //Pump Remote event
#if CMK_SMP_TRACE_COMMTHREAD
    startT = CmiWallTimer();
#endif
#if CQWRITE
    PumpCqWriteTransactions();
#endif
#if CMK_ONESIDED_IMPL
    PumpOneSidedRDMATransactions(rdma_onesided_cqh, rdma_onesided_cq_lock);
#endif
#if REMOTE_EVENT
    STATS_PUMPREMOTETRANSACTIONS_TIME(PumpRemoteTransactions(rdma_rx_cqh));
#endif
#if CMK_SMP_TRACE_COMMTHREAD
    endT = CmiWallTimer();
    if (endT-startT>=TRACE_THRESHOLD) traceUserBracketEvent(event_PumpRdmaTransaction, startT, endT);
#endif

    SEND_OOB_SMSG(smsg_oob_queue)
    PUMP_REMOTE_HIGHPRIORITY
    PUMP_LOCAL_HIGHPRIORITY
    POST_HIGHPRIORITY_RDMA

#if CMK_SMP_TRACE_COMMTHREAD
    startT = CmiWallTimer();
#endif
    STATS_SENDRDMAMSG_TIME(SendRdmaMsg(sendRdmaBuf));
#if CMK_SMP_TRACE_COMMTHREAD
    endT = CmiWallTimer();
    if (endT-startT>=TRACE_THRESHOLD) traceUserBracketEvent(event_SendFmaRdmaMsg, startT, endT);
#endif

#if CMK_SMP && ! LARGEPAGE
    if (_detected_hang)  ProcessDeadlock();
#endif
}

static void set_smsg_max(void)
{
    char *env;

    if(mysize <=512)
    {
        SMSG_MAX_MSG = 1024;
    }else if (mysize <= 4096)
    {
        SMSG_MAX_MSG = 1024;
    }else if (mysize <= 16384)
    {
        SMSG_MAX_MSG = 512;
    }else {
        SMSG_MAX_MSG = 256;
    }

    env = getenv("CHARM_UGNI_SMSG_MAX_SIZE");
    if (env) SMSG_MAX_MSG = atoi(env);
    CmiAssert(SMSG_MAX_MSG > 0);
}

/* useDynamicSMSG */
static void _init_dynamic_smsg(void)
{
    gni_return_t status;
    uint32_t     vmdh_index = -1;
    int i;

    smsg_attr_vector_local = (gni_smsg_attr_t**)malloc(mysize * sizeof(gni_smsg_attr_t*));
    smsg_attr_vector_remote = (gni_smsg_attr_t**)malloc(mysize * sizeof(gni_smsg_attr_t*));
    smsg_connected_flag = (int*)malloc(sizeof(int)*mysize);
    for(i=0; i<mysize; i++) {
        smsg_connected_flag[i] = 0;
        smsg_attr_vector_local[i] = NULL;
        smsg_attr_vector_remote[i] = NULL;
    }

    set_smsg_max();

    send_smsg_attr.msg_type = GNI_SMSG_TYPE_MBOX_AUTO_RETRANSMIT;
    send_smsg_attr.mbox_maxcredit = SMSG_MAX_CREDIT;
    send_smsg_attr.msg_maxsize = SMSG_MAX_MSG;
    status = GNI_SmsgBufferSizeNeeded(&send_smsg_attr, &smsg_memlen);
    GNI_RC_CHECK("GNI_GNI_MemRegister mem buffer", status);

    mailbox_list = (dynamic_smsg_mailbox_t*)malloc(sizeof(dynamic_smsg_mailbox_t));
    mailbox_list->size = smsg_memlen*avg_smsg_connection;
    posix_memalign(&mailbox_list->mailbox_base, 64, mailbox_list->size);
    memset(mailbox_list->mailbox_base, 0, mailbox_list->size);
    mailbox_list->offset = 0;
    mailbox_list->next = 0;

    status = GNI_MemRegister(nic_hndl, (uint64_t)(mailbox_list->mailbox_base),
        mailbox_list->size, smsg_rx_cqh,
        GNI_MEM_READWRITE,
        vmdh_index,
        &(mailbox_list->mem_hndl));
    GNI_RC_CHECK("MEMORY registration for smsg", status);

    status = GNI_EpCreate(nic_hndl, default_tx_cqh, &ep_hndl_unbound);
    GNI_RC_CHECK("Unbound EP", status);

    alloc_smsg_attr(&send_smsg_attr);

    status = GNI_EpPostDataWId (ep_hndl_unbound, &send_smsg_attr,  SMSG_ATTR_SIZE, &recv_smsg_attr, SMSG_ATTR_SIZE, myrank);
    GNI_RC_CHECK("post unbound datagram", status);

      /* always pre-connect to proc 0 */
    //if (myrank != 0) connect_to(0);

    status = GNI_SmsgSetMaxRetrans(nic_hndl, 4096);
    GNI_RC_CHECK("SmsgSetMaxRetrans Init", status);
}

static void _init_static_smsg(void)
{
    gni_smsg_attr_t      *smsg_attr;
    gni_smsg_attr_t      remote_smsg_attr;
    gni_smsg_attr_t      *smsg_attr_vec;
    gni_mem_handle_t     my_smsg_mdh_mailbox;
    int      ret, i;
    gni_return_t status;
    uint32_t              vmdh_index = -1;
    mdh_addr_t            base_infor;
    mdh_addr_t            *base_addr_vec;

    set_smsg_max();

    smsg_attr = (gni_smsg_attr_t *)malloc(mysize * sizeof(gni_smsg_attr_t));

    smsg_attr[0].msg_type = GNI_SMSG_TYPE_MBOX_AUTO_RETRANSMIT;
    smsg_attr[0].mbox_maxcredit = SMSG_MAX_CREDIT;
    smsg_attr[0].msg_maxsize = SMSG_MAX_MSG;
    status = GNI_SmsgBufferSizeNeeded(&smsg_attr[0], &smsg_memlen);
    GNI_RC_CHECK("GNI_GNI_MemRegister mem buffer", status);
    ret = posix_memalign(&smsg_mailbox_base, 64, smsg_memlen*(mysize));
    CmiAssert(ret == 0);
    memset(smsg_mailbox_base, 0, smsg_memlen*(mysize));

    status = GNI_MemRegister(nic_hndl, (uint64_t)smsg_mailbox_base,
            smsg_memlen*(mysize), smsg_rx_cqh,
            GNI_MEM_READWRITE,
            vmdh_index,
            &my_smsg_mdh_mailbox);
    register_memory_size += smsg_memlen*(mysize);
    GNI_RC_CHECK("GNI_GNI_MemRegister mem buffer", status);

    if (myrank == 0 && !quietMode)  printf("Charm++> SMSG memory: %1.1fKB\n", 1.0*smsg_memlen*(mysize)/1024);
    if (myrank == 0 && register_memory_size>=MAX_REG_MEM ) printf("Charm++> FATAL ERROR your program has risk of hanging \n please set CHARM_UGNI_MEMPOOL_MAX  to a larger value or use Dynamic smsg\n");

    base_infor.addr =  (uint64_t)smsg_mailbox_base;
    base_infor.mdh =  my_smsg_mdh_mailbox;
    base_addr_vec = (mdh_addr_t *)malloc(mysize * sizeof(mdh_addr_t));

    allgather(&base_infor, base_addr_vec,  sizeof(mdh_addr_t));

    for(i=0; i<mysize; i++)
    {
        if(i==myrank)
            continue;
        smsg_attr[i].msg_type = GNI_SMSG_TYPE_MBOX_AUTO_RETRANSMIT;
        smsg_attr[i].mbox_maxcredit = SMSG_MAX_CREDIT;
        smsg_attr[i].msg_maxsize = SMSG_MAX_MSG;
        smsg_attr[i].mbox_offset = i*smsg_memlen;
        smsg_attr[i].buff_size = smsg_memlen;
        smsg_attr[i].msg_buffer = smsg_mailbox_base ;
        smsg_attr[i].mem_hndl = my_smsg_mdh_mailbox;
    }

    for(i=0; i<mysize; i++)
    {
        if (myrank == i) continue;

        remote_smsg_attr.msg_type = GNI_SMSG_TYPE_MBOX_AUTO_RETRANSMIT;
        remote_smsg_attr.mbox_maxcredit = SMSG_MAX_CREDIT;
        remote_smsg_attr.msg_maxsize = SMSG_MAX_MSG;
        remote_smsg_attr.mbox_offset = myrank*smsg_memlen;
        remote_smsg_attr.buff_size = smsg_memlen;
        remote_smsg_attr.msg_buffer = (void*)base_addr_vec[i].addr;
        remote_smsg_attr.mem_hndl = base_addr_vec[i].mdh;

        /* initialize the smsg channel */
        status = GNI_SmsgInit(ep_hndl_array[i], &smsg_attr[i], &remote_smsg_attr);
        GNI_RC_CHECK("SMSG Init", status);
    } //end initialization

    free(base_addr_vec);
    free(smsg_attr);

    status = GNI_SmsgSetMaxRetrans(nic_hndl, 4096);
    GNI_RC_CHECK("SmsgSetMaxRetrans Init", status);
}

INLINE_KEYWORD
static void _init_send_queue(SMSG_QUEUE *queue)
{
     int i;
#if ONE_SEND_QUEUE
#if CMK_LOCKLESS_QUEUE
     queue->sendMsgBuf = MPMCQueueCreate();
#else
     queue->sendMsgBuf = PCQueueCreate();
#endif
     destpe_avail = (char*)malloc(mysize * sizeof(char));
#else
     queue->smsg_msglist_index = (MSG_LIST_INDEX*)malloc(mysize*sizeof(MSG_LIST_INDEX));
#if SMP_LOCKS
     queue->nonEmptyQueues = PCQueueCreate();
#endif
     for(i =0; i<mysize; i++)
     {
         queue->smsg_msglist_index[i].sendSmsgBuf = PCQueueCreate();
#if SMP_LOCKS
         queue->smsg_msglist_index[i].pushed = 0;
         queue->smsg_msglist_index[i].lock = CmiCreateLock();
         queue->smsg_msglist_index[i].destpe = i;
#endif
     }
#endif
}

INLINE_KEYWORD
static void _init_smsg(void)
{
    if(mysize > 1) {
        if (useDynamicSMSG)
            _init_dynamic_smsg();
        else
            _init_static_smsg();
    }

    _init_send_queue(&smsg_queue);
#if CMK_USE_OOB
    _init_send_queue(&smsg_oob_queue);
#endif
}

static void _init_static_msgq(void)
{
    gni_return_t status;
    /* MSGQ is to send and receive short messages for large jobs (exceeding 200,000 ranks). The          performance scales by the node count rather than rank count */
    msgq_attrs.max_msg_sz = MSGQ_MAXSIZE;
    msgq_attrs.smsg_q_sz = 1;
    msgq_attrs.rcv_pool_sz = 1;
    msgq_attrs.num_msgq_eps = 2;
    msgq_attrs.nloc_insts = 8;
    msgq_attrs.modes = 0;
    msgq_attrs.rcv_cq_sz = REMOTE_QUEUE_ENTRIES ;

    status = GNI_MsgqInit(nic_hndl, NULL, NULL, NULL, &msgq_attrs, &msgq_handle);
    GNI_RC_CHECK("MSGQ Init", status);


}


static CmiUInt8 total_mempool_size = 0;
static CmiUInt8 total_mempool_calls = 0;

#if USE_LRTS_MEMPOOL

INLINE_KEYWORD
static void *_alloc_mempool_block(size_t *size, gni_mem_handle_t *mem_hndl, int expand_flag, gni_cq_handle_t cqh)
{
    void *pool;
    int ret;
    gni_return_t status = GNI_RC_SUCCESS;

    size_t default_size =  expand_flag? _expand_mem : _mempool_size;
    if (*size < default_size) *size = default_size;
#if LARGEPAGE
    // round up to be multiple of _tlbpagesize
    //*size = (*size + _tlbpagesize - 1)/_tlbpagesize*_tlbpagesize;
    *size = ALIGNHUGEPAGE(*size);
#endif
    total_mempool_size += *size;
    total_mempool_calls += 1;
#if   !LARGEPAGE
    if ((*size > MAX_REG_MEM || *size > MAX_BUFF_SEND) && expand_flag)
    {
        printf("Error: A mempool block with size %lld is allocated, which is greater than the maximum mempool allowed.\n Please increase the max pool size by using +gni-mempool-max or set enviorment variable CHARM_UGNI_MEMPOOL_MAX. (current=%lld, %lld)\n", *size, MAX_REG_MEM, MAX_BUFF_SEND);
        CmiAbort("alloc_mempool_block");
    }
#endif
#if LARGEPAGE
    pool = my_get_huge_pages(*size);
    ret = pool==NULL;
#else
    ret = posix_memalign(&pool, ALIGNBUF, *size);
#endif
    if (ret != 0) {
      printf("Charm++> can not allocate memory pool of size %.2fMB. \n", 1.0*(*size)/1024/1024);
      if (ret == ENOMEM)
        CmiAbort("alloc_mempool_block: out of memory.");
      else
        CmiAbort("alloc_mempool_block: posix_memalign failed");
    }
#if LARGEPAGE
    CmiMemLock();
    register_count++;
    MEMORY_REGISTER(onesided_hnd, nic_hndl, pool, *size, mem_hndl, &omdh, cqh, status);
    CmiMemUnlock();
    if(status != GNI_RC_SUCCESS) {
        printf("[%d, %d] memory reigstration %f G (%lld) ask for %lld\n", myrank, CmiMyRank(), register_memory_size/(1024*1024.0*1024),register_count, *size);
sweep_mempool(CpvAccess(mempool));
    }
    GNI_RC_CHECK("MEMORY_REGISTER", status);
#else
    SetMemHndlZero((*mem_hndl));
#endif
    return pool;
}

INLINE_KEYWORD
static void *alloc_mempool_block(size_t *size, gni_mem_handle_t *mem_hndl, int expand_flag)
{
    return _alloc_mempool_block(size, mem_hndl, expand_flag, rdma_rx_cqh);
}

#if CMK_PERSISTENT_COMM_PUT
INLINE_KEYWORD
static void *alloc_persistent_mempool_block(size_t *size, gni_mem_handle_t *mem_hndl, int expand_flag)
{
    return _alloc_mempool_block(size, mem_hndl, expand_flag, highpriority_rx_cqh);
}
#endif

// ptr is a block head pointer
void free_mempool_block(void *ptr, gni_mem_handle_t mem_hndl)
{
    block_header *bh = (block_header *)ptr;
    if(!(IsMemHndlZero(mem_hndl)))
    {
        MEMORY_DEREGISTER(onesided_hnd, nic_hndl, &mem_hndl, &omdh, GetSizeFromBlockHeader(bh));
    }
#if LARGEPAGE
    my_free_huge_pages(ptr, GetSizeFromBlockHeader(bh));
#else
    free(ptr);
#endif
}
#endif

void LrtsPreCommonInit(int everReturn){
#if USE_LRTS_MEMPOOL
    CpvInitialize(mempool_type*, mempool);
    CpvAccess(mempool) = mempool_init(_mempool_size, alloc_mempool_block, free_mempool_block, _mempool_size_limit);
#if CMK_PERSISTENT_COMM_PUT
    CpvInitialize(mempool_type*, persistent_mempool);
    CpvAccess(persistent_mempool) = mempool_init(_mempool_size, alloc_persistent_mempool_block, free_mempool_block, _mempool_size_limit);
#endif
    MACHSTATE2(8, "mempool_init %d %p\n", CmiMyRank(), CpvAccess(mempool)) ;
#endif
}

extern int quietMode;

void LrtsInit(int *argc, char ***argv, int *numNodes, int *myNodeID)
{
    register int            i;
    int                     rc;
    int                     device_id = 0;
    unsigned int            remote_addr;
    gni_cdm_handle_t        cdm_hndl;
    gni_return_t            status = GNI_RC_SUCCESS;
    uint32_t                vmdh_index = -1;
    uint8_t                 ptag;
    unsigned int            local_addr, *MPID_UGNI_AllAddr;
    int                     first_spawned;
    int                     physicalID;
    char                   *env;

    //void (*local_event_handler)(gni_cq_entry_t *, void *)       = &LocalEventHandle;
    //void (*remote_smsg_event_handler)(gni_cq_entry_t *, void *) = &RemoteSmsgEventHandle;
    //void (*remote_bte_event_handler)(gni_cq_entry_t *, void *)  = &RemoteBteEventHandle;

    if(!CharmLibInterOperate || userDrivenMode) {
      status = (gni_return_t)PMI_Init(&first_spawned);
      GNI_RC_CHECK("PMI_Init", status);
    }

    status = (gni_return_t)PMI_Get_size(&mysize);
    GNI_RC_CHECK("PMI_Getsize", status);

    status = (gni_return_t)PMI_Get_rank(&myrank);
    GNI_RC_CHECK("PMI_getrank", status);

    //physicalID = CmiPhysicalNodeID(myrank);

    //printf("Pysical Node ID:%d for PE:%d\n", physicalID, myrank);

    *myNodeID = myrank;
    *numNodes = mysize;

#if MULTI_THREAD_SEND
    /* Currently, we only consider the case that comm. thread will only recv msgs */
    Cmi_smp_mode_setting = COMM_WORK_THREADS_SEND_RECV;
#endif

#if CMI_EXERT_SEND_LARGE_CAP
    CmiGetArgInt(*argv,"+useSendLargeCap", &SEND_large_cap);
#endif

#if CMI_EXERT_RECV_RDMA_CAP
    CmiGetArgInt(*argv,"+useRecvRdmaCap", &RDMA_cap);
#endif

#if CMI_SENDBUFFERSMSG_CAP
    CmiGetArgInt(*argv,"+useSendBufferCap", &SendBufferMsg_cap);
#endif

#if CMI_PUMPNETWORKSMSG_CAP
    CmiGetArgInt(*argv,"+usePumpSmsgCap", &PumpNetworkSmsg_cap);
#endif

    CmiGetArgInt(*argv,"+useRecvQueue", &REMOTE_QUEUE_ENTRIES);

    env = getenv("CHARM_UGNI_REMOTE_QUEUE_SIZE");
    if (env) REMOTE_QUEUE_ENTRIES = atoi(env);
    CmiGetArgInt(*argv,"+useRecvQueue", &REMOTE_QUEUE_ENTRIES);

    env = getenv("CHARM_UGNI_LOCAL_QUEUE_SIZE");
    if (env) LOCAL_QUEUE_ENTRIES = atoi(env);
    CmiGetArgInt(*argv,"+useSendQueue", &LOCAL_QUEUE_ENTRIES);

    env = getenv("CHARM_UGNI_DYNAMIC_SMSG");
    if (env) useDynamicSMSG = 1;
    if (!useDynamicSMSG)
      useDynamicSMSG = CmiGetArgFlag(*argv, "+useDynamicSmsg");
    CmiGetArgIntDesc(*argv, "+smsgConnection", &avg_smsg_connection,"Initial number of SMSGS connection per code");
    if (avg_smsg_connection>mysize) avg_smsg_connection = mysize;
    //useStaticMSGQ = CmiGetArgFlag(*argv, "+useStaticMsgQ");

    if ((myrank == 0) && (!quietMode))
    {
        printf("Charm++> Running on Gemini (GNI) with %d processes\n", mysize);
        printf("Charm++> %s SMSG\n", useDynamicSMSG?"dynamic":"static");
    }
#ifdef USE_ONESIDED
    onesided_init(NULL, &onesided_hnd);

    // this is a GNI test, so use the libonesided bypass functionality
    onesided_gni_bypass_get_nih(onesided_hnd, &nic_hndl);
    local_addr = gniGetNicAddress();
#else
    ptag = get_ptag();
    cookie = get_cookie();
#if 0
    modes = GNI_CDM_MODE_CQ_NIC_LOCAL_PLACEMENT;
#endif
    //Create and attach to the communication  domain */
    status = GNI_CdmCreate(myrank, ptag, cookie, modes, &cdm_hndl);
    GNI_RC_CHECK("GNI_CdmCreate", status);
    //* device id The device id is the minor number for the device
    //that is assigned to the device by the system when the device is created.
    //To determine the device number, look in the /dev directory, which contains a list of devices. For a NIC, the device is listed as kgniX
    //where X is the device number 0 default
    // GNI_CdmAttach adds about 1GB memory usage
    status = GNI_CdmAttach(cdm_hndl, device_id, &local_addr, &nic_hndl);
    GNI_RC_CHECK("GNI_CdmAttach", status);
    local_addr = get_gni_nic_address(0);
#endif
    MPID_UGNI_AllAddr = (unsigned int *)malloc(sizeof(unsigned int) * mysize);
    _MEMCHECK(MPID_UGNI_AllAddr);
    allgather(&local_addr, MPID_UGNI_AllAddr, sizeof(unsigned int));
    /* create the local completion queue */
    /* the third parameter : The number of events the NIC allows before generating an interrupt. Setting this parameter to zero results in interrupt delivery with every event. When using this parameter, the mode parameter must be set to GNI_CQ_BLOCKING*/
    status = GNI_CqCreate(nic_hndl, LOCAL_QUEUE_ENTRIES, 0, GNI_CQ_NOBLOCK, NULL, NULL, &default_tx_cqh);
    GNI_RC_CHECK("GNI_CqCreate (tx)", status);
#if MULTI_THREAD_SEND
    status = GNI_CqCreate(nic_hndl, LOCAL_QUEUE_ENTRIES, 0, GNI_CQ_NOBLOCK, NULL, NULL, &rdma_tx_cqh);
    GNI_RC_CHECK("GNI_CqCreate RDMA (tx)", status);
#endif

#if CMK_USE_OOB
    status = GNI_CqCreate(nic_hndl, LOCAL_QUEUE_ENTRIES, 0, GNI_CQ_NOBLOCK, NULL, NULL, &highprior_rdma_tx_cqh);
    GNI_RC_CHECK("GNI_CqCreate high priority RDMA (tx)", status);
#endif
    /* create the destination completion queue for receiving micro-messages, make this queue considerably larger than the number of transfers */

    status = GNI_CqCreate(nic_hndl, REMOTE_QUEUE_ENTRIES, 0, GNI_CQ_NOBLOCK, NULL, NULL, &smsg_rx_cqh);
    GNI_RC_CHECK("Create CQ (rx)", status);

    status = GNI_CqCreate(nic_hndl, REMOTE_QUEUE_ENTRIES, 0, GNI_CQ_NOBLOCK, NULL, NULL, &rdma_rx_cqh);
    GNI_RC_CHECK("Create Post CQ (rx)", status);

#if CMK_PERSISTENT_COMM_PUT
    status = GNI_CqCreate(nic_hndl, REMOTE_QUEUE_ENTRIES, 0, GNI_CQ_NOBLOCK, NULL, NULL, &highpriority_rx_cqh);
    GNI_RC_CHECK("Create Post CQ (rx)", status);
#endif

#if CMK_ONESIDED_IMPL
    _initOnesided();
#endif
    //status = GNI_CqCreate(nic_hndl, REMOTE_QUEUE_ENTRIES, 0, GNI_CQ_NOBLOCK, NULL, NULL, &rdma_cqh);
    //GNI_RC_CHECK("Create BTE CQ", status);

    /* create the endpoints. they need to be bound to allow later CQWrites to them */
    ep_hndl_array = (gni_ep_handle_t*)malloc(mysize * sizeof(gni_ep_handle_t));
    _MEMCHECK(ep_hndl_array);
#if MULTI_THREAD_SEND
    rx_cq_lock = global_gni_lock = default_tx_cq_lock = smsg_mailbox_lock = CmiCreateLock();
    //default_tx_cq_lock = CmiCreateLock();
    rdma_tx_cq_lock = CmiCreateLock();
    smsg_rx_cq_lock = CmiCreateLock();
    //global_gni_lock  = CmiCreateLock();
    //rx_cq_lock  = CmiCreateLock();
#endif
    for (i=0; i<mysize; i++) {
        if(i == myrank) continue;
        status = GNI_EpCreate(nic_hndl, default_tx_cqh, &ep_hndl_array[i]);
        GNI_RC_CHECK("GNI_EpCreate ", status);
        remote_addr = MPID_UGNI_AllAddr[i];
        status = GNI_EpBind(ep_hndl_array[i], remote_addr, i);
        GNI_RC_CHECK("GNI_EpBind ", status);
    }

    /* SMSG is fastest but not scale; Msgq is scalable, FMA is own implementation for small message */
    _init_smsg();
    PMI_Barrier();

#if     USE_LRTS_MEMPOOL
    env = getenv("CHARM_UGNI_MAX_MEMORY_ON_NODE");
    if (env) {
        _totalmem = CmiReadSize(env);
        if ((myrank == 0) && (!quietMode))
            printf("Charm++> total registered memory available per node is %.1fGB\n", (float)(_totalmem*1.0/oneGB));
    }

    env = getenv("CHARM_UGNI_MEMPOOL_INIT_SIZE");
    if (env) _mempool_size = CmiReadSize(env);
    if (CmiGetArgStringDesc(*argv,"+gni-mempool-init-size",&env,"Set the memory pool size"))
        _mempool_size = CmiReadSize(env);


    env = getenv("CHARM_UGNI_MEMPOOL_MAX");
    if (env) {
        MAX_REG_MEM = CmiReadSize(env);
        user_set_flag = 1;
    }
    if (CmiGetArgStringDesc(*argv,"+gni-mempool-max",&env,"Set the memory pool max size"))  {
        MAX_REG_MEM = CmiReadSize(env);
        user_set_flag = 1;
    }

    env = getenv("CHARM_UGNI_SEND_MAX");
    if (env) {
        MAX_BUFF_SEND = CmiReadSize(env);
        user_set_flag = 1;
    }
    if (CmiGetArgStringDesc(*argv,"+gni-mempool-max-send",&env,"Set the memory pool max size for send"))  {
        MAX_BUFF_SEND = CmiReadSize(env);
        user_set_flag = 1;
    }

    env = getenv("CHARM_UGNI_MEMPOOL_SIZE_LIMIT");
    if (env) {
        _mempool_size_limit = CmiReadSize(env);
    }

    if (MAX_REG_MEM < _mempool_size) MAX_REG_MEM = _mempool_size;
    if (MAX_BUFF_SEND > MAX_REG_MEM)  MAX_BUFF_SEND = MAX_REG_MEM;

    if (myrank==0) {
        if (!quietMode) printf("Charm++> memory pool init block size: %1.fMB, total memory pool limit %1.fMB (0 means no limit)\n", _mempool_size/1024.0/1024, _mempool_size_limit/1024.0/1024);
        if (!quietMode) printf("Charm++> memory pool registered memory limit: %1.fMB, send limit: %1.fMB\n", MAX_REG_MEM/1024.0/1024, MAX_BUFF_SEND/1024.0/1024);
        if (MAX_REG_MEM < BIG_MSG * 2 + oneMB)  {
            /* memblock can expand to BIG_MSG * 2 size */
            if (!quietMode) printf("Charm++ Error: The mempool maximum size is too small, please use command line option +gni-mempool-max or environment variable CHARM_UGNI_MEMPOOL_MAX to increase the value to at least %1.fMB.\n",  BIG_MSG * 2.0/1024/1024 + 1);
            CmiAbort("mempool maximum size is too small. \n");
        }
#if MULTI_THREAD_SEND
        if (!quietMode) printf("Charm++> worker thread sending messages\n");
#elif COMM_THREAD_SEND
        if (!quietMode) printf("Charm++> only comm thread send/recv messages\n");
#endif
    }

#endif     /* end of USE_LRTS_MEMPOOL */

    env = getenv("CHARM_UGNI_BIG_MSG_SIZE");
    if (env) {
        BIG_MSG = CmiReadSize(env);
        if (BIG_MSG < ONE_SEG)
          CmiAbort("BIG_MSG size is too small in the environment variable CHARM_UGNI_BIG_MSG_SIZE.");
    }
    env = getenv("CHARM_UGNI_BIG_MSG_PIPELINE_LEN");
    if (env) {
        BIG_MSG_PIPELINE = atoi(env);
    }

    env = getenv("CHARM_UGNI_NO_DEADLOCK_CHECK");
    if (env) _checkProgress = 0;
    if (mysize == 1) _checkProgress = 0;

#if CMI_EXERT_RECV_RDMA_CAP
    env = getenv("CHARM_UGNI_RDMA_MAX");
    if (env)  {
        RDMA_pending = atoi(env);
        if ((myrank == 0) && (!quietMode))
            printf("Charm++> Max pending RDMA set to: %d\n", RDMA_pending);
    }
#endif

    /*
    env = getenv("HUGETLB_DEFAULT_PAGE_SIZE");
    if (env)
        _tlbpagesize = CmiReadSize(env);
    */
    /* real gethugepagesize() is only available when hugetlb module linked */
#if LARGEPAGE
    _tlbpagesize = gethugepagesize();
#else
    _tlbpagesize = getpagesize();
#endif
    if ((myrank == 0) && (!quietMode)) {
        printf("Charm++> Cray TLB page size: %1.fK\n", _tlbpagesize/1024.0);
    }

#if LARGEPAGE
    if (_tlbpagesize == 4096) {
        CmiAbort("Hugepage module, e.g. craype-hugepages8M must be loaded.");
    }
#endif

      /* stats related arguments */
#if CMK_WITH_STATS
    CmiGetArgStringDesc(*argv,"+gni_stats_root",&counters_dirname,"counter directory name, default counters");

    print_stats = CmiGetArgFlag(*argv, "+print_stats");

    stats_off = CmiGetArgFlag(*argv, "+stats_off");

    init_comm_stats();
#endif

    /* checksum flag */
    if (CmiGetArgFlag(*argv,"+checksum")) {
#if CMK_ERROR_CHECKING
        checksum_flag = 1;
        if (myrank == 0) CmiPrintf("Charm++> CheckSum checking enabled! \n");
#else
        if (myrank == 0) CmiPrintf("Charm++> +checksum ignored in optimized version! \n");
#endif
    }

    /* init DMA buffer for medium message */

    //_init_DMA_buffer();

    free(MPID_UGNI_AllAddr);

    sendRdmaBuf = PCQueueCreate();
    sendHighPriorBuf = PCQueueCreate();

//    NTK_Init();
//    ntk_return_t sts = NTK_System_GetSmpdCount(&_smpd_count);

#if CMK_SMSGS_FREE_AFTER_EVENT
    int smsgPoolSize = 16384;
    if(mysize*SMSG_MAX_CREDIT < smsgPoolSize)
        smsgPoolSize = mysize*SMSG_MAX_CREDIT;
    IndexPool_init(&smsgsPool, smsgPoolSize, 1u<<31-1);
#endif

#if  REMOTE_EVENT
    SHIFT = 1;
    while (1<<SHIFT < mysize) SHIFT++;
    CmiAssert(SHIFT < 31);
    if ( (1<<(31-SHIFT)) < POOL_INIT_SIZE) CmiAbort("IndexPool_init: pool initial size is too big.");
    IndexPool_init(&ackPool, POOL_INIT_SIZE, 1u<<(31-SHIFT));
#if CMK_PERSISTENT_COMM_PUT
    IndexPool_init(&persistPool, POOL_INIT_SIZE, 1u<<(31-SHIFT));
#endif
#endif
}

void* LrtsRdmaAlloc(int n_bytes, int header)
{
    void *ptr = NULL;
    int alignbuf = ALIGNBUF;
    CmiAssert(header+sizeof(mempool_header) <= ALIGNBUF);
#if     USE_LRTS_MEMPOOL
    n_bytes = ALIGN64(n_bytes);
    int val = ALIGNBUF+n_bytes-sizeof(mempool_header);
    if(n_bytes < BIG_MSG)
    {
        char *res = (char *)mempool_malloc(CpvAccess(mempool), ALIGNBUF+n_bytes-sizeof(mempool_header), 1);
        if (res) ptr = res - sizeof(mempool_header) + ALIGNBUF - header;
    }else
    {
#if LARGEPAGE
        n_bytes = ALIGNHUGEPAGE(n_bytes+ALIGNBUF);
        char *res = (char *)my_get_huge_pages(n_bytes);
#else
        char *res = (char *)memalign(ALIGNBUF, n_bytes+ALIGNBUF);
#endif
        if (res) ptr = res + ALIGNBUF - header;
    }
#else
    n_bytes = ALIGN64(n_bytes);           /* make sure size if 4 aligned */
    char *res = (char *)memalign(ALIGNBUF, n_bytes+ALIGNBUF);
    ptr = res + ALIGNBUF - header;
#endif
    return ptr;
}

void  LrtsRdmaFree(void *msg)
{
    int headersize = sizeof(CmiChunkHeader);
    char *aligned_addr = (char *)msg + headersize - ALIGNBUF;
    CmiUInt4 size = SIZEFIELD((char*)msg+headersize);
    size = ALIGN64(size);
    if(size>=BIG_MSG)
    {
#if LARGEPAGE
        int s = ALIGNHUGEPAGE(size+ALIGNBUF);
        my_free_huge_pages(aligned_addr, s);
#else
        free((char*)msg + headersize - ALIGNBUF);
#endif
    }
    else {
#if    USE_LRTS_MEMPOOL
#if CMK_SMP
        mempool_free_thread(aligned_addr + sizeof(mempool_header));
#else
        mempool_free(CpvAccess(mempool), aligned_addr + sizeof(mempool_header));
#endif
#else
        free(aligned_addr);
#endif
    }
}

void* LrtsAlloc(int n_bytes, int header)
{
    void *ptr = NULL;
#if 0
    printf("\n[PE:%d]Alloc Lrts for bytes=%d, head=%d %d\n", CmiMyPe(), n_bytes, header, SMSG_MAX_MSG);
#endif
    if(n_bytes <= SMSG_MAX_MSG)
    {
        int totalsize = n_bytes+header;
        ptr = memalign(ALIGNBUF, totalsize);
    }
    else {
        CmiAssert(header+sizeof(mempool_header) <= ALIGNBUF);
#if     USE_LRTS_MEMPOOL
        n_bytes = ALIGN64(n_bytes);
        if(n_bytes < BIG_MSG)
        {
            char *res = (char *)mempool_malloc(CpvAccess(mempool), ALIGNBUF+n_bytes-sizeof(mempool_header), 1);
            if (res) ptr = res - sizeof(mempool_header) + ALIGNBUF - header;
        }else
        {
#if LARGEPAGE
            //printf("[%d] LrtsAlloc a big_msg: %d %d\n", myrank, n_bytes, ALIGNHUGEPAGE(n_bytes+ALIGNBUF));
            n_bytes = ALIGNHUGEPAGE(n_bytes+ALIGNBUF);
            char *res = (char *)my_get_huge_pages(n_bytes);
#else
            char *res = (char *)memalign(ALIGNBUF, n_bytes+ALIGNBUF);
#endif
            if (res) ptr = res + ALIGNBUF - header;
        }
#else
        n_bytes = ALIGN64(n_bytes);           /* make sure size if 4 aligned */
        char *res = (char *)memalign(ALIGNBUF, n_bytes+ALIGNBUF);
        ptr = res + ALIGNBUF - header;
#endif
    }
    return ptr;
}

void  LrtsFree(void *msg)
{
    int headersize = sizeof(CmiChunkHeader);
    char *aligned_addr = (char *)msg + headersize - ALIGNBUF;
    CmiUInt4 size = SIZEFIELD((char*)msg+headersize);
#if CMK_PERSISTENT_COMM_PUT
    if (IS_PERSISTENT_MEMORY(msg)) return;
#endif
    if (size <= SMSG_MAX_MSG)
        free(msg);
    else {
        size = ALIGN64(size);
        if(size>=BIG_MSG)
        {
#if LARGEPAGE
            int s = ALIGNHUGEPAGE(size+ALIGNBUF);
            my_free_huge_pages(aligned_addr, s);
#else
            free((char*)msg + headersize - ALIGNBUF);
#endif
        }
        else {
#if    USE_LRTS_MEMPOOL
#if CMK_SMP
            mempool_free_thread(aligned_addr + sizeof(mempool_header));
#else
            mempool_free(CpvAccess(mempool), aligned_addr + sizeof(mempool_header));
#endif
#else
            free(aligned_addr);
#endif
        }
    }
}

void LrtsExit(int exitcode)
{
#if CMK_WITH_STATS
#if CMK_SMP
    if(CmiMyRank() == CmiMyNodeSize())
#endif
    if (print_stats) print_comm_stats();
#endif
    /* free memory ? */
#if USE_LRTS_MEMPOOL
    //printf("FINAL [%d, %d]  register=%lld, send=%lld\n", myrank, CmiMyRank(), register_memory_size, buffered_send_msg);
    mempool_destroy(CpvAccess(mempool));
#endif
    if(!CharmLibInterOperate || userDrivenMode) {
      PMI_Barrier();
      PMI_Finalize();
      if (!userDrivenMode) {
        exit(exitcode);
      }
    }
}

void LrtsDrainResources(void)
{
    if(mysize == 1) return;
    while (
#if CMK_USE_OOB
           !SendBufferMsg(&smsg_oob_queue, NULL) ||
#endif
           !SendBufferMsg(&smsg_queue, NULL)
          )
    {
        if (useDynamicSMSG)
            PumpDatagramConnection();
        PumpNetworkSmsg();
        PumpLocalTransactions(default_tx_cqh, default_tx_cq_lock);

#if MULTI_THREAD_SEND
        PumpLocalTransactions(rdma_tx_cqh, rdma_tx_cq_lock);
#endif

#if CMK_USE_OOB
        PumpLocalTransactions(highprior_rdma_tx_cqh, rdma_tx_cq_lock);
#endif

#if REMOTE_EVENT
        PumpRemoteTransactions(rdma_rx_cqh);
#endif
        SendRdmaMsg(sendRdmaBuf);
        SendRdmaMsg(sendHighPriorBuf);
    }
    PMI_Barrier();
}

void LrtsAbort(const char *message) {
    PMI_Abort(-1, message);
    CMI_NORETURN_FUNCTION_END
}

/**************************  TIMER FUNCTIONS **************************/
#if CMK_TIMER_USE_SPECIAL
/* MPI calls are not threadsafe, even the timer on some machines */
static CmiNodeLock  timerLock = 0;
static int _absoluteTime = 0;
static int _is_global = 0;
static struct timespec start_ts;

int CmiTimerIsSynchronized(void) {
    return 0;
}

int CmiTimerAbsolute(void) {
    return _absoluteTime;
}

double CmiStartTimer(void) {
    return 0.0;
}

double CmiInitTime(void) {
    return (double)(start_ts.tv_sec)+(double)start_ts.tv_nsec/1000000000.0;
}

void CmiTimerInit(char **argv) {
    _absoluteTime = CmiGetArgFlagDesc(argv,"+useAbsoluteTime", "Use system's absolute time as wallclock time.");
    if (_absoluteTime && CmiMyPe() == 0)
        printf("Charm++> absolute  timer is used\n");

    _is_global = CmiTimerIsSynchronized();


    if (_is_global) {
        if (CmiMyRank() == 0) {
            clock_gettime(CLOCK_MONOTONIC, &start_ts);
        }
    } else { /* we don't have a synchronous timer, set our own start time */
        CmiBarrier();
        CmiBarrier();
        CmiBarrier();
        clock_gettime(CLOCK_MONOTONIC, &start_ts);
    }
    CmiNodeAllBarrier();          /* for smp */
}

/**
 * Since the timerLock is never created, and is
 * always NULL, then all the if-condition inside
 * the timer functions could be disabled right
 * now in the case of SMP.
 */
double CmiTimer(void) {
    struct timespec now_ts;
    clock_gettime(CLOCK_MONOTONIC, &now_ts);
    return _absoluteTime?((double)(now_ts.tv_sec)+(double)now_ts.tv_nsec/1000000000.0)
        : (double)( now_ts.tv_sec - start_ts.tv_sec ) + (((double) now_ts.tv_nsec - (double) start_ts.tv_nsec)  / 1000000000.0);
}

double CmiWallTimer(void) {
    struct timespec now_ts;
    clock_gettime(CLOCK_MONOTONIC, &now_ts);
    return _absoluteTime?((double)(now_ts.tv_sec)+(double)now_ts.tv_nsec/1000000000.0)
        : ( now_ts.tv_sec - start_ts.tv_sec ) + ((now_ts.tv_nsec - start_ts.tv_nsec)  / 1000000000.0);
}

double CmiCpuTimer(void) {
    struct timespec now_ts;
    clock_gettime(CLOCK_MONOTONIC, &now_ts);
    return _absoluteTime?((double)(now_ts.tv_sec)+(double)now_ts.tv_nsec/1000000000.0)
        : (double)( now_ts.tv_sec - start_ts.tv_sec ) + (((double) now_ts.tv_nsec - (double) start_ts.tv_nsec)  / 1000000000.0);
}

#endif
/************Barrier Related Functions****************/

void LrtsBarrier(void)
{
    gni_return_t status;

    status = (gni_return_t)PMI_Barrier();
    GNI_RC_CHECK("PMI_Barrier", status);
}
#if CMK_ONESIDED_IMPL
#include "machine-onesided.C"
#endif
#if CMK_PERSISTENT_COMM
#include "machine-persistent.C"
#endif


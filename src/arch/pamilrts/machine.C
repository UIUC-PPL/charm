#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <string.h>
#include "machine.h"
#include "converse.h"
#include "cmirdmautils.h"
#include "pcqueue.h"
#include "assert.h"
#include "malloc.h"
#include <algorithm>

#if CMK_BLUEGENEQ
#include <hwi/include/bqc/A2_inlines.h>
#include "spi/include/kernel/process.h"
#include "spi/include/kernel/memory.h"
#endif

#include "pami.h"
#include "pami_sys.h"

#if !CMK_SMP
#if CMK_ENABLE_ASYNC_PROGRESS
#error "async progress non supported with non-smp"
#endif
#endif

#if CMK_SMP && CMK_PPC_ATOMIC_QUEUE
#include "PPCAtomicQueue.h"
#include "memalloc.C"
#endif

#if CMK_SMP && CMK_PPC_ATOMIC_MUTEX
#include "PPCAtomicMutex.h"
#endif

// These macros are used in machine-common-core.C  (LRTS layer)
#define CMK_MACH_SPECIALIZED_QUEUE        (CMK_SMP && CMK_PPC_ATOMIC_QUEUE)
#define CMK_MACH_SPECIALIZED_MUTEX        (CMK_SMP && CMK_PPC_ATOMIC_MUTEX)

#define CMI_LIKELY(x)    (__builtin_expect(x,1))
#define CMI_UNLIKELY(x)  (__builtin_expect(x,0))

char *ALIGN_32(char *p) {
  return((char *)((((unsigned long)p)+0x1f) & (~0x1FUL)));
}


#define CMI_MAGIC(msg)                   ((CmiMsgHeaderBasic *)msg)->magic
/* FIXME: need a random number that everyone agrees ! */
#define CHARM_MAGIC_NUMBER               126

#define CMI_IS_BCAST_ON_CORES(msg) (CMI_BROADCAST_ROOT(msg) > 0)
#define CMI_IS_BCAST_ON_NODES(msg) (CMI_BROADCAST_ROOT(msg) < 0)

#define CMI_PAMI_SHORT_DISPATCH           7
#define CMI_PAMI_RZV_DISPATCH             8
#define CMI_PAMI_ACK_DISPATCH             9
#define CMI_PAMI_DISPATCH                10

#if CMK_BLUEGENEQ
#define SHORT_CUTOFF   128
#define EAGER_CUTOFF   4096
#else
#define SHORT_CUTOFF   7680
#define EAGER_CUTOFF   16384
#endif

#if CMK_PERSISTENT_COMM
#include "machine-persistent.h"
#endif

#if CMK_ERROR_CHECKING
static int checksum_flag = 0;
unsigned char computeCheckSum(unsigned char *data, size_t len);

#define CMI_SET_CHECKSUM(msg, len)      \
  if (checksum_flag)  {   \
    ((CmiMsgHeaderBasic *)msg)->cksum = 0;        \
    ((CmiMsgHeaderBasic *)msg)->cksum = computeCheckSum((unsigned char*)msg, len);        \
  }

#define CMI_CHECK_CHECKSUM(msg, len)    \
  int count; \
if (checksum_flag)      \
     if (computeCheckSum((unsigned char*)msg, len) != 0)  { \
       printf("\n\n------------------------------\n\nReceiver %d size %d:", CmiMyPe(), len); \
         for(count = 0; count < len; count++) { \
           printf("%2x", msg[count]);                 \
         }                                             \
       printf("------------------------------\n\n"); \
         CmiAbort("Fatal error: checksum doesn't agree!\n"); \
     }
#else
#define CMI_SET_CHECKSUM(msg, len)
#define CMI_CHECK_CHECKSUM(msg, len)
#endif

typedef struct ProcState {
#if CMK_SMP && CMK_PPC_ATOMIC_QUEUE
  PPCAtomicQueue   atomic_queue;
  char            _pad[128-sizeof(PPCAtomicQueue)];
#endif
} ProcState;

static ProcState  *procState;

#if CMK_SMP && CMK_PPC_ATOMIC_QUEUE
static PPCAtomicQueue node_recv_atomic_q;
#endif

#if CMK_SMP && CMK_PPC_ATOMIC_MUTEX
static PPCAtomicMutex *node_recv_mutex;
#endif

#if CMK_SMP && !CMK_MULTICORE
//static volatile int commThdExit = 0;
//static CmiNodeLock commThdExitLock = 0;

//The random seed to pick destination context
CMK_THREADLOCAL uint32_t r_seed = 0xdeadbeef;
CMK_THREADLOCAL int32_t _cmi_bgq_incommthread = 0;
CMK_THREADLOCAL int32_t _comm_thread_id = 0;
#endif

//int CmiInCommThread () {
//  //if (_cmi_bgq_incommthread)
//  //printf ("CmiInCommThread: %d\n", _cmi_bgq_incommthread);
//  return _cmi_bgq_incommthread;
//}

#if CMK_SMP && CMK_PPC_ATOMIC_QUEUE
void LrtsSpecializedQueuePush(int pe, void  *msg) {
  PPCAtomicEnqueue(&procState[pe].atomic_queue, (char *)msg);
}

char *LrtsSpecializedQueuePop() {
 return (char *)PPCAtomicDequeue(&procState[CmiMyRank()].atomic_queue);
}

#if CMK_NODE_QUEUE_AVAILABLE
void LrtsSpecializedNodeQueuePush(char *msg) {
  PPCAtomicEnqueue(&node_recv_atomic_q, msg);
}

char *LrtsSpecializedNodeQueuePop() {
  return (char *)PPCAtomicDequeue(&node_recv_atomic_q);
}

int LrtsSpecializedNodeQueueEmpty() {
  return PPCAtomicQueueEmpty(&node_recv_atomic_q);
}
#endif

#if CMK_SMP && CMK_PPC_ATOMIC_MUTEX
int LrtsSpecializedMutexTryAcquire() {
  return PPCAtomicMutexTryAcquire(node_recv_mutex);
}

void LrtsSpecializedMutexRelease() {
  PPCAtomicMutexRelease(node_recv_mutex);
}
#endif
#endif

static void CmiNetworkBarrier(int async);
#if SPECIFIC_PCQUEUE && CMK_SMP
#define  QUEUE_NUMS     _Cmi_mynodesize + 3
#include "lrtsqueue.h"
#include "memalloc.C"
#endif
#include "machine-lrts.h"
#include "machine-common-core.C"

#if CMK_SMP
extern pthread_t *_Cmi_mypidlist;
#endif

#if CMK_SMP && !CMK_ENABLE_ASYNC_PROGRESS
CpvDeclare(int, uselock);
#endif

#if CMK_ENABLE_ASYNC_PROGRESS  
//Immediate messages not supported yet
void LrtsAdvanceCommunication(int whenidle) {}
#endif

void _alias_rank (int rank);

#define MAX_NUM_CONTEXTS  64

#if CMK_SMP 
#define CMK_PAMI_MULTI_CONTEXT  1
#else
#define CMK_PAMI_MULTI_CONTEXT  0
#endif

#if CMK_PAMI_MULTI_CONTEXT
volatile int msgQueueLen [MAX_NUM_CONTEXTS];
volatile int outstanding_recvs [MAX_NUM_CONTEXTS];

//#if CMK_SMP && CMK_ENABLE_ASYNC_PROGRESS
//#define THREADS_PER_CONTEXT 2
//#define LTPS                1 //Log Threads Per Context (TPS)
//#else
#if CMK_BLUEGENEQ
#define THREADS_PER_CONTEXT 4
#define LTPS                2 //Log Threads Per Context (TPS)
#else
#define THREADS_PER_CONTEXT 1
#define LTPS                0 //Log Threads Per Context (TPS)
#endif //endif CMK_BLUEGENEQ

#define  MY_CONTEXT_ID() (CmiMyRank() >> LTPS)
#define  MY_CONTEXT()    (cmi_pami_contexts[CmiMyRank() >> LTPS])

#define  INCR_MSGQLEN()  //(msgQueueLen[CmiMyRank() >> LTPS] ++)
#define  DECR_MSGQLEN()  //(msgQueueLen[CmiMyRank() >> LTPS] --)
#define  MSGQLEN()       0 //(msgQueueLen[CmiMyRank() >> LTPS])
#define  INCR_ORECVS()   //(outstanding_recvs[CmiMyRank() >> LTPS] ++)
#define  DECR_ORECVS()   //(outstanding_recvs[CmiMyRank() >> LTPS] --)
#define  ORECVS()        0 //(outstanding_recvs[CmiMyRank() >> LTPS])
#else
#define LTPS    1 
volatile int msgQueueLen;
volatile int outstanding_recvs;
#define  MY_CONTEXT_ID() (0)
#define  MY_CONTEXT()    (cmi_pami_contexts[0])

#define  INCR_MSGQLEN()  (msgQueueLen ++)
#define  DECR_MSGQLEN()  (msgQueueLen --)
#define  MSGQLEN()       (msgQueueLen)
#define  INCR_ORECVS()   (outstanding_recvs ++)
#define  DECR_ORECVS()   (outstanding_recvs --)
#define  ORECVS()        (outstanding_recvs)
#endif

// Function declaration for ppc_msync
void ppc_msync();

#if CMK_SMP  && !CMK_ENABLE_ASYNC_PROGRESS
#define PAMIX_CONTEXT_LOCK_INIT(x)
#define PAMIX_CONTEXT_LOCK(x)        if(LTPS) PAMI_Context_lock(x)
#define PAMIX_CONTEXT_UNLOCK(x)      if(LTPS) {ppc_msync(); PAMI_Context_unlock(x);}
#define PAMIX_CONTEXT_TRYLOCK(x)     ((LTPS)?(PAMI_Context_trylock(x) == PAMI_SUCCESS):(1))
#else
#define PAMIX_CONTEXT_LOCK_INIT(x)
#define PAMIX_CONTEXT_LOCK(x)
#define PAMIX_CONTEXT_UNLOCK(x)
#define PAMIX_CONTEXT_TRYLOCK(x)      1
#endif

#define A_PRIME 13
#define B_PRIME 19

static INLINE_KEYWORD unsigned myrand (unsigned *seed) {
  *seed = A_PRIME * (*seed) + B_PRIME;
  return *seed;
}

static void send_done(pami_context_t ctxt, void *data, pami_result_t result) 
{
  CmiFree(data);
  DECR_MSGQLEN();
}

#if CMK_SMP
static void CmiSendPeer (int rank, int size, char *msg) {
  //fprintf(stderr, "%d Send messages to peer\n", CmiMyPe());
  CmiPushPE (rank, msg);
}
#endif


static void recv_done(pami_context_t ctxt, void *clientdata, pami_result_t result) 
  /* recv done callback: push the recved msg to recv queue */
{
  char *msg = (char *) clientdata;
  int sndlen = ((CmiMsgHeaderBasic *) msg)->size;
  //int rank = *(int *) (msg + sndlen); //get rank from bottom of the message
  //CMI_DEST_RANK(msg) = rank;

  CMI_CHECK_CHECKSUM(msg, sndlen);
  if (CMI_MAGIC(msg) != CHARM_MAGIC_NUMBER) { 
    CmiAbort("Charm++ Warning: Non Charm++ Message Received. If your application has a large number of messages, this may be because of overflow in the low-level FIFOs. Please set the environment variable MUSPI_INJFIFOSIZE if the application has large number of small messages (<=4K bytes), and/or PAMI_RGETINJFIFOSIZE if the application has a large number of large messages. The default value of these variable is 65536 which is sufficient for 1000 messages in flight; please try a larger value. Please note that the memory used for these FIFOs eats up the memory = 10*FIFO_SIZE per core. Please contact Charm++ developers for further information. \n");     
    return;
  }

#if CMK_SMP && !CMK_ENABLE_ASYNC_PROGRESS
  CpvAccess(uselock) = 0;
#endif
  handleOneRecvedMsg(sndlen,msg);
#if CMK_SMP && !CMK_ENABLE_ASYNC_PROGRESS
  CpvAccess(uselock) = 1;
#endif

  DECR_ORECVS();
}

typedef struct _cmi_pami_rzv {
  void           * buffer;
  size_t           offset;
  int              bytes;
  int              dst_context;
}CmiPAMIRzv_t;  

typedef struct _cmi_pami_rzv_recv {
  void           * msg;
  void           * src_buffer;
  int              src_ep;
} CmiPAMIRzvRecv_t;

static void pkt_dispatch (pami_context_t       context,      
    void               * clientdata,   
    const void         * header_addr,  
    size_t               header_size,  
    const void         * pipe_addr,    
    size_t               pipe_size,    
    pami_endpoint_t      origin,
    pami_recv_t         * recv)        
{
  //fprintf (stderr, "Received Message of size %d %p\n", pipe_size, recv);
  INCR_ORECVS();    
  int alloc_size = pipe_size;
  char * buffer  = (char *)CmiAlloc(alloc_size);

  if (recv) {
    recv->local_fn = recv_done;
    recv->cookie   = buffer;
    recv->type     = PAMI_TYPE_BYTE;
    recv->addr     = buffer;
    recv->offset   = 0;
    recv->data_fn  = PAMI_DATA_COPY;
  }
  else {
    memcpy (buffer, pipe_addr, pipe_size);
    recv_done (NULL, buffer, PAMI_SUCCESS);
  }
}

static void short_pkt_dispatch (pami_context_t       context,      
    void               * clientdata,   
    const void         * header_addr,  
    size_t               header_size,  
    const void         * pipe_addr,    
    size_t               pipe_size,    
    pami_endpoint_t      origin,
    pami_recv_t         * recv)        
{
  int alloc_size = pipe_size;
  char * buffer  = (char *)CmiAlloc(alloc_size);

  memcpy (buffer, pipe_addr, pipe_size);
  char *smsg = (char *)pipe_addr;
  char *msg  = (char *)buffer;

  CMI_CHECK_CHECKSUM(smsg, (int)pipe_size);  
  if (CMI_MAGIC(smsg) != CHARM_MAGIC_NUMBER) {
    /* received a non-charm msg */
    CmiAbort("Charm++ Warning: Non Charm++ Message Received. If your application has a large number of messages, this may be because of overflow in the low-level FIFOs. Please set the environment variable MUSPI_INJFIFOSIZE if the application has large number of small messages (<=4K bytes), and/or PAMI_RGETINJFIFOSIZE if the application has a large number of large messages. The default value of these variable is 65536 which is sufficient for 1000 messages in flight; please try a larger value. Please note that the memory used for these FIFOs eats up the memory = 10*FIFO_SIZE per core. Please contact Charm++ developers for further information. \n");     
  }
#if CMK_SMP && !CMK_ENABLE_ASYNC_PROGRESS
  CpvAccess(uselock) = 0;
#endif
  handleOneRecvedMsg(pipe_size,msg);
#if CMK_SMP && !CMK_ENABLE_ASYNC_PROGRESS
  CpvAccess(uselock) = 1;
#endif
}

void rzv_pkt_dispatch (pami_context_t       context,   
    void               * clientdata,
    const void         * header_addr,
    size_t               header_size,
    const void         * pipe_addr,  
    size_t               pipe_size,  
    pami_endpoint_t      origin,
    pami_recv_t         * recv);

void ack_pkt_dispatch (pami_context_t       context,   
    void               * clientdata,
    const void         * header_addr,
    size_t               header_size,
    const void         * pipe_addr,  
    size_t               pipe_size,  
    pami_endpoint_t      origin,
    pami_recv_t         * recv);

void rzv_recv_done   (pami_context_t     ctxt, 
    void             * clientdata, 
    pami_result_t      result); 

#if CMK_BLUEGENEQ
//approx sleep command
size_t mysleep_iter = 0;
void mysleep (unsigned long cycles) {
  unsigned long start = GetTimeBase();
  unsigned long end = start + cycles;

  while (start < end) {
    mysleep_iter ++;
    start = GetTimeBase();
  }

  return;
}
#endif

static void * test_buf;
volatile int coll_barrier_flag = 0;
//typedef pami_result_t (*pamix_proc_memalign_fn) (void**, size_t, size_t, const char*);

pami_client_t      cmi_pami_client;
pami_context_t   * cmi_pami_contexts;
size_t             cmi_pami_numcontexts;
char clientname[] = "Converse";

void coll_barrier_done (void *ctxt, void * clientdata, pami_result_t err)
{
  int * active = (int *) clientdata;
  (*active)--;
}

//Collective definitions
#ifdef LIBCOLL
#include <libcoll.h>
libcoll_context_t   cmi_libcoll_context;
libcoll_geometry_t  world_geometry;
libcoll_xfer_t      xfer_barrier;
#else
pami_geometry_t     world_geometry;
pami_xfer_t         xfer_barrier;
#endif
void init_barrier ();


typedef struct _cmi_pami_mregion_t {
  pami_memregion_t   mregion;
  void             * baseVA;
} CmiPAMIMemRegion_t;

//one for each of the 64 possible contexts
CmiPAMIMemRegion_t  cmi_pami_memregion[64];

#if CMK_ONESIDED_IMPL
#include "machine-rdma.h"
#include "machine-onesided.h"
#endif
#include "machine-rdma.h"

#include "malloc.h"
//void *l2atomicbuf;

void _alias_rank (int rank) {
#if CMK_SMP && CMK_ENABLE_ASYNC_PROGRESS

  CmiState cs = CmiGetState();
  CmiState cs_r = CmiGetStateN(rank);

  cs->rank = cs_r->rank;
  cs->pe   = cs_r->pe;
#endif
}

#if CMK_SMP && CMK_ENABLE_ASYNC_PROGRESS

typedef struct _comm_thr_init_state {
  volatile int flag;
  int          id;
} CommThreadInitState;

pami_result_t init_comm_thread (pami_context_t   context,
    void           * cookie)
{
  CmiState cs  = CmiGetState();
  CmiState cs0 = CmiGetStateN(0);
  *cs = *cs0; //Alias comm thread to rank 0
  //printf("Initialized comm thread, my rank %d, my pe %d\n", 
  // CmiMyRank(), 
  // CmiMyPe());

  //Notify main thread comm thread has been initialized
  CommThreadInitState *state = (CommThreadInitState *) cookie;
  state->flag = 0;
  _comm_thread_id = state->id;

  //set the seed to choose destination context
  uint64_t rseedl = r_seed;
  rseedl |= (uint64_t)context;
  r_seed = ((uint32_t)rseedl)^((uint32_t)(rseedl >> 32));

  _cmi_bgq_incommthread = 1;

  return PAMI_SUCCESS;
}

typedef void (*pamix_progress_function) (pami_context_t context, void *cookie);
typedef pami_result_t (*pamix_progress_register_fn) 
  (pami_context_t            context,
   pamix_progress_function   progress_fn,
   pamix_progress_function   suspend_fn,
   pamix_progress_function   resume_fn,
   void                     * cookie);
typedef pami_result_t (*pamix_progress_enable_fn)(pami_context_t   context,
    int              event_type);
typedef pami_result_t (*pamix_progress_disable_fn)(pami_context_t  context,
    int             event_type);
#define PAMI_EXTENSION_OPEN(client, name, ext)  \
  ({                                              \
   pami_result_t rc;                             \
   rc = PAMI_Extension_open(client, name, ext);  \
   CmiAssert (rc == PAMI_SUCCESS);      \
   })
#define PAMI_EXTENSION_FUNCTION(type, name, ext)        \
  ({                                                      \
   void* fn;                                             \
   fn = PAMI_Extension_symbol(ext, name);                \
   CmiAssert (fn != NULL);				\
   (type)fn;                                             \
   })

pami_extension_t            cmi_ext_progress;
pamix_progress_register_fn  cmi_progress_register;
pamix_progress_enable_fn    cmi_progress_enable;
pamix_progress_disable_fn   cmi_progress_disable;

extern int quietMode;

int CMI_Progress_init(int start, int ncontexts) {
  if ((CmiMyPe() == 0) && (!quietMode))
    printf("Enabling communication threads\n");

  PAMI_EXTENSION_OPEN(cmi_pami_client,"EXT_async_progress",&cmi_ext_progress);
  cmi_progress_register = PAMI_EXTENSION_FUNCTION(pamix_progress_register_fn, "register", cmi_ext_progress);
  cmi_progress_enable   = PAMI_EXTENSION_FUNCTION(pamix_progress_enable_fn,   "enable",   cmi_ext_progress);
  cmi_progress_disable  = PAMI_EXTENSION_FUNCTION(pamix_progress_disable_fn,  "disable",  cmi_ext_progress);

  int i = 0;
  for (i = start; i < start+ncontexts; ++i) {
    //fprintf(stderr, "Enabling progress on context %d\n", i);
    cmi_progress_register (cmi_pami_contexts[i], 
        NULL, 
        NULL, 
        NULL, NULL);
    cmi_progress_enable   (cmi_pami_contexts[i], 0 /*progress all*/);  
  }

  pami_work_t  work;
  CommThreadInitState cstate;
  for (i = start; i < start+ncontexts; ++i) {
    cstate.flag = 1;
    cstate.id   = i;
    PAMI_Context_post(cmi_pami_contexts[i], &work,
		      init_comm_thread, (void*)&cstate);
    while(cstate.flag);
  }

  return 0;
}

int CMI_Progress_finalize(int start, int ncontexts) {
  int i = 0;
  for (i = start; i < start+ncontexts; ++i) {
    cmi_progress_disable  (cmi_pami_contexts[i], 0 /*progress all*/);  
  }
  PAMI_Extension_close (cmi_ext_progress);
  return 0;
}
#endif

void LrtsInit(int *argc, char ***argv, int *numNodes, int *myNodeID)
{
  int n, i, count;

  PAMI_Client_create (clientname, &cmi_pami_client, NULL, 0);
  size_t _n = 1;
#if CMK_PAMI_MULTI_CONTEXT
  if ((_Cmi_mynodesize % THREADS_PER_CONTEXT) == 0)
    _n = _Cmi_mynodesize / THREADS_PER_CONTEXT;  //have a context for each four threads
  else
    _n = 1 + (_Cmi_mynodesize / THREADS_PER_CONTEXT);  //have a context for each four threads
#endif

  cmi_pami_contexts = (pami_context_t *) malloc (sizeof(pami_context_t) * _n);
  pami_result_t rc = PAMI_Context_createv (cmi_pami_client, NULL, 0, cmi_pami_contexts, _n);
  if (rc != PAMI_SUCCESS) {
    fprintf(stderr, "PAMI_Context_createv failed for %zu contexts\n", _n);
    assert(0);
  }
  cmi_pami_numcontexts = _n;

#if CMK_SMP && !CMK_ENABLE_ASYNC_PROGRESS
  // No comm thread for regular smp
  Cmi_smp_mode_setting = COMM_THREAD_NOT_EXIST;
#endif
  // The default setting i.e COMM_THREAD_SEND_RECV, set in machine-common-core.C is used for async-smp

  //fprintf(stderr,"Creating %d pami contexts\n", _n);

  pami_configuration_t configuration;
  pami_result_t result;

  configuration.name = PAMI_CLIENT_TASK_ID;
  result = PAMI_Client_query(cmi_pami_client, &configuration, 1);
  *myNodeID = configuration.value.intval;

  configuration.name = PAMI_CLIENT_NUM_TASKS;
  result = PAMI_Client_query(cmi_pami_client, &configuration, 1);
  *numNodes = configuration.value.intval;

  pami_dispatch_hint_t options = (pami_dispatch_hint_t) {0};
  pami_dispatch_callback_function pfn;
  for (i = 0; i < _n; ++i) {
    pfn.p2p = pkt_dispatch;
    PAMI_Dispatch_set (cmi_pami_contexts[i],
        CMI_PAMI_DISPATCH,
        pfn,
        NULL,
        options);

    pfn.p2p = ack_pkt_dispatch;
    PAMI_Dispatch_set (cmi_pami_contexts[i],
        CMI_PAMI_ACK_DISPATCH,
        pfn,
        NULL,
        options);

    pfn.p2p = rzv_pkt_dispatch;
    PAMI_Dispatch_set (cmi_pami_contexts[i],
        CMI_PAMI_RZV_DISPATCH,
        pfn,
        NULL,
        options);      

    pfn.p2p = short_pkt_dispatch;
    PAMI_Dispatch_set (cmi_pami_contexts[i],
        CMI_PAMI_SHORT_DISPATCH,
        pfn,
        NULL,
        options);      
  }

#if CMK_BLUEGENEQ
  size_t bytes_out;
  void * buf = malloc(sizeof(long));    
  uint32_t retval;
  Kernel_MemoryRegion_t k_mregion;
  retval = Kernel_CreateMemoryRegion (&k_mregion, buf, sizeof(long));
  assert(retval==0);  
  for (i = 0; i < _n; ++i) {
    cmi_pami_memregion[i].baseVA = k_mregion.BaseVa;
    PAMI_Memregion_create (cmi_pami_contexts[i],
        k_mregion.BaseVa,
        k_mregion.Bytes,
        &bytes_out,
        &cmi_pami_memregion[i].mregion);
  }
  free(buf);
#endif

  //fprintf(stderr, "%d Initializing Converse PAMI machine Layer on %d tasks\n", _Cmi_mynode, _Cmi_numnodes);

  init_barrier();

  CmiNetworkBarrier(0);
  CmiNetworkBarrier(0);
  CmiNetworkBarrier(0);

  /* checksum flag */
  if (CmiGetArgFlag(*argv,"+checksum")) {
#if CMK_ERROR_CHECKING
    checksum_flag = 1;
    if (_Cmi_mynode == 0) CmiPrintf("Charm++: CheckSum checking enabled! \n");
#else
    if (_Cmi_mynode == 0) CmiPrintf("Charm++: +checksum ignored in optimized version! \n");
#endif
  }

#if CMK_SMP
  posix_memalign((void**)&procState, 128, (_Cmi_mynodesize) * sizeof(ProcState));

#if SPECIFIC_PCQUEUE
  //if(CmiMyPe() == 0)
  //  printf(" in L2Atomic Queue\n");
  LRTSQueuePreInit();
  //reserve for pe queues and node queue first
   int actualNodeSize = 64/Kernel_ProcessCount(); 
   CmiMemAllocInit_bgq ((char*)l2atomicbuf + 
       (QUEUE_NUMS)*sizeof(L2AtomicState),
       2*actualNodeSize*sizeof(L2AtomicState)); 
#else

#if CMK_PPC_ATOMIC_QUEUE
#if CMK_BLUEGENEQ
    int actualNodeSize = 64/Kernel_ProcessCount();
#else
    int actualNodeSize = _Cmi_mynodesize;
#endif

#if CMK_PPC_ATOMIC_MUTEX
    //Allocate for PPC Atomic Mutex as well
    size_t size = (_Cmi_mynodesize + 6*actualNodeSize + 1)
      * sizeof(PPCAtomicState) + 2*sizeof(PPCAtomicMutex);
#else
    size_t size = (_Cmi_mynodesize + 6*actualNodeSize + 1)
      * sizeof(PPCAtomicState);
#endif

    void *atomic_buf;
    PPC_AtomicCounterAllocate(&atomic_buf, size);

    char *atomic_start = (char *) atomic_buf;
    for (i=0; i<_Cmi_mynodesize; i++) {
      PPCAtomicQueueInit (atomic_start + sizeof(PPCAtomicState)*i,
			  sizeof(PPCAtomicState),
			  &procState[i].atomic_queue,
			  1, /*use overflow*/
			  DEFAULT_SIZE /*2048 entries*/);
    }
    atomic_start += _Cmi_mynodesize * sizeof(PPCAtomicState);

    CmiMemAllocInit_ppcq(atomic_start,6*actualNodeSize*sizeof(PPCAtomicState));
    atomic_start += 6*actualNodeSize*sizeof(PPCAtomicState);

    PPCAtomicQueueInit (atomic_start,
			sizeof(PPCAtomicState),
			&node_recv_atomic_q,
			1, /*use overflow*/
			DEFAULT_SIZE /*2048 entries*/);
    atomic_start += sizeof(PPCAtomicState);

#if CMK_PPC_ATOMIC_MUTEX
    node_recv_mutex = PPCAtomicMutexInit(atomic_start, sizeof(PPCAtomicMutex));
#endif
#endif //End of CMK_PPC_ATOMIC_MUTEX

#endif //End of SPECIFIC_PCQUEUE

#endif //End of CMK_SMP

#if CMK_PERSISTENT_COMM
  _initPersistent(cmi_pami_contexts, _n);
#endif      

#if CMK_ONESIDED_IMPL
  _initOnesided(cmi_pami_contexts, _n);
#endif
}

void LrtsPreCommonInit(int everReturn)
{
#if CMK_SMP && !CMK_ENABLE_ASYNC_PROGRESS
  CpvInitialize(int, uselock);
  CpvAccess(uselock) = 1;
#endif
#if CMK_SMP && CMK_ENABLE_ASYNC_PROGRESS
  if(CmiMyRank() == 0) {
    CMI_Progress_init(0, cmi_pami_numcontexts);
  }
#endif
}

void LrtsPostCommonInit(int everReturn)
{
  //printf ("before calling CmiBarrier() \n");
  CmiBarrier();
}

void LrtsPostNonLocal(void) {}

void LrtsDrainResources(void)
{
  while (MSGQLEN() > 0 || ORECVS() > 0) {
    LrtsAdvanceCommunication(0);
  }
  CmiNodeBarrier();
}

void LrtsExit(int exitcode)
{
#if !CMK_BLUEGENEQ
  CmiBarrier();
  CmiBarrier();
#endif

  int rank0 = 0, i = 0;
  CmiBarrier();
  if (CmiMyRank() == 0) {
    rank0 = 1;
#if CMK_SMP && CMK_ENABLE_ASYNC_PROGRESS
    CMI_Progress_finalize(0, cmi_pami_numcontexts);
#endif
    PAMI_Context_destroyv(cmi_pami_contexts, cmi_pami_numcontexts);
    PAMI_Client_destroy(&cmi_pami_client);
  }

  CmiNodeBarrier();
  if(!CharmLibInterOperate || userDrivenMode) {
#if CMK_SMP
    if (rank0) {
      // Wait for other threads (except me and the comm thread) to exit and join
      for(i=0; i< (_Cmi_mynodesize - 1) ; i++)
        pthread_join(_Cmi_mypidlist[i], NULL);
      exit(exitcode);
    }
    else
      pthread_exit(&exitcode);
#else
    exit(exitcode);
#endif
  }
}

void LrtsAbort(const char *message) {
  assert(0);
  CMI_NORETURN_FUNCTION_END
}

INLINE_KEYWORD void LrtsBeginIdle(void) {}

INLINE_KEYWORD void LrtsStillIdle(void) {}

void LrtsNotifyIdle(void)
{
#if CMK_BLUEGENEQ && CMK_SMP && CMK_PAMI_MULTI_CONTEXT
#if !CMK_ENABLE_ASYNC_PROGRESS && SPECIFIC_QUEUE  
  //Wait on the atomic queue to get a message with very low core
  //overheads. One thread calls advance more frequently
  ////spin wait for 2-4us when idle
  ////process node queue messages every 10us
  ////Idle cores will only use one LMQ slot and an int sum
  CmiState cs = CmiGetStateN(rank);
  if ((CmiMyRank()% THREADS_PER_CONTEXT) == 0)
  {LRTSQueueSpinWait(CmiMyRecvQueue(), 
			    10);}
  else
#endif
#if 0 && SPECIFIC_QUEUE && CMK_NODE_QUEUE_AVAILABLE 
  { LRTSQueueSpinWait(CmiMyRecvQueue(), 
			    1000);
  }
#endif
#endif
}
pami_result_t machine_send_handoff (pami_context_t context, void *msg);
void  machine_send       (pami_context_t      context, 
    int                 node, 
    int                 rank, 
    int                 size, 
    char              * msg, 
    int                 to_lock)__attribute__((always_inline));

CmiCommHandle LrtsSendFunc(int node, int destPE, int size, char *msg, int to_lock)
{
#if CMK_SMP && CMK_ENABLE_ASYNC_PROGRESS
  //int c = myrand(&r_seed) % cmi_pami_numcontexts;
  int c = node % cmi_pami_numcontexts;
  pami_context_t my_context = cmi_pami_contexts[c];    
  CmiMsgHeaderBasic *hdr = (CmiMsgHeaderBasic *)msg;
  hdr->dstnode = node;
  hdr->size    = size;

  PAMI_Context_post(my_context, (pami_work_t *)hdr->work, 
      machine_send_handoff, msg);
#else
  pami_context_t my_context = MY_CONTEXT();    
  machine_send (my_context, node, CMI_DEST_RANK(msg), size, msg, to_lock);
#endif
  return 0;
}

#if CMK_SMP && CMK_ENABLE_ASYNC_PROGRESS
pami_result_t machine_send_handoff (pami_context_t context, void *msg) {
  CmiMsgHeaderBasic *hdr = (CmiMsgHeaderBasic *)msg;
  int node = hdr->dstnode;
  int rank = hdr->rank;
  int size = hdr->size;

  //As this is executed on the comm thread no locking is necessary
  machine_send(context, node, rank, size, (char*)msg, 0);
  return PAMI_SUCCESS;
}
#endif

void  machine_send       (pami_context_t      context, 
    int                 node, 
    int                 rank, 
    int                 size, 
    char              * msg, 
    int                 to_lock) 
{
  CMI_MAGIC(msg) = CHARM_MAGIC_NUMBER;
  CMI_MSG_SIZE(msg) = size;
  CMI_SET_CHECKSUM(msg, size);

  pami_endpoint_t target;

#if CMK_SMP && !CMK_ENABLE_ASYNC_PROGRESS
  to_lock = CpvAccess(uselock);
#endif

#if CMK_PAMI_MULTI_CONTEXT &&  CMK_NODE_QUEUE_AVAILABLE
  size_t dst_context = (rank != DGRAM_NODEMESSAGE) ? (rank>>LTPS) : (rand_r(&r_seed) % cmi_pami_numcontexts);
  //Choose a context at random
  //size_t dst_context = myrand(&r_seed) % cmi_pami_numcontexts;
#else
  size_t dst_context = 0;
#endif
  PAMI_Endpoint_create (cmi_pami_client, (pami_task_t)node, dst_context, &target);

  //fprintf (stderr, "Calling PAMI Send to %d magic %d size %d\n", node, CMI_MAGIC(msg), size);
  if (CMI_LIKELY(size < SHORT_CUTOFF)) {
    pami_send_immediate_t parameters;

    parameters.dispatch        = CMI_PAMI_DISPATCH;
    if ( CMI_LIKELY(CMI_BROADCAST_ROOT(msg) == 0))
#if CMK_NODE_QUEUE_AVAILABLE
      if ( CMI_LIKELY(rank != DGRAM_NODEMESSAGE) )
#endif
        //use short callback if not a bcast and not an SMP node message
        parameters.dispatch        = CMI_PAMI_SHORT_DISPATCH;

    parameters.header.iov_base = NULL; //&rank;
    parameters.header.iov_len  = 0;    //sizeof(int);
    parameters.data.iov_base   = msg;
    parameters.data.iov_len    = size;
    parameters.dest = target;

    if(to_lock)
      PAMIX_CONTEXT_LOCK(context);

    PAMI_Send_immediate (context, &parameters);

    if(to_lock)
      PAMIX_CONTEXT_UNLOCK(context);
    CmiFree(msg);
  }
  else if (size < EAGER_CUTOFF) {
    pami_send_t parameters;
    parameters.send.dispatch        = CMI_PAMI_DISPATCH;
    parameters.send.header.iov_base = NULL; //&rank;
    parameters.send.header.iov_len  = 0;    //sizeof(int);
    parameters.send.data.iov_base   = msg;
    parameters.send.data.iov_len    = size;
    parameters.events.cookie        = msg;
    parameters.events.local_fn      = send_done;
    parameters.events.remote_fn     = NULL;
    memset(&parameters.send.hints, 0, sizeof(parameters.send.hints));
    parameters.send.dest = target;

    if (to_lock)
      PAMIX_CONTEXT_LOCK(context);
    INCR_MSGQLEN();
    PAMI_Send (context, &parameters);
    if (to_lock)
      PAMIX_CONTEXT_UNLOCK(context);
  }
  else {
    CmiPAMIRzv_t   rzv;
    rzv.bytes       = size;
    rzv.buffer      = msg;
#if CMK_BLUEGENEQ
    rzv.offset      = (size_t)msg - (size_t)cmi_pami_memregion[0].baseVA;
#else
    rzv.offset      = (size_t)msg;
    size_t bytes_out;
    pami_memregion_t mregion;
    //In use for PAMI_Get
    PAMI_Memregion_create (context,
                           msg,
                           size,
                           &bytes_out,
                           &mregion);
#endif
    rzv.dst_context = dst_context;

    pami_send_immediate_t parameters;
    parameters.dispatch        = CMI_PAMI_RZV_DISPATCH;
    parameters.header.iov_base = &rzv;
    parameters.header.iov_len  = sizeof(rzv);
#if CMK_BLUEGENEQ
    parameters.data.iov_base   = &cmi_pami_memregion[0].mregion;      
    parameters.data.iov_len    = sizeof(pami_memregion_t);
#else
    parameters.data.iov_base   = NULL;
    parameters.data.iov_len    = 0;
#endif
    parameters.dest = target;

    if(to_lock)
      PAMIX_CONTEXT_LOCK(context);

    PAMI_Send_immediate (context, &parameters);

    if(to_lock)
      PAMIX_CONTEXT_UNLOCK(context);
  }
}

#if !CMK_ENABLE_ASYNC_PROGRESS  
//threads have to progress contexts themselves  
void LrtsAdvanceCommunication(int whenidle) {
  pami_context_t my_context = MY_CONTEXT();

#if CMK_SMP
  if (PAMIX_CONTEXT_TRYLOCK(my_context))
  {
    PAMI_Context_advance(my_context, 1);
    PAMIX_CONTEXT_UNLOCK(my_context);
  }
#else
  PAMI_Context_advance(my_context, 1);
#endif
}
#endif

static pami_result_t machine_network_barrier(pami_context_t   my_context, 
    int              to_lock) 
{
  if (to_lock)
    PAMIX_CONTEXT_LOCK(my_context);    

#ifdef LIBCOLL
  libcoll_result_t result = LIBCOLL_SUCCESS;
  result = LIBCOLL_Start (&cmi_libcoll_context, &xfer_barrier);
  if (result != LIBCOLL_SUCCESS)
    fprintf (stderr, "Error. Unable to issue  collective. result = %d\n", result);
#else
  pami_result_t result = PAMI_SUCCESS;    
  result = PAMI_Collective(my_context, &xfer_barrier);       
  if (result != PAMI_SUCCESS)
    fprintf (stderr, "Error. Unable to issue  collective. result = %d\n", result);
#endif
  if (to_lock)
    PAMIX_CONTEXT_UNLOCK(my_context);
  return (pami_result_t)result;
}

pami_result_t network_barrier_handoff(pami_context_t context, void *msg)
{
  return machine_network_barrier(context, 0);
}

void LrtsBarrier(void)
{
    CmiNetworkBarrier(1);
}
static void CmiNetworkBarrier(int async) {
  if (CmiNumNodes() == 1) return;

  pami_context_t my_context = cmi_pami_contexts[0];
  coll_barrier_flag = 1;
#if CMK_SMP && CMK_ENABLE_ASYNC_PROGRESS
  if (async) {
    pami_work_t work;
    PAMI_Context_post(my_context, &work, network_barrier_handoff, NULL);
    while (coll_barrier_flag);
    //fprintf (stderr, "After Network Barrier\n");
  }
  else 
#endif
  {
    machine_network_barrier(my_context, 1);    
    PAMIX_CONTEXT_LOCK(my_context);
    while (coll_barrier_flag)
      PAMI_Context_advance (my_context, 100);
    PAMIX_CONTEXT_UNLOCK(my_context);
  }
}

static void  sendAck (pami_context_t      context,
    CmiPAMIRzvRecv_t      *recv) 
{
  pami_send_immediate_t parameters;
  parameters.dispatch        = CMI_PAMI_ACK_DISPATCH;
  parameters.header.iov_base = &recv->src_buffer; 
  parameters.header.iov_len  = sizeof(void *);    
  parameters.data.iov_base   = NULL;
  parameters.data.iov_len    = 0;
  parameters.dest            = recv->src_ep;

  //Called from advance and hence we dont need a mutex
  PAMI_Send_immediate (context, &parameters);
}


void rzv_recv_done   (pami_context_t     ctxt, 
    void             * clientdata, 
    pami_result_t      result) 
{
  CmiPAMIRzvRecv_t recv = *(CmiPAMIRzvRecv_t *)clientdata;
  recv_done(ctxt, recv.msg, PAMI_SUCCESS);
  sendAck(ctxt, &recv);
}

void rzv_pkt_dispatch (pami_context_t       context,   
    void               * clientdata,
    const void         * header_addr,
    size_t               header_size,
    const void         * pipe_addr,  
    size_t               pipe_size,  
    pami_endpoint_t      origin,
    pami_recv_t         * recv) 
{
  INCR_ORECVS();    

  CmiPAMIRzv_t  *rzv_hdr = (CmiPAMIRzv_t *) header_addr;
  CmiAssert (header_size == sizeof(CmiPAMIRzv_t));  
  int alloc_size = rzv_hdr->bytes;
  char * buffer  = (char *)CmiAlloc(alloc_size + sizeof(CmiPAMIRzvRecv_t));
  //char *buffer=(char*)CmiAlloc(alloc_size+sizeof(CmiPAMIRzvRecv_t)+sizeof(int))
  //*(int *)(buffer+alloc_size) = *(int *)header_addr;  
  CmiAssert (recv == NULL);

  CmiPAMIRzvRecv_t *rzv_recv = (CmiPAMIRzvRecv_t *)(buffer+alloc_size);
  rzv_recv->msg        = buffer;
  rzv_recv->src_ep     = origin;
  rzv_recv->src_buffer = rzv_hdr->buffer;

#if CMK_BLUEGENEQ
  CmiAssert (pipe_addr != NULL);
  pami_memregion_t *mregion = (pami_memregion_t *) pipe_addr;
  CmiAssert (pipe_size == sizeof(pami_memregion_t));

  //Rzv inj fifos are on the 17th core shared by all contexts
  pami_rget_simple_t  rget;
  rget.rma.dest    = origin;
  rget.rma.bytes   = rzv_hdr->bytes;
  rget.rma.cookie  = rzv_recv;
  rget.rma.done_fn = rzv_recv_done;
  rget.rma.hints.buffer_registered = PAMI_HINT_ENABLE;
  rget.rma.hints.use_rdma = PAMI_HINT_ENABLE;
  rget.rdma.local.mr      = &cmi_pami_memregion[rzv_hdr->dst_context].mregion;  
  rget.rdma.local.offset  = (size_t)buffer - 
    (size_t)cmi_pami_memregion[rzv_hdr->dst_context].baseVA;
  rget.rdma.remote.mr     = mregion; //from message payload
  rget.rdma.remote.offset = rzv_hdr->offset;

  //printf ("starting rget\n");
  PAMI_Rget (context, &rget);  
#else
  size_t bytes_out;
  pami_memregion_t mregion;
  //In use for PAMI_Get
  PAMI_Memregion_create (context,
			 buffer,
			 rzv_hdr->bytes,
			 &bytes_out,
			 &mregion);

  pami_get_simple_t get;
  memset(&get, 0, sizeof(get));
  get.rma.dest = origin;
  get.rma.bytes = rzv_hdr->bytes;
  get.rma.cookie = rzv_recv;
  get.rma.done_fn = rzv_recv_done;
  get.rma.hints.use_rdma = PAMI_HINT_ENABLE;
  get.rma.hints.buffer_registered = PAMI_HINT_ENABLE;
  get.rma.hints.use_shmem = PAMI_HINT_DEFAULT;
  get.rma.hints.remote_async_progress = PAMI_HINT_DEFAULT;
  get.addr.local = buffer;
  get.addr.remote = (void*)rzv_hdr->offset;
  PAMI_Get(context, &get);
#endif
}

void ack_pkt_dispatch (pami_context_t       context,   
    void               * clientdata,
    const void         * header_addr,
    size_t               header_size,
    const void         * pipe_addr,  
    size_t               pipe_size,  
    pami_endpoint_t      origin,
    pami_recv_t         * recv) 
{
  char **buf = (char **)header_addr;
  CmiFree (*buf);
}


/*==========================================================*/

/* Optional routines which could use common code which is shared with
   other machine layer implementations. */

/* MULTICAST/VECTOR SENDING FUNCTIONS

 * In relations to some flags, some other delivery functions may be needed.
 */

#if ! CMK_MULTICAST_LIST_USE_COMMON_CODE

void LrtsSyncListSendFn(int npes, const int *pes, int size, char *msg) {
  char *copymsg;
  copymsg = (char *)CmiAlloc(size);
  CmiMemcpy(copymsg,msg,size);
  CmiFreeListSendFn(npes, pes, size, msg);
}

typedef struct ListMulticastVec_t {
  int   *pes;
  int    npes;
  char  *msg;
  int    size;
} ListMulticastVec;

void machineFreeListSendFn(pami_context_t    context, 
    int               npes, 
    const int       * pes,
    int               size, 
    char            * msg);

pami_result_t machineFreeList_handoff(pami_context_t context, void *cookie)
{
  ListMulticastVec *lvec = (ListMulticastVec *) cookie;
  machineFreeListSendFn(context, lvec->npes, lvec->pes, lvec->size, lvec->msg);
  CmiFree(cookie);
  return PAMI_SUCCESS;
}

void LrtsFreeListSendFn(int npes, const int *pes, int size, char *msg) {
  //printf("%d: In Free List Send Fn imm %d\n", CmiMyPe(), CmiIsImmediate(msg));

  CMI_SET_BROADCAST_ROOT(msg,0);
  CMI_MAGIC(msg) = CHARM_MAGIC_NUMBER;
  CmiMsgHeaderBasic *hdr = (CmiMsgHeaderBasic *)msg;
  hdr->size = size;

  //Fast path
  if (npes == 1) {
    CMI_DEST_RANK(msg) = CmiRankOf(pes[0]);
    LrtsSendFunc(CmiGetNodeGlobal(CmiNodeOf(pes[0]),CmiMyPartition()), pes[0], size, msg, 1);
    return;
  }

  pami_context_t my_context = MY_CONTEXT();
#if CMK_SMP && CMK_ENABLE_ASYNC_PROGRESS
  ListMulticastVec *lvec = (ListMulticastVec *) 
    CmiAlloc(sizeof(ListMulticastVec) + sizeof(int)*npes);
  lvec->pes = (int*)((char*)lvec + sizeof(ListMulticastVec));
  int i = 0;
  for (i=0; i<npes; i++) 
    lvec->pes[i] = pes[i];
  lvec->npes = npes;
  lvec->msg  = msg;
  lvec->size = size;
  PAMI_Context_post(my_context, (pami_work_t*)hdr->work, 
      machineFreeList_handoff, lvec);
#else
  machineFreeListSendFn(my_context, npes, pes, size, msg);
#endif
}

void machineFreeListSendFn(pami_context_t my_context, int npes, const int *pes, int size, char *msg) {
  int i;
  char *copymsg;
#if CMK_SMP
  for (i=0; i<npes; i++) {
    if (CmiNodeOf(pes[i]) == CmiMyNode()) {
      copymsg = (char *)CmiAlloc(size);
      CmiAssert(copymsg != NULL);
      CmiMemcpy(copymsg,msg,size);	  
      CmiSendPeer(CmiRankOf(pes[i]), size, copymsg);
    }
  }
#else
  for (i=0; i<npes; i++) {
    if (CmiNodeOf(pes[i]) == CmiMyNode()) {
      CmiSyncSend(pes[i], size, msg);
    }
  }
#endif

  PAMIX_CONTEXT_LOCK(my_context);
#if CMK_SMP  && !CMK_ENABLE_ASYNC_PROGRESS
  CpvAccess(uselock) = 0;
#endif

  for (i=0;i<npes;i++) {
    if (CmiNodeOf(pes[i]) == CmiMyNode());
    else if (i < npes - 1) {
#if !CMK_SMP
      CmiReference(msg);
      copymsg = msg;
#else
      copymsg = (char *)CmiAlloc(size);
      CmiAssert(copymsg != NULL);
      CmiMemcpy(copymsg,msg,size);
#endif
      CMI_DEST_RANK(copymsg) = CmiRankOf(pes[i]);
      LrtsSendFunc(CmiGetNodeGlobal(CmiNodeOf(pes[i]),CmiMyPartition()), pes[i], size, copymsg, 0);
    }
  }

  if (npes  && CmiNodeOf(pes[npes-1]) != CmiMyNode()) {
    CMI_DEST_RANK(msg) = CmiRankOf(pes[npes-1]);
    LrtsSendFunc(CmiGetNodeGlobal(CmiNodeOf(pes[npes-1]),CmiMyPartition()), pes[npes-1], size, msg, 0);
  }
  else
    CmiFree(msg);    

  PAMIX_CONTEXT_UNLOCK(my_context);
#if CMK_SMP  && !CMK_ENABLE_ASYNC_PROGRESS
  CpvAccess(uselock) = 1;
#endif
}

CmiCommHandle LrtsAsyncListSendFn(int npes, const int *pes, int size, char *msg) {
  CmiAbort("CmiAsyncListSendFn not implemented.");
  return (CmiCommHandle) 0;
}
#endif

#ifdef LIBCOLL

static void libcoll_init_done (void *ctxt, void * clientdata, libcoll_result_t err)
{
  int * active = (int *) clientdata;
  (*active)--;
}

void init_barrier_libcoll () {
    ///////////---------------------------------/////////////////////
    //////////----------- Initialize Barrier -------////////////////
    /* Docs01:  Initialize LIBCOLL */
    libcoll_attribute_t attribute[3];
    size_t nattr = 3;
    attribute[0].attribute_id    = LIBCOLL_PAMI_CLIENT;
    attribute[0].attribute_value = cmi_pami_client;
    attribute[1].attribute_id    = LIBCOLL_PAMI_CONTEXT_POINTER;
    attribute[1].attribute_value = (void *) cmi_pami_contexts;
    attribute[2].attribute_id    = LIBCOLL_PAMI_NUM_CONTEXTS;
    attribute[2].attribute_value = (void *) 1UL;

    libcoll_epdomain_t epdomain;
    volatile int active = 1;
    LIBCOLL_Init("CMI LIBCOLL",
                 attribute, nattr,
                 libcoll_init_done, (void *)&active,
                 &cmi_libcoll_context,
                 &epdomain, &world_geometry);
    //Wait for libcoll init
    while (active) LIBCOLL_Advance(cmi_libcoll_context);

    size_t               num_algorithm[2];
    libcoll_algorithm_t    *always_works_algo = NULL;
    libcoll_metadata_t     *always_works_md = NULL;
    libcoll_algorithm_t    *must_query_algo = NULL;
    libcoll_metadata_t     *must_query_md = NULL;
    libcoll_xfer_type_t     xfer_type = LIBCOLL_XFER_BARRIER;

    libcoll_result_t result = LIBCOLL_Geometry_algorithms_num(world_geometry,
                                                              xfer_type,
                                                              num_algorithm);

    if (result != LIBCOLL_SUCCESS || num_algorithm[0]==0)
    {
        fprintf (stderr,
                 "Error. Unable to query algorithm, or no algorithms available result = %d\n",
                 result);
        return;
    }

    always_works_algo = (libcoll_algorithm_t*)malloc(sizeof(libcoll_algorithm_t)*num_algorithm[0]);
    always_works_md  = (libcoll_metadata_t*)malloc(sizeof(libcoll_metadata_t)*num_algorithm[0]);
    must_query_algo   = (libcoll_algorithm_t*)malloc(sizeof(libcoll_algorithm_t)*num_algorithm[1]);
    must_query_md    = (libcoll_metadata_t*)malloc(sizeof(libcoll_metadata_t)*num_algorithm[1]);

    /* Docs05:  Query the algorithm lists */
    result = LIBCOLL_Geometry_algorithms_query(world_geometry,
                                               xfer_type,
                                               always_works_algo,
                                               always_works_md,
                                               num_algorithm[0],
                                               must_query_algo,
                                               must_query_md,
                                               num_algorithm[1]);
    int opt_alg = 0, nalg = 0;
    for (nalg = 0; nalg < num_algorithm[0]; ++nalg)
        if (strstr(always_works_md[nalg].name, "SHMEM") != NULL) {
            opt_alg = nalg;
            break;
        }

    if ((_Cmi_mynode == 0) && (!quietMode))
        printf ("Choosing optimized barrier algorithm name %s\n",
                always_works_md[opt_alg].name);

    xfer_barrier.cb_done   = (libcoll_event_function) coll_barrier_done;
    xfer_barrier.cookie    = (void*) & coll_barrier_flag;
    xfer_barrier.algorithm = always_works_algo[opt_alg];
    xfer_barrier.geometry  = world_geometry;

    /* Docs06:  Query the algorithm lists */
    if (result != LIBCOLL_SUCCESS)
    {
        fprintf (stderr, "Error. Unable to get query algorithm. result = %d\n", result);
        return;
    }
}

#else
void init_barrier_pami () {
    ///////////---------------------------------/////////////////////
    //////////----------- Initialize Barrier -------////////////////
    size_t               num_algorithm[2];
    pami_algorithm_t    *always_works_algo = NULL;
    pami_metadata_t     *always_works_md = NULL;
    pami_algorithm_t    *must_query_algo = NULL;
    pami_metadata_t     *must_query_md = NULL;
    pami_xfer_type_t     xfer_type = PAMI_XFER_BARRIER;

    /* Docs01:  Get the World Geometry */
    pami_result_t result = PAMI_Geometry_world (cmi_pami_client,&world_geometry);
    if (result != PAMI_SUCCESS)
    {
        fprintf (stderr, "Error. Unable to get world geometry: result = %d\n", result);
        return;
    }

    result = PAMI_Geometry_algorithms_num(world_geometry,
                                          xfer_type,
                                          (size_t*)num_algorithm);

    if (result != PAMI_SUCCESS || num_algorithm[0]==0)
    {
        fprintf (stderr,
                 "Error. Unable to query algorithm, or no algorithms available result = %d\n",
                 result);
        return;
    }

    always_works_algo = (pami_algorithm_t*)malloc(sizeof(pami_algorithm_t)*num_algorithm[0]);
    always_works_md  = (pami_metadata_t*)malloc(sizeof(pami_metadata_t)*num_algorithm[0]);
    must_query_algo   = (pami_algorithm_t*)malloc(sizeof(pami_algorithm_t)*num_algorithm[1]);
    must_query_md    = (pami_metadata_t*)malloc(sizeof(pami_metadata_t)*num_algorithm[1]);

    /* Docs05:  Query the algorithm lists */
    result = PAMI_Geometry_algorithms_query(world_geometry,
                                            xfer_type,
                                            always_works_algo,
                                            always_works_md,
                                            num_algorithm[0],
                                            must_query_algo,
                                            must_query_md,
                                            num_algorithm[1]);

    int opt_alg = 0, nalg = 0;
    for (nalg = 0; nalg < num_algorithm[0]; ++nalg)
        if (strstr(always_works_md[nalg].name, "GI") != NULL) {
            opt_alg = nalg;
            break;
        }

    if ((_Cmi_mynode == 0) && (!quietMode))
        printf ("Choosing optimized barrier algorithm name %s\n",
	              always_works_md[opt_alg]);

    xfer_barrier.cb_done   = coll_barrier_done;
    xfer_barrier.cookie    = (void*) & coll_barrier_flag;
    xfer_barrier.algorithm = always_works_algo[opt_alg];

    /* Docs06:  Query the algorithm lists */
    if (result != PAMI_SUCCESS)
    {
        fprintf (stderr, "Error. Unable to get query algorithm. result = %d\n", result);
        return;
    }
}
#endif

void init_barrier () {
#ifndef LIBCOLL
  init_barrier_pami();
#else
  init_barrier_libcoll();
#endif
}

#if CMK_BLUEGENEQ
#include "cmimemcpy_qpx.h"
#endif

#if CMK_PERSISTENT_COMM
#include "machine-persistent.C"
#endif

#if CMK_ONESIDED_IMPL
#include "machine-onesided.C"
#endif


/*
 *created by Chao Mei
 *revised by Yanhua, Gengbin
 */

#if CMK_C_INLINE
#define INLINE_KEYWORD inline
#else
#define INLINE_KEYWORD
#endif

#if MACHINE_DEBUG_LOG
FILE *debugLog = NULL;
#endif

int cmiNcpyAckSize;

// Macro for message type
#define CMI_CMA_MSGTYPE(msg)         ((CmiMsgHeaderBasic *)msg)->cmaMsgType

/******* broadcast related  */
#ifndef CMK_BROADCAST_SPANNING_TREE
#define CMK_BROADCAST_SPANNING_TREE    1
#endif

#ifndef CMK_BROADCAST_HYPERCUBE
#define CMK_BROADCAST_HYPERCUBE        0
#endif

#define BROADCAST_SPANNING_FACTOR      4
/* The number of children used when a msg is broadcast inside a node */
#define BROADCAST_SPANNING_INTRA_FACTOR  8

/**
 * For some machine layers such as on Active Message framework,
 * the receiver callback is usally executed on an internal
 * thread (i.e. not the flow managed by ours). Therefore, for
 * forwarding broadcast messages, we could have a choice whether
 * to offload such function to the flow we manage such as the
 * communication thread. -
 */

#ifndef CMK_OFFLOAD_BCAST_PROCESS
#define CMK_OFFLOAD_BCAST_PROCESS 0
#endif

#if CMK_OFFLOAD_BCAST_PROCESS
CsvDeclare(CMIQueue, procBcastQ);
#if CMK_NODE_QUEUE_AVAILABLE
CsvDeclare(CMIQueue, nodeBcastQ);
#endif
#endif

static int _exitcode;

#if CMK_WITH_STATS
static int  MSG_STATISTIC = 0;
int     msg_histogram[22];
static int _cmi_log2(int size)
{
    int ret = 1;
    size = size-1;
    while( (size=size>>1)>0) ret++;
    return ret;
}
#endif

#if CMK_BROADCAST_HYPERCUBE
/* ceil(log2(CmiNumNodes)) except when _Cmi_numnodes is 1, used for hypercube */
static int CmiNodesDim;
#endif
/* ###End of Broadcast related definitions ### */

#if CMK_SMP_TRACE_COMMTHREAD
double TraceTimerCommon(void);
#endif

static void handleOneBcastMsg(size_t size, char *msg);
static void processBcastQs(void);

/* Utility functions for forwarding broadcast messages,
 * should not be used in machine-specific implementations
 * except in some special occasions.
 */
static INLINE_KEYWORD void processProcBcastMsg(size_t size, char *msg);
static INLINE_KEYWORD void processNodeBcastMsg(size_t size, char *msg);
static void SendSpanningChildrenProc(size_t size, char *msg);
static void SendHyperCubeProc(size_t size, char *msg);
#if CMK_NODE_QUEUE_AVAILABLE
static void SendSpanningChildrenNode(size_t size, char *msg);
static void SendHyperCubeNode(size_t size, char *msg);
#endif

static void SendSpanningChildren(size_t size, char *msg, int rankToAssign, int startNode);
static void SendHyperCube(size_t size,  char *msg, int rankToAssign, int startNode);

#if USE_COMMON_SYNC_BCAST || USE_COMMON_ASYNC_BCAST
#if !CMK_BROADCAST_SPANNING_TREE && !CMK_BROADCAST_HYPERCUBE
#warning "Broadcast function is based on the plain P2P O(P)-message scheme!!!"
#endif
#endif

#include <assert.h>

void CmiSyncBroadcastFn(size_t size, char *msg);
CmiCommHandle CmiAsyncBroadcastFn(size_t size, char *msg);
void CmiFreeBroadcastFn(size_t size, char *msg);

void CmiSyncBroadcastAllFn(size_t size, char *msg);
CmiCommHandle CmiAsyncBroadcastAllFn(size_t size, char *msg);
void CmiFreeBroadcastAllFn(size_t size, char *msg);

#if CMK_NODE_QUEUE_AVAILABLE
void CmiSyncNodeBroadcastFn(size_t size, char *msg);
CmiCommHandle CmiAsyncNodeeroadcastFn(size_t size, char *msg);
void CmiFreeNodeBroadcastFn(size_t size, char *msg);

void CmiSyncNodeBroadcastAllFn(size_t size, char *msg);
CmiCommHandle CmiAsyncNodeBroadcastAllFn(size_t size, char *msg);
void CmiFreeNodeBroadcastAllFn(size_t size, char *msg);
#endif

/************** Done with Broadcast related */

#define CMI_DEST_RANK(msg)               ((CmiMsgHeaderBasic *)msg)->rank

#ifndef CMK_HAS_SIZE_IN_MSGHDR
#define CMK_HAS_SIZE_IN_MSGHDR 1
#endif
#if CMK_HAS_SIZE_IN_MSGHDR
#define CMI_MSG_SIZE(msg)  ((CmiMsgHeaderBasic *)msg)->size
#else
#define CMI_MSG_SIZE(msg)  (CmiAbort("Has no msg size in header"))
#endif

#if CMK_NODE_QUEUE_AVAILABLE
/* This value should be larger than the number of cores used
 * per charm smp node. So it's currently set to such a large
 * value.
 */
#define DGRAM_NODEMESSAGE   (0x1FFB)
#endif

// global state, equals local if running one partition
PartitionInfo _partitionInfo;
int _Cmi_mype_global;
int _Cmi_numpes_global;
int _Cmi_mynode_global;
int _Cmi_numnodes_global;
static int _writeToStdout = 1;

// Node state structure, local information for the partition
int               _Cmi_myphysnode_numprocesses;  /* Total number of processes within this node */
int               _Cmi_mynodesize;/* Number of processors in my address space */
int               _Cmi_mynode;    /* Which address space am I */
int               _Cmi_numnodes;  /* Total number of address spaces */
int               _Cmi_numpes;    /* Total number of processors */
int               userDrivenMode; /* Set by CharmInit for interop in user driven mode */
extern int CharmLibInterOperate;

CpvDeclare(void*, CmiLocalQueue);

/* different modes for sending a message */
#define P2P_SYNC      0x1
#define P2P_ASYNC     0x2
#define BCAST_SYNC    0x4
#define BCAST_ASYNC   0x8
#define OUT_OF_BAND   0x10

enum MACHINE_SMP_MODE {
    INVALID_MODE,
#if CMK_BLUEGENEQ
    COMM_THREAD_SEND_RECV = 0,
#else 
    COMM_THREAD_SEND_RECV = 0,
#endif
    COMM_THREAD_ONLY_RECV, /* work threads will do the send */
    COMM_WORK_THREADS_SEND_RECV, /* work and comm threads do the both send/recv */
    COMM_THREAD_NOT_EXIST /* work threads will do both send and recv */
};
/* The default mode of smp charm runtime */
static enum MACHINE_SMP_MODE Cmi_smp_mode_setting = COMM_THREAD_SEND_RECV;

/* Machine layer dependent specific modes of the smp charm runtime are set in individual machine layers */

#if CMK_OMP
/* Suspended tasks should be in a separate queue 
 * They are pushed by the master thread who generated the tasks. When they are stolen by other PEs and suspended on the other PEs, they should be reinserted to the PEs local Queue so that the locality of resumed tasks can be maintained 
 * */
void CmiSuspendedTaskEnqueue(int targetRank, void *data);
void* CmiSuspendedTaskPop();
#endif

#include <atomic>

extern int CharmLibInterOperate;
std::atomic<int> ckExitComplete {0};

#if CMK_SMP
std::atomic<int> numPEsReadyForExit {0};

/**
 *  The macro defines whether to have a comm thd to offload some
 *  work such as forwarding bcast messages etc. This macro
 *  should be defined before including "machine-smp.C". Note
 *  that whether a machine layer in SMP mode could run w/o comm
 *  thread depends on the support of the underlying
 *  communication library.
 *
 */
#ifndef CMK_SMP_NO_COMMTHD
#define CMK_SMP_NO_COMMTHD CMK_MULTICORE
#endif

#if CMK_SMP_NO_COMMTHD
int Cmi_commthread = 0;
#else
int Cmi_commthread = 1;
#endif

#endif //CMK_SMP

/*SHOULD BE MOVED TO MACHINE-SMP.C ??*/
int Cmi_nodestart = -1;
static int Cmi_nodestartGlobal = -1;

/*
 * Network progress utility variables. Period controls the rate at
 * which the network poll is called
 */
#ifndef NETWORK_PROGRESS_PERIOD_DEFAULT
#define NETWORK_PROGRESS_PERIOD_DEFAULT 1000
#endif

CpvDeclare(unsigned , networkProgressCount);
int networkProgressPeriod;

#if CMK_CCS_AVAILABLE
extern int ccsRunning;
#endif

/* ===== Beginning of Common Function Declarations ===== */
CMK_NORETURN void CmiAbort(const char *message, ...);
static void PerrorExit(const char *msg);

/* This function handles the msg received as which queue to push into */
static void handleOneRecvedMsg(size_t size, char *msg);

/* Utility functions for forwarding broadcast messages,
 * should not be used in machine-specific implementations
 * except in some special occasions.
 */
static void SendToPeers(size_t size, char *msg);


void CmiPushPE(int rank, void *msg);

#if CMK_NODE_QUEUE_AVAILABLE
void CmiPushNode(void *msg);
#endif

/* Functions regarding send ops declared in converse.h */

/* In default, using the common codes for msg sending */
#ifndef USE_COMMON_SYNC_P2P
#define USE_COMMON_SYNC_P2P 1
#endif
#ifndef USE_COMMON_ASYNC_P2P
#define USE_COMMON_ASYNC_P2P 1
#endif
#ifndef USE_COMMON_SYNC_BCAST
#define USE_COMMON_SYNC_BCAST 1
#endif
#ifndef USE_COMMON_ASYNC_BCAST
#define USE_COMMON_ASYNC_BCAST 1
#endif

static void CmiSendSelf(char *msg);

void CmiSyncSendFn(int destPE, size_t size, char *msg);
CmiCommHandle CmiAsyncSendFn(int destPE, size_t size, char *msg);
void CmiFreeSendFn(int destPE, size_t size, char *msg);

#if CMK_NODE_QUEUE_AVAILABLE
static void CmiSendNodeSelf(char *msg);

void CmiSyncNodeSendFn(int destNode, size_t size, char *msg);
CmiCommHandle CmiAsyncNodeSendFn(int destNode, size_t size, char *msg);
void CmiFreeNodeSendFn(int destNode, size_t size, char *msg);

#endif

/* Functions and variables regarding machine startup */
static char     **Cmi_argv;
static char     **Cmi_argvcopy;
static CmiStartFn Cmi_startfn;   /* The start function */
static int        Cmi_usrsched;  /* Continue after start function finishes? */
void ConverseInit(int argc, char **argv, CmiStartFn fn, int usched, int initret);
static void ConverseRunPE(int everReturn);

/* Functions regarding machine running on every proc */
static void AdvanceCommunication(int whenidle);
static void CommunicationServer(int sleepTime);
void CommunicationServerThread(int sleepTime);

/* Functions providing incoming network messages */
void *CmiGetNonLocal(void);
#if CMK_NODE_QUEUE_AVAILABLE
void *CmiGetNonLocalNodeQ(void);
#endif
/* Utiltiy functions */
static char *CopyMsg(char *msg, size_t len);

/* ===== End of Common Function Declarations ===== */

#include "machine-smp.C"

/* ===== Beginning of Idle-state Related Declarations =====  */
typedef struct {
    int sleepMs; /*Milliseconds to sleep while idle*/
    int nIdles; /*Number of times we've been idle in a row*/
    CmiState cs; /*Machine state*/
} CmiIdleState;

static CmiIdleState *CmiNotifyGetState(void);

/**
 *  Generally,
 *
 *  CmiNotifyIdle is used in non-SMP mode when the proc is idle.
 *  When the proc is idle, AdvanceCommunication needs to be
 *  called.
 *
 *  CmiNotifyStillIdle and CmiNotifyBeginIdle are used in SMP mode.
 *
 *  Different layers have choices of registering different callbacks for
 *  idle state.
 */
static void CmiNotifyBeginIdle(CmiIdleState *s);
static void CmiNotifyStillIdle(CmiIdleState *s);
void CmiNotifyIdle(void);
/* ===== End of Idle-state Related Declarations =====  */

CsvDeclare(CmiNodeState, NodeState);
/* ===== Beginning of Processor/Node State-related Stuff =====*/
#if !CMK_SMP
/************ non SMP **************/
static struct CmiStateStruct Cmi_state;
int _Cmi_mype;
int _Cmi_myrank;

void CmiMemLock(void) {}
void CmiMemUnlock(void) {}

#define CmiGetState() (&Cmi_state)
#define CmiGetStateN(n) (&Cmi_state)

void CmiYield(void) {
    sleep(0);
}

static void CmiStartThreads(char **argv) {
    CmiStateInit(Cmi_nodestart, 0, &Cmi_state);
    _Cmi_mype = Cmi_nodestart;
    _Cmi_myrank = 0;
    _Cmi_mype_global = _Cmi_mynode_global;
}

INLINE_KEYWORD int CmiNodeSpan(void) {
  return 1;
}
#else
/************** SMP *******************/
INLINE_KEYWORD CMIQueue CmiMyRecvQueue(void) {
    return CmiGetState()->recv;
}

#if CMK_NODE_QUEUE_AVAILABLE
INLINE_KEYWORD
#if CMK_LOCKLESS_QUEUE
MPMCQueue
#else
CMIQueue
#endif
CmiMyNodeQueue(void) {
    return CsvAccess(NodeState).NodeRecv;
}
#endif

int CmiMyPe(void) {
    return CmiGetState()->pe;
}
int CmiNodeSpan(void) {
  return (CmiMyNodeSize() + 1);
}
int CmiMyPeGlobal(void) {
    return CmiGetPeGlobal(CmiGetState()->pe,CmiMyPartition());
}
int CmiMyRank(void) {
    return CmiGetState()->rank;
}
int CmiNodeSize(int node) {
    return _Cmi_mynodesize;
}
#if !CMK_MULTICORE // these are defined in converse.h
int CmiNodeFirst(int node) {
    return node*_Cmi_mynodesize;
}
int CmiNodeOf(int pe) {
    return (pe/_Cmi_mynodesize);
}
int CmiRankOf(int pe) {
    return pe%_Cmi_mynodesize;
}
#endif // end of !CMK_MULTICORE
#endif // end of CMK_SMP

static int CmiState_hasMessage(void) {
  CmiState cs = CmiGetState();
  return CmiIdleLock_hasMessage(cs);
}

// Function declaration
CmiCommHandle CmiInterSendNetworkFunc(int destPE, int partition, size_t size, char *msg, int mode);

/* ===== End of Processor/Node State-related Stuff =====*/

#include "machine-broadcast.C"
#include "immediate.C"
#include "machine-commthd-util.C"
#if CMK_USE_CMA
// cma_min_thresold and cma_max_threshold specify the range of sizes between which CMA will be used for SHM messaging
int cma_works, cma_reg_msg, cma_min_threshold, cma_max_threshold;
#include "machine-cma.C"
int CmiDoesCMAWork() {
  return cma_works;
}
#endif

/* ===== Beginning of Common Function Definitions ===== */
static void PerrorExit(const char *msg) {
    perror(msg);
    exit(1);
}

/* ##### Beginning of Functions Related with Message Sending OPs ##### */

extern "C" void CmiPushImmediateMsg(void *);

/*Add a message to this processor's receive queue, pe is a rank */
void CmiPushPE(int rank,void *msg) {
    CmiState cs = CmiGetStateN(rank);
    MACHSTATE2(3,"Pushing message into rank %d's queue %p{",rank, cs->recv);
#if CMK_IMMEDIATE_MSG
    if (CmiIsImmediate(msg)) {
        MACHSTATE1(3, "[%p] Push Immediate Message begin{",CmiGetState());
        CMI_DEST_RANK(msg) = rank;
        CmiPushImmediateMsg(msg);
        MACHSTATE1(3, "[%p] Push Immediate Message end}",CmiGetState());
        return;
    }
#endif

#if CMK_MACH_SPECIALIZED_QUEUE
    LrtsSpecializedQueuePush(rank, msg);
#elif CMK_SMP_MULTIQ
    CMIQueuePush(cs->recv[CmiGetState()->myGrpIdx], (char *)msg);
#else
    CMIQueuePush(cs->recv,(char*)msg);
#endif

#if CMK_SHARED_VARS_POSIX_THREADS_SMP
  if (_Cmi_sleepOnIdle)
#endif
    CmiIdleLock_addMessage(&cs->idle);
    MACHSTATE1(3,"} Pushing message into rank %d's queue done",rank);
}

#if CMK_OMP
void CmiSuspendedTaskEnqueue(int targetRank, void *msg) {
  CMIQueuePush((PCQueue)CpvAccessOther(CmiSuspendedTaskQueue, targetRank), (char *)msg);
}

void* CmiSuspendedTaskPop() {
  return CMIQueuePop((CMIQueue)CpvAccess(CmiSuspendedTaskQueue));
}
#endif

#if CMK_NODE_QUEUE_AVAILABLE
/*Add a message to this processor's receive queue */
void CmiPushNode(void *msg) {
    MACHSTATE(3,"Pushing message into NodeRecv queue");
#if CMK_IMMEDIATE_MSG
    if (CmiIsImmediate(msg)) {
        CMI_DEST_RANK(msg) = 0;
        CmiPushImmediateMsg(msg);
        return;
    }
#endif

#if CMK_MACH_SPECIALIZED_QUEUE
    LrtsSpecializedNodeQueuePush((char *)msg);
#elif CMK_LOCKLESS_QUEUE
    MPMCQueuePush(CsvAccess(NodeState).NodeRecv,msg);
#else
    CmiLock(CsvAccess(NodeState).CmiNodeRecvLock);
    CMIQueuePush(CsvAccess(NodeState).NodeRecv, (char *)msg);
    CmiUnlock(CsvAccess(NodeState).CmiNodeRecvLock);
#endif

#if CMK_SHARED_VARS_POSIX_THREADS_SMP
    if (_Cmi_sleepOnIdle)
#endif
    {
        CmiState cs=CmiGetStateN(0);
        CmiIdleLock_addMessage(&cs->idle);
    }
}
#endif

/* This function handles the msg received as which queue to push into */
static INLINE_KEYWORD void handleOneRecvedMsg(size_t size, char *msg) {

#if CMK_SMP_TRACE_COMMTHREAD
    TRACE_COMM_CREATION(TraceTimerCommon(), msg);
#endif

#if CMK_USE_CMA
    // If CMA message, perform CMA read to get the payload message
    if(cma_reg_msg && CMI_CMA_MSGTYPE(msg) == CMK_CMA_MD_MSG) {
      handleOneCmaMdMsg(&size, &msg);  // size & msg are modififed
    } else if(cma_reg_msg && CMI_CMA_MSGTYPE(msg) == CMK_CMA_ACK_MSG) {
      handleOneCmaAckMsg(size, msg);
      return;
    }
#endif

    int isBcastMsg = 0;
#if CMK_BROADCAST_SPANNING_TREE || CMK_BROADCAST_HYPERCUBE
    isBcastMsg = (CMI_BROADCAST_ROOT(msg)!=0);
#endif

    if (isBcastMsg) {
        handleOneBcastMsg(size, msg);
        return;
    }

#if CMK_NODE_QUEUE_AVAILABLE
    if (CMI_DEST_RANK(msg)==DGRAM_NODEMESSAGE){
        CmiPushNode(msg);
        return;
    }
#endif
    CmiPushPE(CMI_DEST_RANK(msg), msg);

}


static void SendToPeers(size_t size, char *msg) {
  /* FIXME: now it's just a flat p2p send!! When node size is large,
  * it should also be sent in a tree
  */

  int exceptRank = CMI_DEST_RANK(msg);
  if (CMI_MSG_NOKEEP(msg)) {
    for (int i = 0; i < exceptRank; i++) {
      CmiReference(msg);
      CmiPushPE(i, msg);
    }
    for (int i = exceptRank + 1; i < CmiMyNodeSize(); i++) {
      CmiReference(msg);
      CmiPushPE(i, msg);
    }
  } else {
    for (int i = 0; i < exceptRank; i++) {
      CmiPushPE(i, CopyMsg(msg, size));
    }
    for (int i = exceptRank + 1; i < CmiMyNodeSize(); i++) {
      CmiPushPE(i, CopyMsg(msg, size));
    }
  }
}


/* Functions regarding sending operations */
static void CmiSendSelf(char *msg) {
#if CMK_IMMEDIATE_MSG
    if (CmiIsImmediate(msg)) {
        /* CmiBecomeNonImmediate(msg); */
        CmiPushImmediateMsg(msg);
        CmiHandleImmediate();
        return;
    }
#endif
    CdsFifo_Enqueue(CpvAccess(CmiLocalQueue),msg);
}

/* Functions regarding P2P send op */
#if USE_COMMON_SYNC_P2P
void CmiSyncSendFn(int destPE, size_t size, char *msg) {
    if (CMI_MSG_NOKEEP(msg)) {
        CmiReference(msg);
        CmiFreeSendFn(destPE, size, msg);
    } else {
        char *dupmsg = CopyMsg(msg, size);
        CmiFreeSendFn(destPE, size, dupmsg);
    }
}
//inter-partition send
void CmiInterSyncSendFn(int destPE, int partition, size_t size, char *msg) {
    if (CMI_MSG_NOKEEP(msg)) {
        CmiReference(msg);
        CmiInterFreeSendFn(destPE, partition, size, msg);
    } else {
        char *dupmsg = CopyMsg(msg, size);
        CmiInterFreeSendFn(destPE, partition, size, dupmsg);
    }
}

#if CMK_USE_PXSHM
#include "machine-pxshm.C"
#endif
#if CMK_USE_XPMEM
#include "machine-xpmem.C"
#endif

static int refcount = 0;

#if CMK_USE_OOB
CpvExtern(int, _urgentSend);
#endif

//I am changing this function to offload task to a generic function - the one
//that handles sending to any partition
INLINE_KEYWORD CmiCommHandle CmiSendNetworkFunc(int destPE, size_t size, char *msg, int mode) {
  // Set the message as a regular message (defined in lrts-common.h)
  CMI_CMA_MSGTYPE(msg) = CMK_REG_NO_CMA_MSG;
  return CmiInterSendNetworkFunc(destPE, CmiMyPartition(), size, msg, mode);
}
//the generic function that replaces the older one
CmiCommHandle CmiInterSendNetworkFunc(int destPE, int partition, size_t size, char *msg, int mode)
{
        int rank;
        int destLocalNode = CmiNodeOf(destPE); 
        int destNode = CmiGetNodeGlobal(destLocalNode,partition); 

#if CMK_USE_CMA
        if(cma_reg_msg && partition == CmiMyPartition() && CmiPeOnSamePhysicalNode(CmiMyPe(), destPE)) {
          if(CMI_CMA_MSGTYPE(msg) == CMK_REG_NO_CMA_MSG && cma_min_threshold <= size && size <= cma_max_threshold) {
            CmiSendMessageCma(&msg, &size); // size & msg are modififed
          }
        }
#endif

#if CMK_USE_PXSHM      
        if ((partition == CmiMyPartition()) && CmiValidPxshm(destLocalNode, size)) {
          CmiSendMessagePxshm(msg, size, destLocalNode, &refcount);
          //for (int i=0; i<refcount; i++) CmiReference(msg);
          return 0;
        }
#endif
#if CMK_USE_XPMEM     
        if ((partition == CmiMyPartition()) && CmiValidXpmem(destLocalNode, size)) {
          CmiSendMessageXpmem(msg, size, destLocalNode, &refcount);
          //for (int i=0; i<refcount; i++) CmiReference(msg);
          return 0;
        }
#endif
#if CMK_PERSISTENT_COMM
        if (CpvAccess(phs)) {
          if (size > PERSIST_MIN_SIZE) {
            CmiAssert(CpvAccess(curphs) < CpvAccess(phsSize));
            PersistentSendsTable *slot = (PersistentSendsTable *)CpvAccess(phs)[CpvAccess(curphs)];
            CmiAssert(CmiNodeOf(slot->destPE) == destLocalNode);
            LrtsSendPersistentMsg(CpvAccess(phs)[CpvAccess(curphs)], destNode, size, msg);
            return 0;
          }
        }
#endif

#if CMK_WITH_STATS
if (MSG_STATISTIC)
{
    int ret_log = _cmi_log2(size);
    if(ret_log >21) ret_log = 21;
    msg_histogram[ret_log]++;
}
#endif
#if CMK_USE_OOB
    if (CpvAccess(_urgentSend)) mode |= OUT_OF_BAND;
#endif
    return LrtsSendFunc(destNode, CmiGetPeGlobal(destPE,partition), size, msg, mode);
}

//I am changing this function to offload task to a generic function - the one
//that handles sending to any partition
void CmiFreeSendFn(int destPE, size_t size, char *msg) {
    CmiInterFreeSendFn(destPE, CmiMyPartition(), size, msg);
}
//and the generic implementation - I may be in danger of making the frequent
//case slower - two extra comparisons may happen
void CmiInterFreeSendFn(int destPE, int partition, size_t size, char *msg) {
    CMI_SET_BROADCAST_ROOT(msg, 0);

    // Set the message as a regular message (defined in lrts-common.h)
    CMI_CMA_MSGTYPE(msg) = CMK_REG_NO_CMA_MSG;
#if CMI_QD
    CQdCreate(CpvAccess(cQdState), 1);
#endif
    if (CmiMyPe()==destPE && partition == CmiMyPartition()) {
        CmiSendSelf(msg);
#if CMK_PERSISTENT_COMM
        if (CpvAccess(phs)) CpvAccess(curphs)++;
#endif
    } 
    else {
        int destNode = CmiNodeOf(destPE);
        int destRank = CmiRankOf(destPE);
#if CMK_SMP
        if (CmiMyNode()==destNode && partition == CmiMyPartition()) {
            CmiPushPE(destRank, msg);
#if CMK_PERSISTENT_COMM
            if (CpvAccess(phs)) CpvAccess(curphs)++;
#endif
            return;
        }
#endif
        CMI_DEST_RANK(msg) = destRank;
        CmiInterSendNetworkFunc(destPE, partition, size, msg, P2P_SYNC);

#if CMK_PERSISTENT_COMM
        if (CpvAccess(phs)) CpvAccess(curphs)++;
#endif
    }
}
#endif

#if USE_COMMON_ASYNC_P2P
//not implementing it for partition
CmiCommHandle CmiAsyncSendFn(int destPE, size_t size, char *msg) {
    int destNode = CmiNodeOf(destPE);
    if (destNode == CmiMyNode()) {
        CmiSyncSendFn(destPE,size,msg);
        return 0;
    } else {
#if CMK_WITH_STATS
if (  MSG_STATISTIC)
{
    int ret_log = _cmi_log2(size);
        if(ret_log >21) ret_log = 21;
        msg_histogram[ret_log]++;
}
#endif
        return CmiSendNetworkFunc(destPE, size, msg, P2P_ASYNC);
    }
}
#endif

#if CMK_NODE_QUEUE_AVAILABLE
static void CmiSendNodeSelf(char *msg) {
#if CMK_IMMEDIATE_MSG
    if (CmiIsImmediate(msg)) {
        CmiPushImmediateMsg(msg);
        if (!_immRunning) CmiHandleImmediate();
        return;
    }
#endif
#if CMK_MACH_SPECIALIZED_QUEUE
    LrtsSpecializedNodeQueuePush(msg);
#elif CMK_LOCKLESS_QUEUE
    MPMCQueuePush(CsvAccess(NodeState).NodeRecv, msg);
#else
    CmiLock(CsvAccess(NodeState).CmiNodeRecvLock);
    CMIQueuePush(CsvAccess(NodeState).NodeRecv, msg);
    CmiUnlock(CsvAccess(NodeState).CmiNodeRecvLock);
#endif
}

//I think this #if is incorrect - should be SYNC_P2P
#if USE_COMMON_SYNC_P2P
void CmiSyncNodeSendFn(int destNode, size_t size, char *msg) {
    if (CMI_MSG_NOKEEP(msg)) {
        CmiReference(msg);
        CmiFreeNodeSendFn(destNode, size, msg);
    } else {
        char *dupmsg = CopyMsg(msg, size);
        CmiFreeNodeSendFn(destNode, size, dupmsg);
    }
}
//inter-partition send
void CmiInterSyncNodeSendFn(int destNode, int partition, size_t size, char *msg) {
    if (CMI_MSG_NOKEEP(msg)) {
        CmiReference(msg);
        CmiInterFreeNodeSendFn(destNode, partition, size, msg);
    } else {
        char *dupmsg = CopyMsg(msg, size);
        CmiInterFreeNodeSendFn(destNode, partition, size, dupmsg);
    }
}

//again, offloading the task to a generic function
void CmiFreeNodeSendFn(int destNode, size_t size, char *msg) {
  CmiInterFreeNodeSendFn(destNode, CmiMyPartition(), size, msg);
}
//and the inter-partition function
void CmiInterFreeNodeSendFn(int destNode, int partition, size_t size, char *msg) {
    CMI_DEST_RANK(msg) = DGRAM_NODEMESSAGE;
#if CMI_QD
    CQdCreate(CpvAccess(cQdState), 1);
#endif
    CMI_SET_BROADCAST_ROOT(msg, 0);
    // Set the message as a regular message (defined in lrts-common.h)
    CMI_CMA_MSGTYPE(msg) = CMK_REG_NO_CMA_MSG;
    if (destNode == CmiMyNode() && CmiMyPartition() == partition) {
        CmiSendNodeSelf(msg);
    } else {
#if CMK_WITH_STATS
if (  MSG_STATISTIC)
{
    int ret_log = _cmi_log2(size);
    if(ret_log >21) ret_log = 21;
    msg_histogram[ret_log]++;
}
#endif
        CmiInterSendNetworkFunc(CmiNodeFirst(destNode), partition, size, msg, P2P_SYNC);
    }
#if CMK_PERSISTENT_COMM
    if (CpvAccess(phs)) CpvAccess(curphs)++;
#endif
}
#endif

#if USE_COMMON_ASYNC_P2P
//not implementing it for partition
CmiCommHandle CmiAsyncNodeSendFn(int destNode, size_t size, char *msg) {
    if (destNode == CmiMyNode()) {
        CmiSyncNodeSendFn(destNode, size, msg);
        return 0;
    } else {
#if CMK_WITH_STATS
if (  MSG_STATISTIC)
{
        int ret_log = _cmi_log2(size);
        if(ret_log >21) ret_log = 21;
        msg_histogram[ret_log]++;
}
#endif
        return CmiSendNetworkFunc(CmiNodeFirst(destNode), size, msg, P2P_ASYNC);
    }
}
#endif
#endif

// functions related to partition
#if defined(_WIN32)
  /* strtok is thread safe in VC++ */
#define strtok_r(x,y,z) strtok(x,y)
#endif

#include "TopoManager.h"
extern "C" void createCustomPartitions(int numparts, int *partitionSize, int *nodeMap);
extern "C" void setDefaultPartitionParams(void);

void create_topoaware_partitions(void) {
  int i, j, numparts_bak;
  Partition_Type type_bak;
  int *testMap;

  _partitionInfo.nodeMap = (int*)malloc(CmiNumNodesGlobal()*sizeof(int));
  _MEMCHECK(_partitionInfo.nodeMap);

  type_bak = _partitionInfo.type;
  _partitionInfo.type = PARTITION_SINGLETON;

  numparts_bak = _partitionInfo.numPartitions;
  _partitionInfo.numPartitions = 1;

  _partitionInfo.myPartition = 0;

  _Cmi_numpes = _Cmi_numnodes * _Cmi_mynodesize;
  
  TopoManager_init();
  if(_partitionInfo.scheme == 100) {
    createCustomPartitions(numparts_bak, _partitionInfo.partitionSize, _partitionInfo.nodeMap);       
  } else {
    TopoManager_createPartitions(_partitionInfo.scheme, numparts_bak, _partitionInfo.nodeMap);
  }
  TopoManager_free();
  
  _partitionInfo.type = type_bak;
  _partitionInfo.numPartitions = numparts_bak;

#if CMK_ERROR_CHECKING
  testMap = (int*)calloc(CmiNumNodesGlobal(), sizeof(int));
  for(i = 0; i < CmiNumNodesGlobal(); i++) {
    assert(_partitionInfo.nodeMap[i] >= 0);
    assert(_partitionInfo.nodeMap[i] < CmiNumNodesGlobal());
    assert(testMap[_partitionInfo.nodeMap[i]] == 0);
    testMap[_partitionInfo.nodeMap[i]] = 1;
  }
  free(testMap);
#endif

  for(i = 0; i < _partitionInfo.numPartitions; i++) {
    int endnode = _partitionInfo.partitionPrefix[i] + _partitionInfo.partitionSize[i];
    for(j = _partitionInfo.partitionPrefix[i]; j < endnode; j++) {
      if(_partitionInfo.nodeMap[j] == CmiMyNodeGlobal()) {
        _Cmi_mynode = j - _partitionInfo.partitionPrefix[i];
        _partitionInfo.myPartition = i;
      }
    }
  }
}

void CmiSetNumPartitions(int nump) {
  _partitionInfo.numPartitions = nump;
}

void CmiSetMasterPartition(void) {
  if(!CmiMyNodeGlobal() && _partitionInfo.type != PARTITION_DEFAULT) {
    CmiAbort("setMasterPartition used with incompatible option\n");
  }
  _partitionInfo.type = PARTITION_MASTER;
} 

void CmiSetPartitionSizes(char *sizes) {
  int length = strlen(sizes);
  _partitionInfo.partsizes = (char*)malloc((length+1)*sizeof(char));

  if(!CmiMyNodeGlobal() && _partitionInfo.type != PARTITION_DEFAULT) {
    CmiAbort("setPartitionSizes used with incompatible option\n");
  }

  memcpy(_partitionInfo.partsizes, sizes, length*sizeof(char));
  _partitionInfo.partsizes[length] = '\0';
  _partitionInfo.type = PARTITION_PREFIX;
}

void CmiSetPartitionScheme(int scheme) {
  _partitionInfo.scheme = scheme;
  _partitionInfo.isTopoaware = 1;
}

void CmiSetCustomPartitioning(void) {
  _partitionInfo.scheme = 100;
  _partitionInfo.isTopoaware = 1;
}

static void create_partition_map( char **argv)
{
  char* token, *tptr;
  int i, flag;
  
  _partitionInfo.numPartitions = 1; 
  _partitionInfo.type = PARTITION_DEFAULT;
  _partitionInfo.partsizes = NULL;
  _partitionInfo.scheme = 0;
  _partitionInfo.isTopoaware = 0;

  setDefaultPartitionParams();

  if(!CmiGetArgIntDesc(argv,"+partitions", &_partitionInfo.numPartitions,"number of partitions")) {
    CmiGetArgIntDesc(argv,"+replicas", &_partitionInfo.numPartitions,"number of partitions");
  }

#if CMK_MULTICORE
  if(_partitionInfo.numPartitions != 1) {
    CmiAbort("+partitions other than 1 is not allowed for multicore build\n");
  }
#endif

  _partitionInfo.partitionSize = (int*)calloc(_partitionInfo.numPartitions,sizeof(int));
  _partitionInfo.partitionPrefix = (int*)calloc(_partitionInfo.numPartitions,sizeof(int));
  
  if (CmiGetArgFlagDesc(argv,"+master_partition","assign a process as master partition")) {
    _partitionInfo.type = PARTITION_MASTER;
  }
 
  if (CmiGetArgStringDesc(argv, "+partition_sizes", &_partitionInfo.partsizes, "size of partitions")) {
    if(!CmiMyNodeGlobal() && _partitionInfo.type != PARTITION_DEFAULT) {
      CmiAbort("+partition_sizes used with incompatible option, possibly +master_partition\n");
    }
    _partitionInfo.type = PARTITION_PREFIX;
  }

  if (CmiGetArgFlagDesc(argv,"+partition_topology","topology aware partitions")) {
    _partitionInfo.isTopoaware = 1;
    _partitionInfo.scheme = 1;
  }  

  if (CmiGetArgIntDesc(argv,"+partition_topology_scheme", &_partitionInfo.scheme, "topology aware partitioning scheme")) {
    _partitionInfo.isTopoaware = 1;
  }

  if (CmiGetArgFlagDesc(argv,"+use_custom_partition", "custom partitioning scheme")) {
    _partitionInfo.scheme = 100;
    _partitionInfo.isTopoaware = 1;
  }

  if(_partitionInfo.type == PARTITION_DEFAULT) {
    if((_Cmi_numnodes_global % _partitionInfo.numPartitions) != 0) {
      CmiAbort("Number of partitions does not evenly divide number of processes. Aborting\n");
    }
    _partitionInfo.partitionPrefix[0] = 0;
    _partitionInfo.partitionSize[0] = _Cmi_numnodes_global / _partitionInfo.numPartitions;
    for(i = 1; i < _partitionInfo.numPartitions; i++) {
      _partitionInfo.partitionSize[i] = _partitionInfo.partitionSize[i-1];
      _partitionInfo.partitionPrefix[i] = _partitionInfo.partitionPrefix[i-1] + _partitionInfo.partitionSize[i-1];
    } 
    _partitionInfo.myPartition = _Cmi_mynode_global / _partitionInfo.partitionSize[0];
  } else if(_partitionInfo.type == PARTITION_MASTER) {
    if(((_Cmi_numnodes_global-1) % (_partitionInfo.numPartitions-1)) != 0) {
      CmiAbort("Number of non-master partitions does not evenly divide number of processes minus one. Aborting\n");
    }
    _partitionInfo.partitionSize[0] = 1;
    _partitionInfo.partitionPrefix[0] = 0;
    _partitionInfo.partitionSize[1] = (_Cmi_numnodes_global-1) / (_partitionInfo.numPartitions-1);
    _partitionInfo.partitionPrefix[1] = 1;
    for(i = 2; i < _partitionInfo.numPartitions; i++) {
      _partitionInfo.partitionSize[i] = _partitionInfo.partitionSize[i-1];
      _partitionInfo.partitionPrefix[i] = _partitionInfo.partitionPrefix[i-1] + _partitionInfo.partitionSize[i-1];
    } 
    _partitionInfo.myPartition = 1 + (_Cmi_mynode_global-1) / _partitionInfo.partitionSize[1];
    if(!_Cmi_mynode_global) 
      _partitionInfo.myPartition = 0;
  } else if(_partitionInfo.type == PARTITION_PREFIX) {
    token = strtok_r(_partitionInfo.partsizes, ",", &tptr);
    while (token)
    {
      int i,j;
      int hasdash=0, hascolon=0, hasdot=0;
      int start, end, stride = 1, block = 1, size;
      for (i = 0; i < strlen(token); i++) {
        if (token[i] == '-') hasdash=1;
        else if (token[i] == ':') hascolon=1;
        else if (token[i] == '.') hasdot=1;
      }
      if (hasdash) {
        if (hascolon) {
          if (hasdot) {
            if (sscanf(token, "%d-%d:%d.%d#%d", &start, &end, &stride, &block, &size) != 5)
              printf("Warning: Check the format of \"%s\".\n", token);
          }
          else {
            if (sscanf(token, "%d-%d:%d#%d", &start, &end, &stride, &size) != 4)
              printf("Warning: Check the format of \"%s\".\n", token);
          }
        }
        else {
          if (sscanf(token, "%d-%d#%d", &start, &end, &size) != 3)
            printf("Warning: Check the format of \"%s\".\n", token);
        }
      }
      else {
        if (sscanf(token, "%d#%d", &start, &size) != 2) {
          printf("Warning: Check the format of \"%s\".\n", token);
        }
        end = start;
      }
      if (block > stride) {
        printf("Warning: invalid block size in \"%s\" ignored.\n", token);
        block = 1;
      }
      for (i = start; i <= end; i += stride) {
        for (j = 0; j < block; j++) {
          if (i + j > end) break;
          _partitionInfo.partitionSize[i+j] = size;
        }
      }
      token = strtok_r(NULL, ",", &tptr);
    }
    _partitionInfo.partitionPrefix[0] = 0;
    _partitionInfo.myPartition = 0;
    for(i = 1; i < _partitionInfo.numPartitions; i++) {
      if(_partitionInfo.partitionSize[i-1] <= 0) {
        CmiAbort("Partition size has to be greater than zero.\n");
      }
      _partitionInfo.partitionPrefix[i] = _partitionInfo.partitionPrefix[i-1] + _partitionInfo.partitionSize[i-1];
      if((_Cmi_mynode_global >= _partitionInfo.partitionPrefix[i]) && (_Cmi_mynode_global < (_partitionInfo.partitionPrefix[i] + _partitionInfo.partitionSize[i]))) {
        _partitionInfo.myPartition = i;
      }
    } 
    if(_partitionInfo.partitionSize[i-1] <= 0) {
      CmiAbort("Partition size has to be greater than zero.\n");
    }
  }
  _Cmi_mynode = _Cmi_mynode - _partitionInfo.partitionPrefix[_partitionInfo.myPartition];

  if(_partitionInfo.isTopoaware) {
    create_topoaware_partitions();
  }
}

void CmiCreatePartitions(char **argv) {
  _Cmi_numnodes_global = _Cmi_numnodes;
  _Cmi_mynode_global = _Cmi_mynode;
  _Cmi_numpes_global = _Cmi_numnodes_global * _Cmi_mynodesize;

  if(Cmi_nodestart != -1) {
    Cmi_nodestartGlobal = Cmi_nodestart;
  } else {
    Cmi_nodestartGlobal =  _Cmi_mynode_global * _Cmi_mynodesize;
  }

  //creates partitions, reset _Cmi_mynode to be the new local rank
  if(_partitionInfo.numPartitions > _Cmi_numnodes_global) {
    CmiAbort("Number of partitions requested is greater than the number of nodes\n");
  }
  create_partition_map(argv);
  
  //reset other local variables
  _Cmi_numnodes = CmiMyPartitionSize();
  //mype and numpes will be set following this
}

int node_lToGTranslate(int node, int partition) {
  int rank;
  if(_partitionInfo.type == PARTITION_SINGLETON) { 
    return node;
  } else if(_partitionInfo.type == PARTITION_DEFAULT) { 
    rank =  (partition * _partitionInfo.partitionSize[0]) + node;
  } else if(_partitionInfo.type == PARTITION_MASTER) {
    if(partition == 0) {
      CmiAssert(node == 0);
      rank =  0;
    } else {
      rank = 1 + ((partition - 1) * _partitionInfo.partitionSize[1]) + node;
    }
  } else if(_partitionInfo.type == PARTITION_PREFIX) {
    rank = _partitionInfo.partitionPrefix[partition] + node;
  } else {
    CmiAbort("Partition type did not match any of the supported types\n");
  }
  if(_partitionInfo.isTopoaware) {
    return _partitionInfo.nodeMap[rank];
  } else {
    return rank;
  }
}

int pe_lToGTranslate(int pe, int partition) {
  if(_partitionInfo.type == PARTITION_SINGLETON) 
    return pe;

  if(pe < CmiPartitionSize(partition)*CmiMyNodeSize()) {
    return node_lToGTranslate(CmiNodeOf(pe),partition)*CmiMyNodeSize() + CmiRankOf(pe);
  }

  return CmiNumPesGlobal() + node_lToGTranslate(pe - CmiPartitionSize(partition)*CmiMyNodeSize(), partition);
}

INLINE_KEYWORD int node_gToLTranslate(int node) {
  CmiAbort("Conversion from global rank to local rank is not supported. Please contact Charm++ developers with the use case.\n");
  return -1;
}

INLINE_KEYWORD int pe_gToLTranslate(int pe) {
  CmiAbort("Conversion from global rank to local rank is not supported. Please contact Charm++ developers with the use case.\n");
  return -1;
}
//end of functions related to partition

extern int quietMode;
extern int quietModeRequested;

#if defined(_WIN32)
#include <windows.h> /* for SetEnvironmentVariable() and routines for CMK_SHARED_VARS_NT_THREADS */
#define SET_ENV_VAR(key, value) SetEnvironmentVariable(key, value)
#else
#define SET_ENV_VAR(key, value) setenv(key, value, 0)
#endif
// For INT_MAX
#include <limits.h>

#if CMK_LOCKLESS_QUEUE
#define DefaultDataNodeSize 2048
#define DefaultMaxDataNodes 2048
extern int DataNodeSize;
extern int MaxDataNodes;
extern int QueueUpperBound;
extern int DataNodeWrap;
extern int QueueWrap;
extern CmiMemoryAtomicInt messageQueueOverflow;

int power_of_two_check(int n)
{
  return (n > 0 && (n & (n - 1)) == 0);
}


void check_and_set_queue_parameters()
{
  if(DataNodeSize <= 0 || MaxDataNodes <= 0 || !power_of_two_check(DataNodeSize) || !power_of_two_check(MaxDataNodes))
  {
    CmiPrintf("MessageQueues: MessageQueueNodeSize: %d,  Check that this value is > 0 and a power of 2.\n", DataNodeSize);
    CmiPrintf("MessageQueues: MessageQueueNodes: %d, Check that this value is > 0 and a power of 2.\n", MaxDataNodes);
    CmiAbort("Invalid MessageQueue Parameters");
  }
  QueueUpperBound = DataNodeSize * MaxDataNodes;
  DataNodeWrap = DataNodeSize - 1;
  QueueWrap = QueueUpperBound - 1;
}
#endif

/* ##### Beginning of Functions Related with Machine Startup ##### */
void ConverseInit(int argc, char **argv, CmiStartFn fn, int usched, int initret) {
    int _ii;
    int tmp;
    //handle output to files for partition if requested
    char *stdoutbase,*stdoutpath;


#if CMK_WITH_STATS
    MSG_STATISTIC = CmiGetArgFlag(argv, "+msgstatistic");
#endif

  if (CmiGetArgFlagDesc(argv,"++quiet","Omit non-error runtime messages")) {
    quietModeRequested = quietMode = 1;
  }

    CmiInitHwlocTopology();

    /* processor per node */
    _Cmi_mynodesize = 1;

    int ppnSet = 0;
    if (!(ppnSet = CmiGetArgInt(argv,"+ppn", &_Cmi_mynodesize)))
        ppnSet = CmiGetArgInt(argv,"++ppn", &_Cmi_mynodesize);

    int npes = 1;
    int plusPSet = CmiGetArgInt(argv,"+p",&npes);

    int auto_provision = CmiGetArgFlagDesc(argv, "+auto-provision", "fully utilize available resources");
    auto_provision |= CmiGetArgFlagDesc(argv, "+autoProvision", "fully utilize available resources");
    int onewth_per_host = CmiGetArgFlagDesc(argv, "+oneWthPerHost", "assign one worker thread per host");
    int onewth_per_socket = CmiGetArgFlagDesc(argv, "+oneWthPerSocket", "assign one worker thread per socket");
    int onewth_per_core = CmiGetArgFlagDesc(argv, "+oneWthPerCore", "assign one worker thread per core");
    int onewth_per_pu = CmiGetArgFlagDesc(argv, "+oneWthPerPU", "assign one worker thread per PU");
    int onewth_active = (onewth_per_host > 0) + (onewth_per_socket > 0) + (onewth_per_core > 0) + (onewth_per_pu > 0);
    if (onewth_active > 1 || onewth_active + (plusPSet || ppnSet) + auto_provision > 1)
    {
      CmiError("Error: Only one of +auto-provision, +oneWthPer(Host|Socket|Core|PU), or +p/++ppn is allowed.\n");
      exit(1);
    }

#if ! CMK_SMP
    if (plusPSet && npes != 1)
    {
      fprintf(stderr,
        "To use multiple processors, you must run this program as:\n"
        " > charmrun +p%d %s <args>\n"
        "or build the %s-smp version of Charm++.\n",
        npes,argv[0],CMK_MACHINE_NAME);
      exit(1);
    }

    if ((_Cmi_mynodesize > 1 && _Cmi_mynode == 0) || onewth_per_socket || onewth_per_core || onewth_per_pu)
    {
      CmiError("Error: +oneWthPer(Socket|Core|PU) and +ppn can only be used in SMP mode.\n");
      exit(1);
    }
#else
    if (onewth_active || auto_provision)
    {
      SET_ENV_VAR("CmiProcessPerHost", "1");

      int ppn;
      if (onewth_per_host)
      {
        ppn = 1;
        SET_ENV_VAR("CmiOneWthPerHost", "1");
      }
      else if (onewth_per_socket)
      {
        ppn = CmiHwlocTopologyLocal.num_sockets;
        SET_ENV_VAR("CmiOneWthPerSocket", "1");
      }
      else if (onewth_per_core)
      {
        ppn = CmiHwlocTopologyLocal.num_cores;
        SET_ENV_VAR("CmiOneWthPerCore", "1");
      }
      else // if (onewth_per_pu || auto_provision)
      {
        ppn = CmiHwlocTopologyLocal.num_pus;
        SET_ENV_VAR("CmiOneWthPerPU", "1");
      }
# if !CMK_MULTICORE
      // account for comm thread, if necessary to avoid oversubscribing PUs
      if (ppn + 1 > CmiHwlocTopologyLocal.num_pus)
        --ppn;

      if (ppn == 0)
        ppn = 1;
# endif

      if (ppn <= 0)
      {
        CmiError("Error: Invalid request for %d PEs\n", ppn);
        exit(1);
      }

      _Cmi_mynodesize = ppn;
    }
    else if (plusPSet)
    {
      if (ppnSet && _Cmi_mynodesize != npes)
      {
        // if you want to use redundant arguments they need to be consistent
        CmiError("Error: p != ppn, must not have inconsistent values. (%d != %d)\n"
          "Standalone invocation should use only one of [+p, +ppn, ++ppn].\n", npes, _Cmi_mynodesize);
        exit(1);
      }
      else
      { // +ppn wasn't set, make it the same as +p
        _Cmi_mynodesize = npes;
      }
    }
# if CMK_NET_VERSION || CMK_IBVERBS || CMK_MULTICORE
#  if CMK_MULTICORE
    else if (!ppnSet)
#  else
    else if (!ppnSet && Cmi_charmrun_fd == -1)
#  endif
    {
      if (!quietMode)
      {
        printf("Charm++> No provisioning arguments specified. Running with a single PE.\n"
               "         Use +auto-provision to fully subscribe resources or +p1 to silence this message.\n");
      }
    }
# endif
#endif

    /* Network progress function is used to poll the network when for
    messages. This flushes receive buffers on some  implementations*/
    networkProgressPeriod = NETWORK_PROGRESS_PERIOD_DEFAULT;
    CmiGetArgInt(argv, "+networkProgressPeriod", &networkProgressPeriod);

    /* _Cmi_mynodesize has to be obtained before LrtsInit
     * because it may be used inside LrtsInit
     */
    /* argv could be changed inside LrtsInit */
    /* Inside this function, the number of nodes and my node id are obtained */
#if CMK_WITH_STATS
if (  MSG_STATISTIC)
{
    for(_ii=0; _ii<22; _ii++)
        msg_histogram[_ii] = 0;
}
#endif
#if CMK_LOCKLESS_QUEUE
    /* Lockfree queue initialization */
    if (!CmiGetArgIntDesc(argv,"+MessageQueueNodes",&MaxDataNodes, "The size of the message queue static arrays")) {
      MaxDataNodes = DefaultMaxDataNodes;
    }
    if (!CmiGetArgIntDesc(argv,"+MessageQueueNodeSize",&DataNodeSize, "The size of the message queue data nodes")) {
      DataNodeSize = DefaultDataNodeSize;
    }
    check_and_set_queue_parameters();
#endif

    LrtsInit(&argc, &argv, &_Cmi_numnodes, &_Cmi_mynode);

#if MACHINE_DEBUG_LOG
    char ln[200];
    sprintf(ln,"debugLog.%d", _Cmi_mynode);
    debugLog=fopen(ln,"w");
    if (debugLog == NULL)
    {
        CmiAbort("debug file not open\n");
    }
#endif


    if (_Cmi_mynode==0) {
#if !CMK_SMP 
      if (!quietMode) printf("Charm++> Running in non-SMP mode: %d processes (PEs)\n", _Cmi_numnodes);
      MACHSTATE1(4,"running nonsmp %d", _Cmi_mynode)
#else

#if !CMK_MULTICORE
      if (!quietMode) {
        // NOTE: calling CmiNumPes() here it sometimes returns zero
        int commThdsPerProcess = (Cmi_smp_mode_setting != COMM_THREAD_NOT_EXIST);
        int totalPes = CmiNumNodes() * _Cmi_mynodesize;
        printf("Charm++> Running in SMP mode: %d processes, %d worker threads (PEs) + %d comm threads per process, %d PEs total\n",
               CmiNumNodes(), _Cmi_mynodesize, commThdsPerProcess, totalPes);
      }
      if (Cmi_smp_mode_setting == COMM_THREAD_SEND_RECV) {
        if (!quietMode) printf("Charm++> The comm. thread both sends and receives messages\n");
      } else if (Cmi_smp_mode_setting == COMM_THREAD_ONLY_RECV) {
        if (!quietMode) printf("Charm++> The comm. thread only receives messages, while work threads send messages\n");
      } else if (Cmi_smp_mode_setting == COMM_WORK_THREADS_SEND_RECV) {
        if (!quietMode) printf("Charm++> Both  comm. thread and worker thread send and messages\n");
      } else if (Cmi_smp_mode_setting == COMM_THREAD_NOT_EXIST) {
        if (!quietMode) printf("Charm++> There's no comm. thread. Work threads both send and receive messages\n");
      } else {
        CmiAbort("Charm++> Invalid SMP mode setting\n");
      }
#else
      if (!quietMode) printf("Charm++> Running in Multicore mode: %d threads (PEs)\n", _Cmi_mynodesize);
#endif
      
#endif
    }

    CmiCreatePartitions(argv);

    _Cmi_numpes = _Cmi_numnodes * _Cmi_mynodesize;
    Cmi_nodestart = _Cmi_mynode * _Cmi_mynodesize;
    Cmi_argvcopy = CmiCopyArgs(argv);
    Cmi_argv = argv;
    Cmi_startfn = fn;
    Cmi_usrsched = usched;

    if ( CmiGetArgStringDesc(argv,"+stdout",&stdoutbase,"base filename to redirect partition stdout to") ) {
      stdoutpath = (char *)malloc(strlen(stdoutbase) + 30);
      sprintf(stdoutpath, stdoutbase, CmiMyPartition(), CmiMyPartition(), CmiMyPartition());
      if ( ! strcmp(stdoutpath, stdoutbase) ) {
        sprintf(stdoutpath, "%s.%d", stdoutbase, CmiMyPartition());
      }
      if ( CmiMyPartition() == 0 && CmiMyNode() == 0 && !quietMode) {
        printf("Redirecting stdout to files %s through %d\n",stdoutpath,CmiNumPartitions()-1);
      }
      if ( ! freopen(stdoutpath, "a", stdout) ) {
        fprintf(stderr,"Rank %d failed redirecting stdout to file %s: %s\n", CmiMyNodeGlobal(), stdoutpath,
            strerror(errno));
        CmiAbort("Error redirecting stdout to file.");
      }
      _writeToStdout = 0;
      free(stdoutpath);
    }
#if CMK_SMP
    comm_mutex=CmiCreateLock();
#endif

#if CMK_USE_PXSHM
    CmiInitPxshm(argv);
#endif
#if CMK_USE_XPMEM
    CmiInitXpmem(argv);
#endif
#if CMK_USE_CMA
    cma_works = 1;
    if (CmiGetArgFlagDesc(argv,"+cma_enable_all","Ignore default thresholds & Enable use of CMA for all intranode SHM transport")) {
      cma_min_threshold = 0U;
      cma_max_threshold = INT_MAX;
    } else {
      // CMA Message size lower bound
      if(!CmiGetArgIntDesc(argv,"+cma_min_threshold", &cma_min_threshold,"CMA Message size (Bytes) lower bound")) {
        // Default cma_min_threshold
        cma_min_threshold = CMK_CMA_MIN;
      }
      // CMA Message size upper bound
      if(!CmiGetArgIntDesc(argv,"+cma_max_threshold", &cma_max_threshold,"CMA Message size (Bytes) upper bound")) {
        // Default cma_max_threshold
        cma_max_threshold = CMK_CMA_MAX;
      }
      // Disable CMA
      if (CmiGetArgFlagDesc(argv,"+cma_disable","Disable use of CMA for SHM transport")) {
        cma_works = 0;
        cma_reg_msg = false;
      }
    }

    if(cma_works) {
      // Initialize CMA if it is not disable to check if the OS supports it
      cma_works = CmiInitCma();
      /* To use CMA, check that the global variable cma_works flag is set to 1.
       * If it is 0, it indicates that CMA calls are supported but operations do
       * not have the right permissions for usage. If cma_works is 0, CMA cannot be
       * used by the RTS. CMA_HAS_CMA being disabled indicates that CMA calls are not
       * even supported by the OS.
       */

      // Enable/disable disable regular messaging
      // TODO: Unset this flag to get CMA working for regular messages
      cma_reg_msg = 0;

      // Display CMA thresholds for regular messages if it is enabled
      if(cma_reg_msg)
        CmiDisplayCMAThresholds(cma_min_threshold, cma_max_threshold);
    }
#endif

    if(CmiGetArgFlagDesc(argv, "+cpd", "Used *only* in conjunction with parallel debugger")) {
#if CMK_CCS_AVAILABLE
        cmiArgDebugFlag = 1;
#endif
    }

    cmiNcpyAckSize = 0;

    /* CmiTimerInit(); */
#if CMK_BROADCAST_HYPERCUBE
    /* CmiNodesDim = ceil(log2(CmiNumNodes)) except when #nodes is 1*/
    tmp = CmiNumNodes()-1;
    CmiNodesDim = 0;
    while (tmp>0) {
        CmiNodesDim++;
        tmp = tmp >> 1;
    }
    if (CmiNumNodes()==1) CmiNodesDim=1;
#endif

    CsvInitialize(CmiNodeState, NodeState);
    CmiNodeStateInit(&CsvAccess(NodeState));

#if CMK_OFFLOAD_BCAST_PROCESS
    /* the actual queues should be created on comm thread considering NUMA in SMP */
    CsvInitialize(CMIQueue, procBcastQ);
#if CMK_NODE_QUEUE_AVAILABLE
    CsvInitialize(CMIQueue, nodeBcastQ);
#endif
#endif

#if CMK_SMP && CMK_LEVERAGE_COMMTHREAD
    CsvInitialize(CMIQueue, notifyCommThdMsgBuffer);
#endif

    CmiStartThreads(argv);

    ConverseRunPE(initret);
}

void ConverseCommonInit(char **argv);
void CthInit(char **argv);
static void ConverseRunPE(int everReturn) {
    CmiState cs;
    char** CmiMyArgv;

    LrtsPreCommonInit(everReturn);

#if CMK_OFFLOAD_BCAST_PROCESS
    int createQueue = 1;
#if CMK_SMP
#if CMK_SMP_NO_COMMTHD
    /* If there's no comm thread, then the queue is created on rank 0 */
    if (CmiMyRank()) createQueue = 0;
#else
    if (CmiMyRank()<CmiMyNodeSize()) createQueue = 0;
#endif
#endif

    if (createQueue) {
        CsvAccess(procBcastQ) = CMIQueueCreate();
#if CMK_NODE_QUEUE_AVAILABLE
        CsvAccess(nodeBcastQ) = CMIQueueCreate();
#endif
    }
#endif

    CmiNodeAllBarrier();

    cs = CmiGetState();
    CpvInitialize(void *,CmiLocalQueue);
    CpvAccess(CmiLocalQueue) = cs->localqueue;

    if (CmiMyRank())
        CmiMyArgv=CmiCopyArgs(Cmi_argvcopy);
    else
        CmiMyArgv=Cmi_argv;

    CthInit(CmiMyArgv);
#if CMK_OMP
    CmiNodeAllBarrier();
#endif
    /* initialize the network progress counter*/
    /* Network progress function is used to poll the network when for
       messages. This flushes receive buffers on some  implementations*/
    CpvInitialize(unsigned , networkProgressCount);
    CpvAccess(networkProgressCount) = 0;

    ConverseCommonInit(CmiMyArgv);
   
    // register idle events

#if CMK_SMP
    {
      CmiIdleState *sidle=CmiNotifyGetState();
      CcdCallOnConditionKeep(CcdPROCESSOR_BEGIN_IDLE,(CcdVoidFn)CmiNotifyBeginIdle,(void *)sidle);
      CcdCallOnConditionKeep(CcdPROCESSOR_STILL_IDLE,(CcdVoidFn)CmiNotifyStillIdle,(void *)sidle);
    }
#else
    CcdCallOnConditionKeep(CcdPROCESSOR_BEGIN_IDLE,(CcdVoidFn)CmiNotifyBeginIdle, NULL);
    CcdCallOnConditionKeep(CcdPROCESSOR_STILL_IDLE,(CcdVoidFn)CmiNotifyStillIdle, NULL);
#endif


    LrtsPostCommonInit(everReturn);

#if CMK_SMP && CMK_LEVERAGE_COMMTHREAD
    CmiInitNotifyCommThdScheme();
#endif
    /* Converse initialization finishes, immediate messages can be processed.
       node barrier previously should take care of the node synchronization */
    _immediateReady = 1;

    /* communication thread */
    if (CmiMyRank() == CmiMyNodeSize()) {
      Cmi_startfn(CmiGetArgc(CmiMyArgv), CmiMyArgv);
      if(!CharmLibInterOperate) {
        while (1) CommunicationServerThread(5);
      }
    } else { /* worker thread */
      if (!everReturn) {
        Cmi_startfn(CmiGetArgc(CmiMyArgv), CmiMyArgv);
        if (Cmi_usrsched==0) CsdScheduler(-1);
        if(!CharmLibInterOperate) {
          ConverseExit();
        } 
      }
    }
}
/* ##### End of Functions Related with Machine Startup ##### */

/* ##### Beginning of Functions Related with Machine Running ##### */
static INLINE_KEYWORD void AdvanceCommunication(int whenidle) {
    int doProcessBcast = 1;

#if CMK_USE_PXSHM
    CommunicationServerPxshm();
#endif
#if CMK_USE_XPMEM
    CommunicationServerXpmem();
#endif

    LrtsAdvanceCommunication(whenidle);

#if CMK_OFFLOAD_BCAST_PROCESS
#if CMK_SMP_NO_COMMTHD
    /*FIXME: only asks rank 0 to process bcast msgs, so perf may suffer*/
    if (CmiMyRank()) doProcessBcast = 0;
#endif
    if (doProcessBcast) processBcastQs();
#endif

#if CMK_IMMEDIATE_MSG
#if !CMK_SMP
    CmiHandleImmediate();
#endif
#if CMK_SMP && CMK_SMP_NO_COMMTHD
    if (CmiMyRank()==0) CmiHandleImmediate();
#endif
#endif
}

void ConverseCommonExit(void);

static void CommunicationServer(int sleepTime) {
#if CMK_SMP 
    AdvanceCommunication(1);

    if (std::atomic_load_explicit(&numPEsReadyForExit, std::memory_order_acquire) == CmiMyNodeSize()) {
        MACHSTATE(2, "CommunicationServer exiting {");
        LrtsDrainResources();
        MACHSTATE(2, "} CommunicationServer EXIT");

        ConverseCommonExit();
  
#if CMK_USE_XPMEM
        CmiExitXpmem();
#endif
        LrtsExit(_exitcode);
        if(CharmLibInterOperate) {
          ckExitComplete = 1;
          CmiNodeAllBarrier();
        }
    }
#endif
}

void CommunicationServerThread(int sleepTime) {
    CommunicationServer(sleepTime);
#if CMK_IMMEDIATE_MSG
    CmiHandleImmediate();
#endif
}

void ConverseExit(int exitcode) {
    int i;
    if (quietModeRequested) quietMode = 1;
#if !CMK_SMP || CMK_SMP_NO_COMMTHD
    LrtsDrainResources();
#else
	if(Cmi_smp_mode_setting == COMM_THREAD_ONLY_RECV
	   || Cmi_smp_mode_setting == COMM_THREAD_NOT_EXIST)
		LrtsDrainResources();
#endif

    ConverseCommonExit();

#if CMK_WITH_STATS
if (MSG_STATISTIC)
{
    for(i=0; i<22; i++)
    {
        CmiPrintf("[MSG PE:%d]", CmiMyPe());
        if(msg_histogram[i] >0)
            CmiPrintf("(%d:%d) ", 1<<i, msg_histogram[i]);
    }
    CmiPrintf("\n");
}
#endif

#if (CMK_DEBUG_MODE || CMK_WEB_MODE || NODE_0_IS_CONVHOST)
    if (CmiMyPe() == 0) 
      CmiPrintf("[Partition %d][Node %d] End of program\n",CmiMyPartition(),CmiMyNode());
#endif

#if !CMK_SMP || CMK_BLUEGENEQ || CMK_PAMI_LINUX_PPC8
#if CMK_USE_PXSHM
    CmiExitPxshm();
#endif
#if CMK_USE_XPMEM
    CmiExitXpmem();
#endif
    LrtsExit(exitcode);
#else
    /* In SMP, the communication thread will exit */
    if (CmiMyRank() == 0)
        _exitcode = exitcode;
#if CMK_SMP_NO_COMMTHD && CMK_USE_XPMEM
    CmiNodeAllBarrier(); /* Ensure all threads have reached here before freeing xpmem buffers */
    if (CmiMyRank() == 0) CmiExitXpmem();
#endif
    std::atomic_fetch_add_explicit(&numPEsReadyForExit, 1, std::memory_order_release); /* atomic increment */
#if CMK_SMP_NO_COMMTHD
    if (CmiMyRank() == 0) {
      /* Spinning on this load is necessary to ensure all threads have fully exited the
         barrier, otherwise exit may destroy it before all threads have left */
      while (std::atomic_load_explicit(&numPEsReadyForExit, std::memory_order_acquire) != CmiMyNodeSize()) {}
      LrtsExit(exitcode);
    }
#endif
    CmiYield();
    if (!CharmLibInterOperate || userDrivenMode) {
      while (1) {
        CmiYield();
      }
    }
#endif
}
/* ##### End of Functions Related with Machine Running ##### */

CMK_NORETURN
void CmiAbortHelper(const char *source, const char *message, const char *suggestion,
                    int tellDebugger, int framesToSkip) {
  if (tellDebugger)
    CpdAborting(message);

  if (CmiNumPartitions() == 1) {
    CmiError("------------- Processor %d Exiting: %s ------------\n"
             "Reason: %s\n", CmiMyPe(), source, message);
  } else {
    CmiError("------- Partition %d Processor %d Exiting: %s ------\n"
             "Reason: %s\n", CmiMyPartition(), CmiMyPe(), source, message);
  }

  if (suggestion && suggestion[0])
    CmiError("Suggestion: %s\n", suggestion);

  CmiPrintStackTrace(framesToSkip);

#if CMK_USE_PXSHM
  CmiExitPxshm();
#endif
#if CMK_USE_XPMEM
  CmiExitXpmem();
#endif
  LrtsAbort(message);
}

void CmiAbort(const char *message, ...) {
  char newmsg[256];
  va_list args;
  va_start(args, message);
  vsnprintf(newmsg, sizeof(newmsg), message, args);
  va_end(args);
  CmiAbortHelper("Called CmiAbort", newmsg, NULL, 1, 0);
}

/* ##### Beginning of Functions Providing Incoming Network Messages ##### */
void *CmiGetNonLocal(void) {
    CmiState cs = CmiGetState();
    void *msg = NULL;
#if !CMK_SMP || CMK_SMP_NO_COMMTHD
    /**
      * In SMP mode with comm thread, it's possible a normal
      * msg is sent from an immediate msg which is executed
      * on comm thread. In this case, the msg is sent to
      * the network queue of the work thread. Therefore,
      * even there's only one worker thread, the polling of
      * network queue is still required.
      */
#if CMK_CCS_AVAILABLE
    if (CmiNumPes() == 1 && CmiNumPartitions() == 1 && ccsRunning != 1) return NULL;
#else
    if (CmiNumPes() == 1 && CmiNumPartitions() == 1) return NULL;
#endif
#endif

    MACHSTATE2(3, "[%p] CmiGetNonLocal begin %d{", cs, CmiMyPe());

#if CMK_MACH_SPECIALIZED_QUEUE
    msg = LrtsSpecializedQueuePop();
#else
    CmiIdleLock_checkMessage(&cs->idle);
    /* ?????although it seems that lock is not needed, I found it crashes very often
       on mpi-smp without lock */
    msg = CMIQueuePop(cs->recv);
#endif
#if (!CMK_SMP || CMK_SMP_NO_COMMTHD) && !CMK_MULTICORE
    if (!msg) {
       AdvanceCommunication(0);
#if CMK_MACH_SPECIALIZED_QUEUE
       msg = LrtsSpecializedQueuePop();
#else
       msg = CMIQueuePop(cs->recv);
#endif
    }
#else
//    LrtsPostNonLocal();
#endif
#if CMK_CCS_AVAILABLE
    if(msg != NULL && CmiNumPes() == 1 && CmiNumPartitions() == 1 )
    {
      ccsRunning = 0;
    }
#endif

    MACHSTATE3(3,"[%p] CmiGetNonLocal from queue %p with msg %p end }",CmiGetState(),(cs->recv), msg);

    return msg;
}

#if CMK_NODE_QUEUE_AVAILABLE
void *CmiGetNonLocalNodeQ(void) {
    char *result = 0;

#if CMK_MACH_SPECIALIZED_QUEUE && CMK_MACH_SPECIALIZED_MUTEX
    if (!LrtsSpecializedNodeQueueEmpty()) {
      if (LrtsSpecializedMutexTryAcquire() == 0) {
        result = LrtsSpecializedNodeQueuePop();
        LrtsSpecializedMutexRelease();
      }
    }
#elif CMK_MACH_SPECIALIZED_QUEUE
    if(!LrtsSpecializedNodeQueueEmpty()) {
      if(CmiTryLock(CsvAccess(NodeState).CmiNodeRecvLock) == 0) {
        result = LrtsSpecializedNodeQueuePop();
        CmiUnlock(CsvAccess(NodeState).CmiNodeRecvLock);
      }

    }
#else
    CmiState cs = CmiGetState();
    CmiIdleLock_checkMessage(&cs->idle);
#if CMK_LOCKLESS_QUEUE
    if (!MPMCQueueEmpty(CsvAccess(NodeState).NodeRecv)) {
#else
    if (!CMIQueueEmpty(CsvAccess(NodeState).NodeRecv)) {
#endif
        MACHSTATE1(3,"CmiGetNonLocalNodeQ begin %d {", CmiMyPe());
#if CMK_LOCKLESS_QUEUE
        result = (char *) MPMCQueuePop(CsvAccess(NodeState).NodeRecv);
#else
        CmiLock(CsvAccess(NodeState).CmiNodeRecvLock);
        result = (char *) CMIQueuePop(CsvAccess(NodeState).NodeRecv);
        CmiUnlock(CsvAccess(NodeState).CmiNodeRecvLock);
#endif
        MACHSTATE1(3,"} CmiGetNonLocalNodeQ end %d ", CmiMyPe());
    }
#endif
    return result;

}
#endif
/* ##### End of Functions Providing Incoming Network Messages ##### */

static CmiIdleState *CmiNotifyGetState(void) {
    CmiIdleState *s=(CmiIdleState *)malloc(sizeof(CmiIdleState));
    s->sleepMs=0;
    s->nIdles=0;
    s->cs=CmiGetState();
    return s;
}

static void CmiNotifyBeginIdle(CmiIdleState *s) {
    if(s!= NULL){
        s->sleepMs=0;
        s->nIdles=0;
    }
    LrtsBeginIdle();
}

/*Number of times to spin before sleeping*/
#define SPINS_BEFORE_SLEEP 20
static void CmiNotifyStillIdle(CmiIdleState *s) {
    MACHSTATE1(2,"still idle (%d) begin {",CmiMyPe())
#if (!CMK_SMP || CMK_SMP_NO_COMMTHD) && !CMK_MULTICORE
    AdvanceCommunication(1);
#else
    LrtsPostNonLocal();

#if CMK_SHARED_VARS_POSIX_THREADS_SMP
    if (_Cmi_sleepOnIdle)
#endif
    {
    s->nIdles++;
    if (s->nIdles>SPINS_BEFORE_SLEEP) { /*Start giving some time back to the OS*/
        s->sleepMs+=2;
        if (s->sleepMs>10) s->sleepMs=10;
    }

    if (s->sleepMs>0) {
        MACHSTATE1(2,"idle lock(%d) {",CmiMyPe())
        CmiIdleLock_sleep(&s->cs->idle,s->sleepMs);
        MACHSTATE1(2,"} idle lock(%d)",CmiMyPe())
    }
    }
#endif
    LrtsStillIdle();
    CsdResetPeriodic();
    MACHSTATE1(2,"still idle (%d) end {",CmiMyPe())
}

/* usually called in non-smp mode */
void CmiNotifyIdle(void) {
    AdvanceCommunication(1);
    CmiYield();
    LrtsNotifyIdle();
}

/* Utiltiy functions */
static char *CopyMsg(char *msg, size_t len) {
    char *copy = (char *)CmiAlloc(len);
#if CMK_ERROR_CHECKING
    if (!copy) {
        CmiAbort("Error: out of memory in machine layer\n");
    }
#endif
    memcpy(copy, msg, len);
    return copy;
}

/************Barrier Related Functions****************/
/* must be called on all ranks including comm thread in SMP */
int CmiBarrier(void) {
#if CMK_SMP
    /* make sure all ranks reach here, otherwise comm threads may reach barrier ignoring other ranks  */
    CmiNodeAllBarrier();
#endif
#if ( CMK_SMP && !CMK_SMP_NO_COMMTHD)
    if (CmiMyRank() == CmiMyNodeSize())
    {
#else
    if (CmiMyRank() == 0)
    {
#endif
        LrtsBarrier();
    }
#if CMK_SMP
    CmiNodeAllBarrier();
#endif
    return 0;
}

/**********Lock Related Functions**********/
#if CMK_USE_COMMON_LOCK
#if CMK_SHARED_VARS_UNAVAILABLE /*Non-smp version of locks*/

LrtsNodeLock LrtsCreateLock(void){ return 0; }
void LrtsLock(LrtsNodeLock lock){ (lock)++; }
void LrtsUnlock(LrtsNodeLock lock){ (lock)--; }
int LrtsTryLock(LrtsNodeLock lock){ return ((lock)?1:((lock)=1,0)); }
void LrtsDestroyLock(LrtsNodeLock lock){ /* empty */ }

#else /*smp version*/
#if CMK_SHARED_VARS_NT_THREADS /*Used only by win versions*/

LrtsNodeLock LrtsCreateLock(void){
    HANDLE hMutex = CreateMutex(NULL, FALSE, NULL);
    return hMutex;
}
void LrtsLock(LrtsNodeLock lock){
    WaitForSingleObject(lock, INFINITE);
}
void LrtsUnlock(LrtsNodeLock lock){
    ReleaseMutex(lock);
}
int LrtsTryLock(LrtsNodeLock lock){
    return !!WaitForSingleObject(lock, 0);
}
void LrtsDestroyLock(LrtsNodeLock lock){
    CloseHandle(lock);
}

#else /*other smp versions uses pthread mutex by default*/
LrtsNodeLock LrtsCreateLock(void){
    void *l = malloc(sizeof(pthread_mutex_t));
    pthread_mutex_init((pthread_mutex_t *)l,(pthread_mutexattr_t *)0);
    return (LrtsNodeLock)l;
}
void LrtsLock(LrtsNodeLock lock){
    pthread_mutex_lock((pthread_mutex_t*)lock);
}
void LrtsUnlock(LrtsNodeLock lock){
    pthread_mutex_unlock((pthread_mutex_t*)lock);
}
int LrtsTryLock(LrtsNodeLock lock){
    return pthread_mutex_trylock((pthread_mutex_t*)lock);
}
void LrtsDestroyLock(LrtsNodeLock lock){
    pthread_mutex_destroy((pthread_mutex_t*)lock);
    free(lock);
}
#endif

#endif //CMK_SHARED_VARS_UNAVAILABLE
#endif //CMK_USE_COMMON_LOCK

void CmiSetNcpyAckSize(int ackSize) { // ackSize is sizeof(CkCallback) passed by the charm layer
  cmiNcpyAckSize = ackSize;
}

int LrtsGetMaxNcpyOperationInfoSize() {
  return sizeof(NcpyOperationInfo) + 2*cmiNcpyAckSize + 2*(CMK_COMMON_NOCOPY_DIRECT_BYTES + CMK_NOCOPY_DIRECT_BYTES);
}

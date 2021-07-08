
/** @file
 * MPI based machine layer
 * @ingroup Machine
 */
/*@{*/

#include <stdio.h>
#include <errno.h>
#include "converse.h"
#include "cmirdmautils.h"
#include <mpi.h>
#include <algorithm>

#ifdef AMPI
#  warning "We got the AMPI version of mpi.h, instead of the system version--"
#  warning "   Try doing an 'rm charm/include/mpi.h' and building again."
#  error "Can't build Charm++ using AMPI version of mpi.h header"
#endif

/*Support for ++debug: */
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincon.h>
#include <sys/types.h>
#include <sys/timeb.h>
static char* strsignal(int sig) {
  static char outbuf[32];
  sprintf(outbuf, "%d", sig);
  return outbuf;
}
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h> /*For getpid()*/
#endif
#include <stdlib.h> /*For sleep()*/

#include "machine.h"
#include "pcqueue.h"

/* Msg types to have different actions taken for different message types
 * REGULAR                     - Regular Charm++ message
 * ONESIDED_BUFFER_SEND        - Nocopy Entry Method API Send buffer
 * ONESIDED_BUFFER_RECV        - Nocopy Entry Method API Recv buffer
 * ONESIDED_BUFFER_DIRECT_RECV - Nocopy Direct API Recv buffer
 * ONESIDED_BUFFER_DIRECT_SEND - Nocopy Direct API Send buffer
 * POST_DIRECT_RECV            - Metadata message with Direct Recv buffer information
 * POST_DIRECT_SEND            - Metadata message with Direct Send buffer information
 * */

#define CMI_MSGTYPE(msg)            ((CmiMsgHeaderBasic *)msg)->mpiMsgType
enum mpiMsgTypes : unsigned char { REGULAR, ONESIDED_BUFFER_SEND, ONESIDED_BUFFER_RECV, ONESIDED_BUFFER_DIRECT_RECV, ONESIDED_BUFFER_DIRECT_SEND, POST_DIRECT_RECV, POST_DIRECT_SEND};

/* =======Beginning of Definitions of Performance-Specific Macros =======*/
/* Whether to use multiple send queue in SMP mode */
#define MULTI_SENDQUEUE    0

/* ###Beginning of flow control related macros ### */
#define CMI_EXERT_SEND_CAP 0
#define CMI_EXERT_RECV_CAP 0

#define CMI_DYNAMIC_EXERT_CAP 0
/* This macro defines the max number of msgs in the sender msg buffer
 * that is allowed for recving operation to continue
 */
static int CMI_DYNAMIC_OUTGOING_THRESHOLD=4;
#define CMI_DYNAMIC_MAXCAPSIZE 1000
static int CMI_DYNAMIC_SEND_CAPSIZE=4;
static int CMI_DYNAMIC_RECV_CAPSIZE=3;
/* initial values, -1 indiates there's no cap */
static int dynamicSendCap = CMI_DYNAMIC_MAXCAPSIZE;
static int dynamicRecvCap = CMI_DYNAMIC_MAXCAPSIZE;
MPI_Comm charmComm;
int tagUb;

#if CMI_EXERT_SEND_CAP
static int SEND_CAP=3;
#endif

#if CMI_EXERT_RECV_CAP
static int RECV_CAP=2;
#endif
/* ###End of flow control related macros ### */

/* ###Beginning of machine-layer-tracing related macros ### */
#if CMK_TRACE_ENABLED && CMK_SMP_TRACE_COMMTHREAD
#define CMI_MPI_TRACE_MOREDETAILED 0
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

#if CMI_MACH_TRACE_USEREVENTS && CMK_TRACE_ENABLED && !CMK_TRACE_IN_CHARM
CpvStaticDeclare(double, projTraceStart);
#define  START_EVENT()  CpvAccess(projTraceStart) = CmiWallTimer();
#define  END_EVENT(x)   traceUserBracketEvent(x, CpvAccess(projTraceStart), CmiWallTimer());
#else
#define  START_EVENT()
#define  END_EVENT(x)
#endif

#if CMK_SMP_TRACE_COMMTHREAD
#define START_TRACE_SENDCOMM(msg)  \
                        int isTraceEligible = traceBeginCommOp(msg); \
                        if(isTraceEligible) traceSendMsgComm(msg);
#define END_TRACE_SENDCOMM(msg) if(isTraceEligible) traceEndCommOp(msg);

#define CONDITIONAL_TRACE_USER_EVENT(x) \
                        do{ \
                            double etime = CmiWallTimer(); \
                            if(etime - CpvAccess(projTraceStart) > 5*1e-6){ \
                                traceUserBracketEvent(x, CpvAccess(projTraceStart), etime); \
                            }\
                        }while(0);
#else
#define START_TRACE_SENDCOMM(msg)
#define END_TRACE_SENDCOMM(msg)
#define CONDITIONAL_TRACE_USER_EVENT(x)
#endif

/* ###End of machine-layer-tracing related macros ### */

/* ###Beginning of POST_RECV related macros ### */
/*
 * If MPI_POST_RECV is defined, we provide default values for
 * size and number of posted recieves. If MPI_POST_RECV_COUNT
 * is set then a default value for MPI_POST_RECV_SIZE is used
 * if not specified by the user.
 */
#define MPI_POST_RECV 0

/* Making those parameters configurable for testing them easily */

#if MPI_POST_RECV
#define MPI_DYNAMIC_POST_RECV 0

/* Note the tag offset of a msg is determined by
 * (its size - MPI_RECV_LOWERSIZE)/MPI_POST_RECV_INC.
 * based on POST_RECV_TAG.
 */
static int MPI_POST_RECV_COUNT=10;

/* The range of msgs to be tracked for histogramming */
static int MPI_POST_RECV_LOWERSIZE=8000;
static int MPI_POST_RECV_UPPERSIZE=64000;

/* The increment of msg size to be tracked, i.e. the histogram bucket size */
static int MPI_POST_RECV_INC = 1000;

/* The unit increment of msg cnt for increase #buf for a post recved msg */
static int MPI_POST_RECV_MSG_INC = 400;

/* If the #msg exceeds this value, post recv is created for such msg */
static int MPI_POST_RECV_MSG_CNT_THRESHOLD = 200;

/* The frequency of checking the existing posted recv buffers in the unit of #msgs */
static int MPI_POST_RECV_FREQ = 1000;

static int MPI_POST_RECV_SIZE;

typedef struct mpiPostRecvList {
    /* POST_RECV_TAG + msgSizeIdx is the recv tag;
     * Based on this value, this buf corresponds to msg size ranging
     * [msgSizeIdx*MPI_POST_RECV_INC, (msgSizeIdx+1)*MPI_POST_RECV_INC)
     */
    int msgSizeIdx;
    int bufCnt;
    MPI_Request *postedRecvReqs;
    char **postedRecvBufs;
    struct mpiPostRecvList *next;
} MPIPostRecvList;
CpvDeclare(MPIPostRecvList *, postRecvListHdr);
CpvDeclare(MPIPostRecvList *, curPostRecvPtr);
CpvDeclare(int, msgRecvCnt);

CpvDeclare(unsigned long long, Cmi_posted_recv_total);
CpvDeclare(unsigned long long, Cmi_unposted_recv_total);
CpvDeclare(MPI_Request*, CmiPostedRecvRequests); /* An array of request handles for posted recvs */
CpvDeclare(char**,CmiPostedRecvBuffers);

/* Note: currently MPI doesn't provide a function whether a request is in progress.
 * For example, a irecv has been filled partially. Then a call to MPI_Test still returns
 * indicating it has not been finished. If only relying on this result, then calling
 * MPI_Cancel will result in a loss of this msg. The dynamic post recv mechanism
 * can only be safely used in a synchronized point such as load balancing.
 */
#if MPI_DYNAMIC_POST_RECV
static int MSG_HISTOGRAM_BINSIZE;
static int MAX_HISTOGRAM_BUCKETS; /* only cares msg size less 2 MB */
CpvDeclare(int *, MSG_HISTOGRAM_ARRAY);
static void recordMsgHistogramInfo(int size);
static void reportMsgHistogramInfo(void);
#endif /* end of MPI_DYNAMIC_POST_RECV defined */

#endif /* end of MPI_POST_RECV defined */

/* to avoid MPI's in order delivery, changing MPI Tag all the time */
#define TAG     1375
#if MPI_POST_RECV
#define POST_RECV_TAG       (TAG+1)
#define BARRIER_ZERO_TAG  TAG
#else
#define BARRIER_ZERO_TAG   (TAG-1)
#endif

#define USE_MPI_CTRLMSG_SCHEME 0

/* Defining this macro will use MPI_Irecv instead of MPI_Recv for
 * large messages. This could save synchronization overhead caused by
 * the rzv protocol used by MPI
 */
#define USE_ASYNC_RECV_FUNC 0

#if USE_ASYNC_RECV_FUNC || USE_MPI_CTRLMSG_SCHEME
static int IRECV_MSG_THRESHOLD = 8000;
typedef struct IRecvListEntry{
    MPI_Request req;
    char *msg;
    int size;
    struct IRecvListEntry *next;
}*IRecvList;

static IRecvList freedIrecvList = NULL; /* used to recycle the entries */
static IRecvList waitIrecvListHead = NULL; /* points to the guardian entry, i.e., the next of it points to the first entry */
static IRecvList waitIrecvListTail = NULL; /* points to the last entry */

static IRecvList irecvListEntryAllocate(void){
    IRecvList ret;
    if(freedIrecvList == NULL) {
        ret = (IRecvList)malloc(sizeof(struct IRecvListEntry));        
        return ret;
    } else {
        ret = freedIrecvList;
        freedIrecvList = freedIrecvList->next;
        return ret;
    }
}
static void irecvListEntryFree(IRecvList used){
    used->next = freedIrecvList;
    freedIrecvList = used;
}

#endif /* end of USE_ASYNC_RECV_FUNC || USE_MPI_CTRLMSG_SCHEME */

/* Providing functions for external usage to set up the dynamic recv buffer
 * when the user is aware that it's safe to call such function
 */
void CmiSetupMachineRecvBuffers(void);

#define CAPTURE_MSG_HISTOGRAM 0
#if CAPTURE_MSG_HISTOGRAM && !MPI_DYNAMIC_POST_RECV
static int MSG_HISTOGRAM_BINSIZE=1000;
static int MAX_HISTOGRAM_BUCKETS=2000; /* only cares msg size less 2 MB */
CpvDeclare(int *, MSG_HISTOGRAM_ARRAY);
static void recordMsgHistogramInfo(int size);
static void reportMsgHistogramInfo(void);
#endif

/* ###End of POST_RECV related related macros ### */


#define NETWORK_PROGRESS_PERIOD_DEFAULT 0
#define MAX_QLEN 200

/* =======End of Definitions of Performance-Specific Macros =======*/


/* =====Beginning of Definitions of Message-Corruption Related Macros=====*/
#define CMI_MAGIC(msg)			 ((CmiMsgHeaderBasic *)msg)->magic
#define CHARM_MAGIC_NUMBER		 126

#if CMK_ERROR_CHECKING
unsigned char computeCheckSum(unsigned char *data, int len);
static int checksum_flag = 0;
#define CMI_SET_CHECKSUM(msg, len)	\
	if (checksum_flag)  {	\
	  ((CmiMsgHeaderBasic *)msg)->cksum = 0; 	\
	  ((CmiMsgHeaderBasic *)msg)->cksum = computeCheckSum((unsigned char*)msg, len);	\
	}
#define CMI_CHECK_CHECKSUM(msg, len)	\
	if (checksum_flag) 	\
	  if (computeCheckSum((unsigned char*)msg, len) != 0) 	\
	    CmiAbort("Fatal error: checksum doesn't agree!\n");
#else
#define CMI_SET_CHECKSUM(msg, len)
#define CMI_CHECK_CHECKSUM(msg, len)
#endif
/* =====End of Definitions of Message-Corruption Related Macros=====*/

/* =====Beginning of Declarations of Machine Specific Variables===== */
#include <signal.h>
#if !defined(_WIN32)
struct sigaction signal_int;
#else
void (*signal_int)(int);
#endif
static int _thread_provided = -1; /* Indicating MPI thread level */
static int idleblock = 0;

#if __FAULT__ 
typedef struct crashedrank{
  int rank;
  struct crashedrank *next;
} crashedRankList;
CpvDeclare(crashedRankList *, crashedRankHdr);
CpvDeclare(crashedRankList *, crashedRankPtr);
int isRankDie(int rank);
#endif

#include "machine-rdma.h"
#if CMK_ONESIDED_IMPL
int srcRank;
#if CMK_SMP
//Lock used for incrementing rdmaTag in SMP mode
static CmiNodeLock rdmaTagLock = 0;
#endif

#define RDMA_BASE_TAG     TAG+2
#define RDMA_ACK_TAG      TAG-2
int rdmaTag=RDMA_BASE_TAG;
#include "machine-onesided.h"
#endif //end of CMK_ONESIDED_IMPL

/* A simple list for msgs that have been sent by MPI_Isend */
typedef struct msg_list {
    char *msg;
    struct msg_list *next;
    int size, destpe, mode, type;
    MPI_Request req;
#if CMK_ONESIDED_IMPL
    void *ref;
    // This field can store the pointer to any structure that might have to be accessed.
    // For rdma messages, it stores the pointer to rdma buffer specific information (ack, tag)
    //for rdma messages, tag is greater than RDMA_BASE_TAG; regular messages, it is 0
#endif
#if __FAULT__
    int dstrank; //used in fault tolerance protocol, if the destination is the died rank, delete the msg
#endif
} SMSG_LIST;

CpvDeclare(SMSG_LIST *, sent_msgs);
CpvDeclare(SMSG_LIST *, end_sent);

CpvDeclare(int, MsgQueueLen);
static int request_max;
/*FLAG: consume outstanding Isends in scheduler loop*/
static int no_outstanding_sends=0;

#if NODE_0_IS_CONVHOST
int inside_comm = 0;
#endif

typedef struct ProcState {
#if MULTI_SENDQUEUE
    PCQueue      postMsgBuf;       /* per processor message sending queue */
#endif
    CmiNodeLock  recvLock;		    /* for cs->recv */
} ProcState;
static ProcState  *procState;

#if CMK_SMP && !MULTI_SENDQUEUE
PCQueue postMsgBuf;
static CmiNodeLock  postMsgBufLock = NULL;        /* for postMsgBuf */
#endif
/* =====End of Declarations of Machine Specific Variables===== */

#if CMK_MEM_CHECKPOINT || CMK_MESSAGE_LOGGING
#define FAIL_TAG   1200
int num_workpes, total_pes;
int *petorank = NULL;
int  nextrank;
void mpi_end_spare(void);
#endif

/* =====Beginning of Declarations of Machine Specific Functions===== */
/* Utility functions */

static size_t CheckAllAsyncMsgsSent(void);
static void ReleasePostedMessages(void);
static int PumpMsgs(void);
static void PumpMsgsBlocking(void);

#if CMK_SMP
static int MsgQueueEmpty(void);
static int RecvQueueEmpty(void);
static int SendMsgBuf(void);
static  void EnqueueMsg(void *m, int size, int node, int mode, int type, void *ref);
#endif

/* ### End of Machine-running Related Functions ### */

/* ### Beginning of Idle-state Related Functions ### */
void CmiNotifyIdleForMPI(void);
/* ### End of Idle-state Related Functions ### */

/* =====End of Declarations of Machine Specific Functions===== */

/**
 *  Macros that overwrites the common codes, such as
 *  CMK_SMP_NO_COMMTHD, NETWORK_PROGRESS_PERIOD_DEFAULT,
 *  USE_COMMON_SYNC_P2P, CMK_HAS_SIZE_IN_MSGHDR,
 *  CMK_OFFLOAD_BCAST_PROCESS etc.
 */
#define CMK_HAS_SIZE_IN_MSGHDR 0
#include "machine-lrts.h"
#include "machine-common-core.C"

#if USE_MPI_CTRLMSG_SCHEME
#include "machine-ctrlmsg.C"
#endif

SMSG_LIST *allocateSmsgList(char *msg, int destNode, int size, int mode, int type, void *ref) {
  SMSG_LIST *msg_tmp = (SMSG_LIST *) malloc(sizeof(SMSG_LIST));
  msg_tmp->msg = msg;
  msg_tmp->destpe = destNode;
  msg_tmp->size = size;
  msg_tmp->next = 0;
  msg_tmp->mode = mode;
  msg_tmp->type = type;
#if CMK_ONESIDED_IMPL
  msg_tmp->ref = ref;
#endif
  return msg_tmp;
}

/* The machine specific msg-sending function */

#if CMK_SMP
static void EnqueueMsg(void *m, int size, int node, int mode, int type, void *ref) {
    /*SMSG_LIST *msg_tmp = (SMSG_LIST *) CmiAlloc(sizeof(SMSG_LIST));*/
    SMSG_LIST *msg_tmp = allocateSmsgList((char *)m, node, size, mode, type, ref);
    MACHSTATE1(3,"EnqueueMsg to node %d {{ ", node);
    msg_tmp->msg = (char *)m;
    msg_tmp->size = size;
    msg_tmp->destpe = node;
    msg_tmp->next = 0;
    msg_tmp->mode = mode;
#if CMK_ONESIDED_IMPL
    msg_tmp->ref = NULL;
#endif

#if MULTI_SENDQUEUE
    PCQueuePush(procState[CmiMyRank()].postMsgBuf,(char *)msg_tmp);
#else
    /*CmiLock(postMsgBufLock);*/
    PCQueuePush(postMsgBuf,(char *)msg_tmp);
    /*CmiUnlock(postMsgBufLock);*/
#endif

    MACHSTATE3(3,"}} EnqueueMsg to %d finish with queue %p len: %d", node, postMsgBuf, PCQueueLength(postMsgBuf));
}
#endif

/* The function that calls MPI_Isend so that both non-SMP and SMP could use */
static CmiCommHandle MPISendOneMsg(SMSG_LIST *smsg) {
    int node = smsg->destpe;
    int size = smsg->size;
    char *msg = smsg->msg;
    int mode = smsg->mode;
    int dstrank;

    MACHSTATE2(3,"MPI_send to node %d rank: %d{", node, CMI_DEST_RANK(msg));
#if CMK_ERROR_CHECKING
    CMI_MAGIC(msg) = CHARM_MAGIC_NUMBER;
    CMI_SET_CHECKSUM(msg, size);
#endif

#if MPI_POST_RECV
    if (size>=MPI_POST_RECV_LOWERSIZE && size < MPI_POST_RECV_UPPERSIZE) {
#if MPI_DYNAMIC_POST_RECV
        int sendTagOffset = (size-MPI_POST_RECV_LOWERSIZE)/MPI_POST_RECV_INC+1;
        START_TRACE_SENDCOMM(msg);
        if (MPI_SUCCESS != MPI_Isend((void *)msg,size,MPI_BYTE,node,POST_RECV_TAG+sendTagOffset,charmComm,&(smsg->req)))
            CmiAbort("MPISendOneMsg: MPI_Isend failed!\n");
        END_TRACE_SENDCOMM(msg);
#else
        START_TRACE_SENDCOMM(msg);
        if (MPI_SUCCESS != MPI_Isend((void *)msg,size,MPI_BYTE,node,POST_RECV_TAG,charmComm,&(smsg->req)))
            CmiAbort("MPISendOneMsg: MPI_Isend failed!\n");
        END_TRACE_SENDCOMM(msg);
#endif
    } else {
        START_TRACE_SENDCOMM(msg);
	    if (MPI_SUCCESS != MPI_Isend((void *)msg,size,MPI_BYTE,node,TAG,charmComm,&(smsg->req)))
            CmiAbort("MPISendOneMsg: MPI_Isend failed!\n");
        END_TRACE_SENDCOMM(msg);
    }
#elif USE_MPI_CTRLMSG_SCHEME
    sendViaCtrlMsg(node, size, msg, smsg);
#else
/* branch not using MPI_POST_RECV or USE_MPI_CTRLMSG_SCHEME */

#if CMK_MEM_CHECKPOINT || CMK_MESSAGE_LOGGING
	dstrank = petorank[node];
        smsg->dstrank = dstrank;
#else
	dstrank=node;
#endif
    START_TRACE_SENDCOMM(msg)
    if (MPI_SUCCESS != MPI_Isend((void *)msg,size,MPI_BYTE,dstrank,TAG,charmComm,&(smsg->req)))
        CmiAbort("MPISendOneMsg: MPI_Isend failed!\n");
    END_TRACE_SENDCOMM(msg)
#endif /* end of #if MPI_POST_RECV */

    MACHSTATE(3,"}MPI_Isend end");
    CpvAccess(MsgQueueLen)++;
    if (CpvAccess(sent_msgs)==0)
        CpvAccess(sent_msgs) = smsg;
    else {
        CpvAccess(end_sent)->next = smsg;
    }
    CpvAccess(end_sent) = smsg;

#if !CMI_DYNAMIC_EXERT_CAP && !CMI_EXERT_SEND_CAP
    if (mode == P2P_SYNC || mode == P2P_ASYNC)
    {
    while (CpvAccess(MsgQueueLen) > request_max) {
        ReleasePostedMessages();
        PumpMsgs();
    }
    }
#endif

    return (CmiCommHandle) &(smsg->req);
}

CmiCommHandle LrtsSendFunc(int destNode, int destPE, int size, char *msg, int mode) {
    /* Ignoring the mode for MPI layer */

    CmiState cs = CmiGetState();
    SMSG_LIST *msg_tmp;

    CmiAssert(destNode != CmiMyNodeGlobal());
    // Mark the message type as REGULAR to indicate a regular charm message
    CMI_MSGTYPE(msg) = REGULAR;
#if CMK_SMP
    if (Cmi_smp_mode_setting == COMM_THREAD_SEND_RECV) {
      EnqueueMsg(msg, size, destNode, mode, REGULAR, NULL);
      return 0;
    }
#endif
    /* non smp */
    /*msg_tmp = (SMSG_LIST *) CmiAlloc(sizeof(SMSG_LIST));*/
    msg_tmp = allocateSmsgList(msg, destNode, size, mode, REGULAR, NULL);
    return MPISendOneMsg(msg_tmp);
}

static size_t CheckAllAsyncMsgsSent(void) {
    SMSG_LIST *msg_tmp = CpvAccess(sent_msgs);
    MPI_Status sts;
    int done;

    while (msg_tmp!=0) {
        done = 0;
        if (MPI_SUCCESS != MPI_Test(&(msg_tmp->req), &done, &sts))
            CmiAbort("CheckAllAsyncMsgsSent: MPI_Test failed!\n");
#if __FAULT__ 
        if(isRankDie(msg_tmp->dstrank)){
          //CmiPrintf("[%d][%d] msg to crashed rank\n",CmiMyPartition(),CmiMyPe());
          //CmiAbort("unexpected send");
          done = 1;
        }
#endif
        if (!done)
            return 0;
        msg_tmp = msg_tmp->next;
        /*    MsgQueueLen--; ????? */
    }
    return 1;
}

int CheckAsyncMsgSent(CmiCommHandle c) {

    SMSG_LIST *msg_tmp = CpvAccess(sent_msgs);
    int done;
    MPI_Status sts;

    while ((msg_tmp) && ((CmiCommHandle)&(msg_tmp->req) != c))
        msg_tmp = msg_tmp->next;
    if (msg_tmp) {
        done = 0;
        if (MPI_SUCCESS != MPI_Test(&(msg_tmp->req), &done, &sts))
            CmiAbort("CheckAsyncMsgSent: MPI_Test failed!\n");
        return ((done)?1:0);
    } else {
        return 1;
    }
}

#if CMK_ONESIDED_IMPL
#include "machine-onesided.C"
#endif

/* ######Beginning of functions related with communication progress ###### */
static void ReleasePostedMessages(void) {
    SMSG_LIST *msg_tmp=CpvAccess(sent_msgs);
    SMSG_LIST *prev=0;
    SMSG_LIST *temp;

    int done;
    MPI_Status sts;


    MACHSTATE1(2,"ReleasePostedMessages begin on %d {", CmiMyPe());
    while (msg_tmp!=0) {
        done =0;
#if CMK_SMP_TRACE_COMMTHREAD || CMK_TRACE_COMMOVERHEAD
        double startT = CmiWallTimer();
#endif
        if (MPI_Test(&(msg_tmp->req), &done, &sts) != MPI_SUCCESS)
            CmiAbort("ReleasePostedMessages: MPI_Test failed!\n");
#if __FAULT__ 
        if (isRankDie(msg_tmp->dstrank)){
          done = 1;
        }
#endif
        if (done) {
            MACHSTATE2(3,"ReleasePostedMessages release one %d to %d", CmiMyPe(), msg_tmp->destpe);
            CpvAccess(MsgQueueLen)--;
            /* Release the message */
            temp = msg_tmp->next;
            if (prev==0) /* first message */
                CpvAccess(sent_msgs) = temp;
            else
                prev->next = temp;
#if CMK_ONESIDED_IMPL
            // Update end_sent for consistent states during possible insertions
            if(CpvAccess(end_sent) == msg_tmp) {
              CpvAccess(end_sent) = prev;
            }
            if(msg_tmp->type == ONESIDED_BUFFER_DIRECT_RECV) {
                // MPI_Irecv posted as a part of the Direct API was completed
                NcpyOperationInfo *ncpyOpInfo = (NcpyOperationInfo *)(msg_tmp->ref);

                // Invoke the destination ack
                ncpyOpInfo->ackMode = CMK_DEST_ACK; // Only invoke destination ack

                // On the destination the NcpyOperationInfo is freed for the Direct API
                // but not freed for the Entry Method API as it a part of the parameter marshalled message
                // and is enentually freed by the RTS
                CmiInvokeNcpyAck(ncpyOpInfo);

            } else if(msg_tmp->type == ONESIDED_BUFFER_DIRECT_SEND) {
                // MPI_Isend posted as a part of the Direct API was completed
                NcpyOperationInfo *ncpyOpInfo = (NcpyOperationInfo *)(msg_tmp->ref);
                // Invoke the source ack
                ncpyOpInfo->ackMode = CMK_SRC_ACK; // Only invoke the source ack

                // Free the NcpyOperationInfo on the source
                ncpyOpInfo->freeMe = CMK_FREE_NCPYOPINFO;

                CmiInvokeNcpyAck(ncpyOpInfo);
            }
            else if(msg_tmp->type == POST_DIRECT_SEND || msg_tmp->type == POST_DIRECT_RECV) {
                // do nothing as the received message is a NcpyOperationInfo object
                // which is freed in the above code (either ONESIDED_BUFFER_DIRECT_RECV or
                // ONESIDED_BUFFER_DIRECT_SEND)
            }
            else
#endif
            {
              CmiFree(msg_tmp->msg);
            }
            /* CmiFree(msg_tmp); */
            free(msg_tmp);
            msg_tmp = temp;
        } else {
            prev = msg_tmp;
            msg_tmp = msg_tmp->next;
        }
#if CMK_SMP_TRACE_COMMTHREAD || CMK_TRACE_COMMOVERHEAD
        {
            double endT = CmiWallTimer();
            /* only record the event if it takes more than 1ms */
            if (endT-startT>=0.001) traceUserSuppliedBracketedNote("MPI_Test: release a msg", 60, startT, endT);
        }
#endif
    }
    MACHSTATE(2,"} ReleasePostedMessages end");
}

static int PumpMsgs(void) {
    int nbytes, flg, res;
    char *msg;
    MPI_Status sts;
    int recd=0;

#if CMI_EXERT_RECV_CAP || CMI_DYNAMIC_EXERT_CAP
    int recvCnt=0;
#endif

    MACHSTATE(2,"PumpMsgs begin {");

#if CMI_DYNAMIC_EXERT_CAP
    dynamicRecvCap = CMI_DYNAMIC_MAXCAPSIZE;
#endif

    while (1) {
        int doSyncRecv = 1;
#if CMI_EXERT_RECV_CAP
        if (recvCnt==RECV_CAP) break;
#elif CMI_DYNAMIC_EXERT_CAP
        if (recvCnt >= dynamicRecvCap) break;
#endif

#if USE_MPI_CTRLMSG_SCHEME
	doSyncRecv = 0;
	nbytes = recvViaCtrlMsg();
  recd = 1;
	if(nbytes == -1) break;
#elif MPI_POST_RECV
		/* First check posted recvs then do  probe unmatched outstanding messages */
        MPIPostRecvList *postedOne = NULL;
        int completed_index = -1;
        flg = 0;
#if MPI_DYNAMIC_POST_RECV
        MPIPostRecvList *oldPostRecvPtr = CpvAccess(curPostRecvPtr);
        if (oldPostRecvPtr) {
            /* post recv buf inited */
            do {
                /* round-robin iteration over the list */
                MPIPostRecvList *cur = CpvAccess(curPostRecvPtr);
                if (MPI_SUCCESS != MPI_Testany(cur->bufCnt, cur->postedRecvReqs, &completed_index, &flg, &sts))
                    CmiAbort("PumpMsgs: MPI_Testany failed!\n");

                if (flg) {
                    postedOne = cur;
                    break;
                }
                CpvAccess(curPostRecvPtr) = CpvAccess(curPostRecvPtr)->next;
            } while (CpvAccess(curPostRecvPtr) != oldPostRecvPtr);
        }
#else
        MPIPostRecvList *cur = CpvAccess(curPostRecvPtr);
        if (MPI_SUCCESS != MPI_Testany(cur->bufCnt, cur->postedRecvReqs, &completed_index, &flg, &sts))
            CmiAbort("PumpMsgs: MPI_Testany failed!\n");
#endif
        CONDITIONAL_TRACE_USER_EVENT(60); /* MPI_Test related user event */
        if (flg) {
            if (MPI_SUCCESS != MPI_Get_count(&sts, MPI_BYTE, &nbytes))
                CmiAbort("PumpMsgs: MPI_Get_count failed!\n");

            recd = 1;
#if !MPI_DYNAMIC_POST_RECV
            postedOne = CpvAccess(curPostRecvPtr);
#endif
            msg = (postedOne->postedRecvBufs)[completed_index];
            (postedOne->postedRecvBufs)[completed_index] = NULL;

            CpvAccess(Cmi_posted_recv_total)++;
        } else {
            START_EVENT();
            res = MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, charmComm, &flg, &sts);
            if (res != MPI_SUCCESS)
                CmiAbort("MPI_Iprobe failed\n");
            if (!flg) break;
            
            CONDITIONAL_TRACE_USER_EVENT(70); /* MPI_Iprobe related user event */
            recd = 1;
            MPI_Get_count(&sts, MPI_BYTE, &nbytes);
            msg = (char *) CmiAlloc(nbytes);

#if USE_ASYNC_RECV_FUNC
            if(nbytes >= IRECV_MSG_THRESHOLD) doSyncRecv = 0;
#endif            
            if(doSyncRecv){
                START_EVENT();
                if (MPI_SUCCESS != MPI_Recv(msg,nbytes,MPI_BYTE,sts.MPI_SOURCE,sts.MPI_TAG, charmComm,&sts))
                    CmiAbort("PumpMsgs: MPI_Recv failed!\n");                
            }
#if USE_ASYNC_RECV_FUNC        
            else {
                START_EVENT();
                IRecvList one = irecvListEntryAllocate();
                if(MPI_SUCCESS != MPI_Irecv(msg, nbytes, MPI_BYTE, sts.MPI_SOURCE, sts.MPI_TAG, charmComm, &(one->req)))
                    CmiAbort("PumpMsgs: MPI_Irecv failed!\n");
		/*printf("[%d]: irecv msg=%p, nbytes=%d, src=%d, tag=%d\n", CmiMyPe(), msg, nbytes, sts.MPI_SOURCE, sts.MPI_TAG);*/
                one->msg = msg;
                one->size = nbytes;
                one->next = NULL;
                waitIrecvListTail->next = one;
		waitIrecvListTail = one;
                CONDITIONAL_TRACE_USER_EVENT(50); /* MPI_Irecv related user events */
            }
#endif
            CpvAccess(Cmi_unposted_recv_total)++;
        }
#else
        /* Original version of not using MPI_POST_RECV and USE_MPI_CTRLMSG_SCHEME */
        START_EVENT();
        res = MPI_Iprobe(MPI_ANY_SOURCE, TAG, charmComm, &flg, &sts);
        if (res != MPI_SUCCESS)
            CmiAbort("MPI_Iprobe failed\n");

        if (!flg) break;
        CONDITIONAL_TRACE_USER_EVENT(70); /* MPI_Iprobe related user event */
        
        recd = 1;
        MPI_Get_count(&sts, MPI_BYTE, &nbytes);
        msg = (char *) CmiAlloc(nbytes);

#if USE_ASYNC_RECV_FUNC
        if(nbytes >= IRECV_MSG_THRESHOLD) doSyncRecv = 0;
#endif        
        if(doSyncRecv){
            START_EVENT();
            if (MPI_SUCCESS != MPI_Recv(msg,nbytes,MPI_BYTE,sts.MPI_SOURCE,sts.MPI_TAG, charmComm,&sts))
                CmiAbort("PumpMsgs: MPI_Recv failed!\n");            
        }
#if USE_ASYNC_RECV_FUNC        
        else {
            IRecvList one = irecvListEntryAllocate();
            if(MPI_SUCCESS != MPI_Irecv(msg, nbytes, MPI_BYTE, sts.MPI_SOURCE, sts.MPI_TAG, charmComm, &(one->req)))
                CmiAbort("PumpMsgs: MPI_Irecv failed!\n");
            one->msg = msg;
            one->size = nbytes;
            one->next = NULL;
            waitIrecvListTail->next = one;
            waitIrecvListTail = one;
            /*printf("PE[%d]: MPI_Irecv msg=%p, size=%d, entry=%p\n", CmiMyPe(), msg, nbytes, one);*/
            CONDITIONAL_TRACE_USER_EVENT(50); /* MPI_Irecv related user events */
        }
#endif

#endif /*end of !MPI_POST_RECV and !USE_MPI_CTRLMSG_SCHEME*/

		if(doSyncRecv){
			MACHSTATE2(3,"PumpMsgs recv one from node:%d to rank:%d", sts.MPI_SOURCE, CMI_DEST_RANK(msg));
			CMI_CHECK_CHECKSUM(msg, nbytes);
	#if CMK_ERROR_CHECKING
			if (CMI_MAGIC(msg) != CHARM_MAGIC_NUMBER) { /* received a non-charm msg */
				CmiPrintf("Charm++ Abort: Non Charm++ Message Received of size %d. \n", nbytes);
				CmiFree(msg);
				CmiAbort("Abort!\n");
				continue;
			}
	#endif
            if(CMI_MSGTYPE(msg) == REGULAR) {
              handleOneRecvedMsg(nbytes, msg);
            }
#if CMK_ONESIDED_IMPL
            else if(CMI_MSGTYPE(msg) == POST_DIRECT_RECV || CMI_MSGTYPE(msg) == POST_DIRECT_SEND) {

              NcpyOperationInfo *ncpyOpInfoMsg = (NcpyOperationInfo *)msg;
              resetNcpyOpInfoPointers(ncpyOpInfoMsg);

              int postMsgType, myPe, otherPe;
              const void *myBuffer;
              if(CMI_MSGTYPE(msg) == POST_DIRECT_RECV) {
                // Direct Buffer destination, post MPI_Irecv
                postMsgType = ONESIDED_BUFFER_DIRECT_RECV;
                myPe = ncpyOpInfoMsg->destPe;
                otherPe = ncpyOpInfoMsg->srcPe;
                myBuffer = ncpyOpInfoMsg->destPtr;
              }
              else {
                // Direct Buffer Source, post MPI_Isend
                postMsgType = ONESIDED_BUFFER_DIRECT_SEND;
                myPe = ncpyOpInfoMsg->srcPe;
                otherPe = ncpyOpInfoMsg->destPe;
                myBuffer = ncpyOpInfoMsg->srcPtr;
              }

              MPIPostOneBuffer(myBuffer,
                               ncpyOpInfoMsg,
                               std::min(ncpyOpInfoMsg->srcSize, ncpyOpInfoMsg->destSize),
                               otherPe,
                               ncpyOpInfoMsg->tag,
                               postMsgType);

            }
#endif
            else {
              CmiAbort("Invalid Type of message\n");
            }
        }
        
#if CAPTURE_MSG_HISTOGRAM || MPI_DYNAMIC_POST_RECV
        recordMsgHistogramInfo(nbytes);
#endif

#if  MPI_POST_RECV
#if MPI_DYNAMIC_POST_RECV
        if (postedOne) {
            //printf("[%d]: get one posted recv\n", CmiMyPe());
            /* Get the upper size of this buffer */
            int postRecvBufSize = postedOne->msgSizeIdx*MPI_POST_RECV_INC + MPI_POST_RECV_LOWERSIZE - 1;
            int postRecvTag = POST_RECV_TAG + postedOne->msgSizeIdx;
            /* Has to re-allocate the buffer for the message */
            (postedOne->postedRecvBufs)[completed_index] = (char *)CmiAlloc(postRecvBufSize);

            /* and repost the recv */
            START_EVENT();

            if (MPI_SUCCESS != MPI_Irecv((postedOne->postedRecvBufs)[completed_index] ,
                                         postRecvBufSize,
                                         MPI_BYTE,
                                         MPI_ANY_SOURCE,
                                         postRecvTag,
                                         charmComm,
                                         &((postedOne->postedRecvReqs)[completed_index])  ))
                CmiAbort("PumpMsgs: MPI_Irecv failed!\n");
            CONDITIONAL_TRACE_USER_EVENT(50); /* MPI_Irecv related user events */
        }
#else
        if (postedOne) {
            /* Has to re-allocate the buffer for the message */
            (postedOne->postedRecvBufs)[completed_index] = (char *)CmiAlloc(MPI_POST_RECV_SIZE);

            /* and repost the recv */
            START_EVENT();
            if (MPI_SUCCESS != MPI_Irecv((postedOne->postedRecvBufs)[completed_index] ,
                                         MPI_POST_RECV_SIZE,
                                         MPI_BYTE,
                                         MPI_ANY_SOURCE,
                                         POST_RECV_TAG,
                                         charmComm,
                                         &((postedOne->postedRecvReqs)[completed_index])  ))
                CmiAbort("PumpMsgs: MPI_Irecv failed!\n");
            CONDITIONAL_TRACE_USER_EVENT(50); /* MPI_Irecv related user events */
        }
#endif /* not MPI_DYNAMIC_POST_RECV */
#endif

#if CMI_EXERT_RECV_CAP
        recvCnt++;
#elif CMI_DYNAMIC_EXERT_CAP
        recvCnt++;
#if CMK_SMP
        /* check postMsgBuf to get the  number of messages that have not been sent
             * which is only available in SMP mode
         * MsgQueueLen indicates the number of messages that have not been released
             * by MPI
             */
        if (PCQueueLength(postMsgBuf) > CMI_DYNAMIC_OUTGOING_THRESHOLD
                || CpvAccess(MsgQueueLen) > CMI_DYNAMIC_OUTGOING_THRESHOLD) {
            dynamicRecvCap = CMI_DYNAMIC_RECV_CAPSIZE;
        }
#else
        /* MsgQueueLen indicates the number of messages that have not been released
             * by MPI
             */
        if (CpvAccess(MsgQueueLen) > CMI_DYNAMIC_OUTGOING_THRESHOLD) {
            dynamicRecvCap = CMI_DYNAMIC_RECV_CAPSIZE;
        }
#endif

#endif

    }

#if USE_ASYNC_RECV_FUNC || USE_MPI_CTRLMSG_SCHEME
/* Another loop to check the irecved msgs list */
{
	/*TODO: msg cap (throttling) is not exerted here */
    IRecvList irecvEnt;
    int irecvDone = 0;
    MPI_Status sts;
    while(waitIrecvListHead->next) {
        IRecvList irecvEnt = waitIrecvListHead->next;
                
        /*printf("PE[%d]: check irecv entry=%p\n", CmiMyPe(), irecvEnt);*/
        if(MPI_SUCCESS != MPI_Test(&(irecvEnt->req), &irecvDone, &sts))
            CmiAbort("PumpMsgs: MPI_Test failed!\n");
        if(!irecvDone) break; /* in-order recv */

        /*printf("PE[%d]: irecv entry=%p finished with size=%d, msg=%p\n", CmiMyPe(), irecvEnt, irecvEnt->size, irecvEnt->msg);*/
        
        handleOneRecvedMsg(irecvEnt->size, irecvEnt->msg);
        waitIrecvListHead->next = irecvEnt->next;
        irecvListEntryFree(irecvEnt);
        //recd = 1;        
    }
    if(waitIrecvListHead->next == NULL)
        waitIrecvListTail = waitIrecvListHead;
}
#endif


    MACHSTATE(2,"} PumpMsgs end ");
    return recd;
}

/* blocking version */
static void PumpMsgsBlocking(void) {
    static int maxbytes = 20000000;
    static char *buf = NULL;
    int nbytes, flg;
    MPI_Status sts;
    char *msg;
    int recd=0;

    if (!PCQueueEmpty(CmiGetState()->recv)) return;
    if (!CdsFifo_Empty(CpvAccess(CmiLocalQueue))) return;
    if (!CqsEmpty(CpvAccess(CsdSchedQueue))) return;
    if (CpvAccess(sent_msgs))  return;

#if 0
    CmiPrintf("[%d] PumpMsgsBlocking. \n", CmiMyPe());
#endif

    if (buf == NULL) {
        buf = (char *) CmiAlloc(maxbytes);
        _MEMCHECK(buf);
    }


#if MPI_POST_RECV
#warning "Using MPI posted receives and PumpMsgsBlocking() will break"
    CmiAbort("Unsupported use of PumpMsgsBlocking. This call should be extended to check posted recvs, cancel them all, and then wait on any incoming message, and then re-post the recvs");
#endif

    if (MPI_SUCCESS != MPI_Recv(buf,maxbytes,MPI_BYTE,MPI_ANY_SOURCE,TAG, charmComm,&sts))
        CmiAbort("PumpMsgs: PMP_Recv failed!\n");    

    MPI_Get_count(&sts, MPI_BYTE, &nbytes);
    msg = (char *) CmiAlloc(nbytes);
    memcpy(msg, buf, nbytes);

#if CMK_SMP_TRACE_COMMTHREAD && CMI_MPI_TRACE_MOREDETAILED
    char tmp[32];
    sprintf(tmp, "To proc %d", CmiNodeFirst(CmiMyNode())+CMI_DEST_RANK(msg));
    traceUserSuppliedBracketedNote(tmp, 30, CpvAccess(projTraceStart), CmiWallTimer());
#endif

    handleOneRecvedMsg(nbytes, msg);
}


#if CMK_SMP

/* called by communication thread in SMP */
static int SendMsgBuf(void) {
    SMSG_LIST *msg_tmp;
    char *msg;
    int node, rank, size;
    int i;
    int sent = 0;

#if CMI_EXERT_SEND_CAP || CMI_DYNAMIC_EXERT_CAP
    int sentCnt = 0;
#endif

#if CMI_DYNAMIC_EXERT_CAP
    dynamicSendCap = CMI_DYNAMIC_MAXCAPSIZE;
#endif

    MACHSTATE(2,"SendMsgBuf begin {");
#if MULTI_SENDQUEUE
    for (i=0; i<_Cmi_mynodesize+1; i++) { /* subtle: including comm thread */
        if (!PCQueueEmpty(procState[i].postMsgBuf)) {
            msg_tmp = (SMSG_LIST *)PCQueuePop(procState[i].postMsgBuf);
#else
    /* single message sending queue */
    /* CmiLock(postMsgBufLock); */
    msg_tmp = (SMSG_LIST *)PCQueuePop(postMsgBuf);
    /* CmiUnlock(postMsgBufLock); */
    while (NULL != msg_tmp) {
#endif

#if CMK_ONESIDED_IMPL
            if(msg_tmp->type == ONESIDED_BUFFER_DIRECT_RECV || msg_tmp->type == ONESIDED_BUFFER_DIRECT_SEND) {
                NcpyOperationInfo *ncpyOpInfo = (NcpyOperationInfo *)(msg_tmp->ref);
                MPISendOrRecvOneBuffer(msg_tmp, ncpyOpInfo->tag);
            }
            else
#endif
            {
                MPISendOneMsg(msg_tmp);
            }
            sent=1;

#if CMI_EXERT_SEND_CAP
            if (++sentCnt == SEND_CAP) break;
#elif CMI_DYNAMIC_EXERT_CAP
            if (++sentCnt >= dynamicSendCap) break;
            if (CpvAccess(MsgQueueLen) > CMI_DYNAMIC_OUTGOING_THRESHOLD)
                dynamicSendCap = CMI_DYNAMIC_SEND_CAPSIZE;
#endif

#if ! MULTI_SENDQUEUE
            /* CmiLock(postMsgBufLock); */
            msg_tmp = (SMSG_LIST *)PCQueuePop(postMsgBuf);
            /* CmiUnlock(postMsgBufLock); */
#endif
        }
#if MULTI_SENDQUEUE
    }
#endif
    MACHSTATE(2,"}SendMsgBuf end ");
    return sent;
}

static int MsgQueueEmpty() {
    int i;
#if MULTI_SENDQUEUE
    for (i=0; i<_Cmi_mynodesize; i++)
        if (!PCQueueEmpty(procState[i].postMsgBuf)) return 0;
#else
    return PCQueueEmpty(postMsgBuf);
#endif
    return 1;
}

/* test if all processors recv queues are empty */
static int RecvQueueEmpty() {
    int i;
    for (i=0; i<_Cmi_mynodesize; i++) {
        CmiState cs=CmiGetStateN(i);
        if (!PCQueueEmpty(cs->recv)) return 0;
    }
    return 1;
}


#define REPORT_COMM_METRICS 0
#if REPORT_COMM_METRICS
static double pumptime = 0.0;
static double releasetime = 0.0;
static double sendtime = 0.0;
#endif

#endif //end of CMK_SMP

void LrtsAdvanceCommunication(int whenidle) {
#if REPORT_COMM_METRICS
    double t1, t2, t3, t4;
    t1 = CmiWallTimer();
#endif

#if CMK_SMP
    PumpMsgs();

#if REPORT_COMM_METRICS
    t2 = CmiWallTimer();
#endif

    ReleasePostedMessages();
#if REPORT_COMM_METRICS
    t3 = CmiWallTimer();
#endif

    SendMsgBuf();

#if REPORT_COMM_METRICS
    t4 = CmiWallTimer();
    pumptime += (t2-t1);
    releasetime += (t3-t2);
    sendtime += (t4-t3);
#endif

#else /* non-SMP case */
    ReleasePostedMessages();

#if REPORT_COMM_METRICS
    t2 = CmiWallTimer();
#endif
    PumpMsgs();

#if REPORT_COMM_METRICS
    t3 = CmiWallTimer();
    pumptime += (t3-t2);
    releasetime += (t2-t1);
#endif

#endif /* end of #if CMK_SMP */
}
/* ######End of functions related with communication progress ###### */

void LrtsPostNonLocal(void) {
#if !CMK_SMP
    if (no_outstanding_sends) {
        while (CpvAccess(MsgQueueLen)>0) {
            LrtsAdvanceCommunication(0);
        }
    }

    /* FIXME: I don't think the following codes are needed because
     * it repeats the same job of the next call of CmiGetNonLocal
     */
#if 0
    if (!msg) {
        ReleasePostedMessages();
        if (PumpMsgs())
            return  PCQueuePop(cs->recv);
        else
            return 0;
    }
#endif
#else
  if (Cmi_smp_mode_setting == COMM_THREAD_ONLY_RECV) {
        ReleasePostedMessages();       
        /* ??? SendMsgBuf is a not a thread-safe function. If it is put
         * here and this function will be called in CmiNotifyStillIdle,
         * then a data-race problem occurs */
        /*SendMsgBuf();*/
  }
#endif
}

/* Idle-state related functions: called in non-smp mode */
void CmiNotifyIdleForMPI(void) {
    ReleasePostedMessages();
    if (!PumpMsgs() && idleblock) PumpMsgsBlocking();
}

/* Network progress function is used to poll the network when for
   messages. This flushes receive buffers on some  implementations*/
#if CMK_MACHINE_PROGRESS_DEFINED
void CommunicationServerThread(int);

void CmiMachineProgressImpl(void) {
#if !CMK_SMP
    PumpMsgs();
#if CMK_IMMEDIATE_MSG
    CmiHandleImmediate();
#endif
#else
    /*Not implemented yet. Communication server does not seem to be
      thread safe, so only communication thread call it */
    if (CmiMyRank() == CmiMyNodeSize())
        CommunicationServerThread(0);
#endif
}
#endif

/* ######Beginning of functions related with exiting programs###### */
void LrtsDrainResources(void) {
#if !CMK_SMP
    while (!CheckAllAsyncMsgsSent()) {
        PumpMsgs();
        ReleasePostedMessages();
    }
#else
    if(Cmi_smp_mode_setting == COMM_THREAD_SEND_RECV){
        while (!MsgQueueEmpty() || !CheckAllAsyncMsgsSent()) {
	    ReleasePostedMessages();
            SendMsgBuf();
            PumpMsgs();
        }
    }else if(Cmi_smp_mode_setting == COMM_THREAD_ONLY_RECV) {
        while(!CheckAllAsyncMsgsSent()) {
            ReleasePostedMessages();
        }
    }
#endif
#if CMK_MEM_CHECKPOINT || CMK_MESSAGE_LOGGING
    if (CmiMyPe() == 0 && CmiMyPartition()==0)
    { 
      mpi_end_spare();
    }
#endif
    MACHSTATE(2, "Machine exit barrier begin {");
    START_EVENT();
    if (MPI_SUCCESS != MPI_Barrier(charmComm))
        CmiAbort("LrtsDrainResources: MPI_Barrier failed!\n");
    END_EVENT(10);
    MACHSTATE(2, "} Machine exit barrier end");
}

void LrtsExit(int exitcode) {
    int i;
#if (CMK_DEBUG_MODE || CMK_WEB_MODE || NODE_0_IS_CONVHOST)
    int doPrint = 0;
    if (CmiMyNode()==0) doPrint = 1;

    if (doPrint /*|| CmiMyNode()%11==0 */) {
#if MPI_POST_RECV
        CmiPrintf("node[%d]: %llu posted receives,  %llu unposted receives\n", CmiMyNode(), CpvAccess(Cmi_posted_recv_total), CpvAccess(Cmi_unposted_recv_total));
#endif
    }
#endif

#if MPI_POST_RECV
    {
        MPIPostRecvList *ptr = CpvAccess(postRecvListHdr);
        if (ptr) {
            do {
                for (i=0; i<ptr->bufCnt; i++) MPI_Cancel(ptr->postedRecvReqs+i);
                ptr = ptr->next;
            } while (ptr!=CpvAccess(postRecvListHdr));
        }
    }
#endif

#if REPORT_COMM_METRICS
#if CMK_SMP
    CmiPrintf("Report comm metrics for node %d[%d-%d]: pumptime: %f, releasetime: %f, senttime: %f\n",
              CmiMyNode(), CmiNodeFirst(CmiMyNode()), CmiNodeFirst(CmiMyNode())+CmiMyNodeSize()-1,
              pumptime, releasetime, sendtime);
#else
    CmiPrintf("Report comm metrics for proc %d: pumptime: %f, releasetime: %f, senttime: %f\n",
              CmiMyPe(), pumptime, releasetime, sendtime);
#endif
#endif
    
   if(!CharmLibInterOperate || userDrivenMode) {
#if ! CMK_AUTOBUILD
#if !defined(_WIN32)
      sigaction(SIGINT, &signal_int, NULL);
#else
      signal(SIGINT, signal_int);
#endif
      MPI_Finalize();
#endif
      // Still want to return control to the user in userDrivenMode
      if (!userDrivenMode) {
        exit(exitcode);
      }
    }
}

static int Cmi_truecrash;

static void KillOnAllSigs(int sigNo) {
    static int already_in_signal_handler = 0;
    char *m;
    if (already_in_signal_handler) return;   /* MPI_Abort(charmComm,1); */
    already_in_signal_handler = 1;

    CmiAbortHelper("Caught Signal", strsignal(sigNo), NULL, 1, 1);
}
/* ######End of functions related with exiting programs###### */


/* ######Beginning of functions related with starting programs###### */
static void registerMPITraceEvents(void) {
#if CMI_MACH_TRACE_USEREVENTS && CMK_TRACE_ENABLED && !CMK_TRACE_IN_CHARM
    traceRegisterUserEvent("MPI_Barrier", 10);
    traceRegisterUserEvent("MPI_Send", 20);
    traceRegisterUserEvent("MPI_Recv", 30);
    traceRegisterUserEvent("MPI_Isend", 40);
    traceRegisterUserEvent("MPI_Irecv", 50);
    traceRegisterUserEvent("MPI_Test[any]", 60);
    traceRegisterUserEvent("MPI_Iprobe", 70);
#endif
}

static const char *thread_level_tostring(int thread_level) {
#if CMK_MPI_INIT_THREAD
    switch (thread_level) {
    case MPI_THREAD_SINGLE:
        return "MPI_THREAD_SINGLE";
    case MPI_THREAD_FUNNELED:
        return "MPI_THREAD_FUNNELED";
    case MPI_THREAD_SERIALIZED:
        return "MPI_THREAD_SERIALIZED";
    case MPI_THREAD_MULTIPLE :
        return "MPI_THREAD_MULTIPLE";
    default: {
        char *str = (char *)malloc(5); // XXX: leaked
        sprintf(str,"%d", thread_level);
        return str;
    }
    }
    return  "unknown";
#else
    char *str = (char *)malloc(5); // XXX: leaked
    sprintf(str,"%d", thread_level);
    return str;
#endif
}

extern int quietMode;

/**
 *  Obtain the number of nodes, my node id, and consuming machine layer
 *  specific arguments
 */
void LrtsInit(int *argc, char ***argv, int *numNodes, int *myNodeID) {
    int n,i;
    int ver, subver;
    int provided;
    int thread_level;
    int myNID;
    int largc=*argc;
    char** largv=*argv;
    int tagUbGetResult;
    void *tagUbVal;

    if (CmiGetArgFlag(largv, "+comm_thread_only_recv")) {
#if CMK_SMP
      Cmi_smp_mode_setting = COMM_THREAD_ONLY_RECV;
#else
      CmiAbort("+comm_thread_only_recv option can only be used with SMP version of Charm++");
#endif
    }

    *argc = CmiGetArgc(largv);     /* update it in case it is out of sync */

    if(!CharmLibInterOperate || userDrivenMode) {
#if CMK_MPI_INIT_THREAD
#if CMK_SMP
    if (Cmi_smp_mode_setting == COMM_THREAD_SEND_RECV)
        thread_level = MPI_THREAD_FUNNELED;
      else
        thread_level = MPI_THREAD_MULTIPLE;
#else
      thread_level = MPI_THREAD_SINGLE;
#endif
      MPI_Init_thread(argc, argv, thread_level, &provided);
      _thread_provided = provided;
#else
      MPI_Init(argc, argv);
      thread_level = 0;
      _thread_provided = -1;
#endif
    }

    largc = *argc;
    largv = *argv;
    if(!CharmLibInterOperate || userDrivenMode) {
      MPI_Comm_dup(MPI_COMM_WORLD, &charmComm);
    }
    MPI_Comm_size(charmComm, numNodes);
    MPI_Comm_rank(charmComm, myNodeID);

#if CMK_ONESIDED_IMPL
    /* srcRank stores the rank of the sender MPI process
     * and is a global variable used for rdma md messages */
    srcRank = *myNodeID;
    MPI_Comm_get_attr(MPI_COMM_WORLD, MPI_TAG_UB, &tagUbVal, &tagUbGetResult);
    CmiAssert(tagUbGetResult);
    tagUb = *(int *)tagUbVal;
#endif

    MPI_Bcast(&_Cmi_mynodesize, 1, MPI_INT, 0, charmComm);

    myNID = *myNodeID;

    MPI_Get_version(&ver, &subver);
    if(!CharmLibInterOperate) {
      if ((myNID == 0) && (!quietMode)) {
        printf("Charm++> Running on MPI version: %d.%d\n", ver, subver);
        printf("Charm++> level of thread support used: %s (desired: %s)\n", thread_level_tostring(_thread_provided), thread_level_tostring(thread_level));
      }
    }

#if CMK_SMP
    if (Cmi_smp_mode_setting == COMM_THREAD_ONLY_RECV && _thread_provided != MPI_THREAD_MULTIPLE) {
        Cmi_smp_mode_setting = COMM_THREAD_SEND_RECV; 
        if ((myNID == 0) && (!quietMode)) {
          printf("Charm++> +comm_thread_only_recv disabled\n");
        }
    }
#endif

    {
#if CMK_OPTIMIZE
        Cmi_truecrash = 0;
#else
        Cmi_truecrash = 1;
#endif
        int debug = CmiGetArgFlag(largv,"++debug");
        if (CmiGetArgFlagDesc(*argv,"+truecrash","Do not install signal handlers") ||
            debug ) Cmi_truecrash = 1;
        int debug_no_pause = CmiGetArgFlag(largv,"++debug-no-pause");
        if (debug || debug_no_pause) {  /*Pause so user has a chance to start and attach debugger*/
#if CMK_HAS_GETPID
            if (!quietMode) printf("CHARMDEBUG> Processor %d has PID %d\n",myNID,getpid());
            fflush(stdout);
            if (!debug_no_pause)
                sleep(15);
#else
            if (!quietMode) printf("++debug ignored.\n");
#endif
        }
    }


#if CMK_MEM_CHECKPOINT || CMK_MESSAGE_LOGGING
    if (CmiGetArgInt(largv,"+wp",&num_workpes)) {
       CmiAssert(num_workpes <= *numNodes);
       total_pes = *numNodes;
       *numNodes = num_workpes;
    }
    else
       total_pes = num_workpes = *numNodes;
    if (*myNodeID == 0)
       CmiPrintf("Charm++> FT using %d processors and %d spare processors.\n", num_workpes, total_pes-num_workpes);
    petorank = (int *)malloc(sizeof(int) * num_workpes);
    for (i=0; i<num_workpes; i++)  petorank[i] = i;
    nextrank = num_workpes;

    if (*myNodeID >= num_workpes) {    /* is spare processor */
      // Previously, the code issued barriers here to bypass the isomalloc_sync sequence.
      // This fails to actually sync spare processors with the rest of the job.
      // Attempting to sync here fails because it cannot take place independently of the rest of ConverseCommonInit.
      // TODO: Refactor spare processor code to initialize more completely and only then wait.

      MPI_Status sts;
      int vals[2];
      MPI_Recv(vals,2,MPI_INT,MPI_ANY_SOURCE,FAIL_TAG, charmComm,&sts);
      int newpe = vals[0];
      CpvAccess(_curRestartPhase) = vals[1];
      
      CmiPrintf("Charm++> Spare MPI rank %d is activated for PE %d.\n", *myNodeID, newpe);

      if (newpe == -1) {
          MPI_Barrier(charmComm);
          //MPI_Barrier(charmComm);
          MPI_Finalize();
          exit(0);
      }

        /* update petorank */
      MPI_Recv(petorank, num_workpes, MPI_INT,MPI_ANY_SOURCE,FAIL_TAG,charmComm, &sts);
      nextrank = *myNodeID + 1;
      *myNodeID = newpe;
      myNID = newpe;

       /* add +restartaftercrash to argv */
      char *phase_str;
      char **restart_argv;
      int i=0;
      while(largv[i]!= NULL) i++;
      restart_argv = (char **)malloc(sizeof(char *)*(i+3));
      i=0;
      while(largv[i]!= NULL){
                restart_argv[i] = largv[i];
                i++;
      }
      static char s_restartaftercrash[] = "+restartaftercrash";
      restart_argv[i] = s_restartaftercrash;
      phase_str = (char*)malloc(10);
      sprintf(phase_str,"%d", CpvAccess(_curRestartPhase));
      restart_argv[i+1]=phase_str;
      restart_argv[i+2]=NULL;
      *argv = restart_argv;
      *argc = i+2;
      largc = *argc;
      largv = *argv;
    }
#endif

    idleblock = CmiGetArgFlag(largv, "+idleblocking");
    if (idleblock && _Cmi_mynode == 0 && !quietMode) {
        printf("Charm++: Running in idle blocking mode.\n");
    }

    /* setup signal handlers */
  if(!Cmi_truecrash) {
#if !defined(_WIN32)
    struct sigaction sa;
    sa.sa_handler = KillOnAllSigs;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGINT, &sa, &signal_int);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
#else
    signal(SIGSEGV, KillOnAllSigs);
    signal(SIGFPE, KillOnAllSigs);
    signal(SIGILL, KillOnAllSigs);
    signal_int = signal(SIGINT, KillOnAllSigs);
    signal(SIGTERM, KillOnAllSigs);
    signal(SIGABRT, KillOnAllSigs);
#endif
#if !defined(_WIN32) /*UNIX-only signals*/
    sigaction(SIGQUIT, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
#endif /*UNIX*/
  }

#if CMK_NO_OUTSTANDING_SENDS
    no_outstanding_sends=1;
#endif
    if (CmiGetArgFlag(largv,"+no_outstanding_sends")) {
        no_outstanding_sends = 1;
        if ((myNID == 0) && (!quietMode))
            printf("Charm++: Will%s consume outstanding sends in scheduler loop\n",
                   no_outstanding_sends?"":" not");
    }

    request_max=MAX_QLEN;
    CmiGetArgInt(largv,"+requestmax",&request_max);
    /*printf("request max=%d\n", request_max);*/

#if MPI_POST_RECV
    CmiGetArgInt(largv, "+postRecvCnt", &MPI_POST_RECV_COUNT);
    CmiGetArgInt(largv, "+postRecvLowerSize", &MPI_POST_RECV_LOWERSIZE);
    CmiGetArgInt(largv, "+postRecvUpperSize", &MPI_POST_RECV_UPPERSIZE);
    CmiGetArgInt(largv, "+postRecvThreshold", &MPI_POST_RECV_MSG_CNT_THRESHOLD);
    CmiGetArgInt(largv, "+postRecvBucketSize", &MPI_POST_RECV_INC);
    CmiGetArgInt(largv, "+postRecvMsgInc", &MPI_POST_RECV_MSG_INC);
    CmiGetArgInt(largv, "+postRecvCheckFreq", &MPI_POST_RECV_FREQ);
    if (MPI_POST_RECV_COUNT<=0) MPI_POST_RECV_COUNT=1;
    if (MPI_POST_RECV_LOWERSIZE>MPI_POST_RECV_UPPERSIZE) MPI_POST_RECV_UPPERSIZE = MPI_POST_RECV_LOWERSIZE;
    MPI_POST_RECV_SIZE = MPI_POST_RECV_UPPERSIZE;
    if ((myNID==0) && (!quietMode)) {
        printf("Charm++: using post-recv scheme with %d pre-posted recvs ranging from %d to %d (bytes) with msg count threshold %d and msg histogram bucket size %d, #buf increment every %d msgs. The buffers are checked every %d msgs\n",
               MPI_POST_RECV_COUNT, MPI_POST_RECV_LOWERSIZE, MPI_POST_RECV_UPPERSIZE,
               MPI_POST_RECV_MSG_CNT_THRESHOLD, MPI_POST_RECV_INC, MPI_POST_RECV_MSG_INC, MPI_POST_RECV_FREQ);
    }
#endif
	
#if USE_MPI_CTRLMSG_SCHEME
	CmiGetArgInt(largv, "+ctrlMsgCnt", &MPI_CTRL_MSG_CNT);
	if((myNID == 0) && (!quietMode)){
		printf("Charm++: using the alternative ctrl msg scheme with %d pre-posted ctrl msgs\n", MPI_CTRL_MSG_CNT);
	}
#endif

#if CMI_EXERT_SEND_CAP
    CmiGetArgInt(largv, "+dynCapSend", &SEND_CAP);
    if ((myNID==0) && (!quietMode)) {
        printf("Charm++: using static send cap %d\n", SEND_CAP);
    }
#endif
#if CMI_EXERT_RECV_CAP
    CmiGetArgInt(largv, "+dynCapRecv", &RECV_CAP);
    if ((myNID==0) && (!quietMode)) {
        printf("Charm++: using static recv cap %d\n", RECV_CAP);
    }
#endif
#if CMI_DYNAMIC_EXERT_CAP 
    CmiGetArgInt(largv, "+dynCapThreshold", &CMI_DYNAMIC_OUTGOING_THRESHOLD);
    CmiGetArgInt(largv, "+dynCapSend", &CMI_DYNAMIC_SEND_CAPSIZE);
    CmiGetArgInt(largv, "+dynCapRecv", &CMI_DYNAMIC_RECV_CAPSIZE);
    if ((myNID==0) && (!quietMode)) {
        printf("Charm++: using dynamic flow control with outgoing threshold %d, send cap %d, recv cap %d\n",
               CMI_DYNAMIC_OUTGOING_THRESHOLD, CMI_DYNAMIC_SEND_CAPSIZE, CMI_DYNAMIC_RECV_CAPSIZE);
    }
#endif

#if USE_ASYNC_RECV_FUNC
    CmiGetArgInt(largv, "+irecvMsgThreshold", &IRECV_MSG_THRESHOLD);
    if((myNID==0) && (!quietMode)) {
        printf("Charm++: for msg size larger than %d, MPI_Irecv is going to be used.\n", IRECV_MSG_THRESHOLD);
    }
#endif

    /* checksum flag */
    if (CmiGetArgFlag(largv,"+checksum")) {
#if CMK_ERROR_CHECKING
        checksum_flag = 1;
        if (myNID == 0) CmiPrintf("Charm++: CheckSum checking enabled! \n");
#else
        if (myNID == 0) CmiPrintf("Charm++: +checksum ignored in optimized version! \n");
#endif
    }

    procState = (ProcState *)malloc((_Cmi_mynodesize+1) * sizeof(ProcState));
    for (i=0; i<_Cmi_mynodesize+1; i++) {
#if MULTI_SENDQUEUE
        procState[i].postMsgBuf = PCQueueCreate();
#endif
        procState[i].recvLock = CmiCreateLock();
    }
#if CMK_SMP
#if !MULTI_SENDQUEUE
    postMsgBuf = PCQueueCreate();
    postMsgBufLock = CmiCreateLock();
#endif

#if CMK_ONESIDED_IMPL && CMK_SMP
    rdmaTagLock = CmiCreateLock();
#endif
#endif
}

INLINE_KEYWORD void LrtsNotifyIdle(void) {}

INLINE_KEYWORD void LrtsBeginIdle(void) {}

INLINE_KEYWORD void LrtsStillIdle(void) {}

void LrtsPreCommonInit(int everReturn) {

#if USE_MPI_CTRLMSG_SCHEME
	#if CMK_SMP
		if(CmiMyRank() == CmiMyNodeSize()) createCtrlMsgIrecvBufs();
	#else
		createCtrlMsgIrecvBufs();
	#endif
#elif MPI_POST_RECV
    int doInit = 1;
    int i;

#if CMK_SMP
    if (CmiMyRank() != CmiMyNodeSize()) doInit = 0;
#endif

    /* Currently, in mpi smp, the main thread will be the comm thread, so
     *	only the comm thread should post recvs. Cpvs, however, need to be
     * created on rank 0 (the ptrs to the actual cpv memory), while
     * other ranks are busy waiting for this to finish.	So cpv initialize
     * routines have to be called on every ranks, although they are only
     * useful on comm thread (whose rank is not zero) -Chao Mei
     */
    CpvInitialize(unsigned long long, Cmi_posted_recv_total);
    CpvInitialize(unsigned long long, Cmi_unposted_recv_total);
    CpvInitialize(MPI_Request*, CmiPostedRecvRequests);
    CpvInitialize(char **, CmiPostedRecvBuffers);

    CpvAccess(CmiPostedRecvRequests) = NULL;
    CpvAccess(CmiPostedRecvBuffers) = NULL;

    CpvInitialize(MPIPostRecvList *, postRecvListHdr);
    CpvInitialize(MPIPostRecvList *, curPostRecvPtr);
    CpvInitialize(int, msgRecvCnt);

    CpvAccess(postRecvListHdr) = NULL;
    CpvAccess(curPostRecvPtr) = NULL;
    CpvAccess(msgRecvCnt) = 0;

#if MPI_DYNAMIC_POST_RECV
    CpvInitialize(int *, MSG_HISTOGRAM_ARRAY);
#endif

    if (doInit) {
#if MPI_DYNAMIC_POST_RECV
        MSG_HISTOGRAM_BINSIZE = MPI_POST_RECV_INC;
        /* including two more buckets that are out of the range [LOWERSIZE, UPPERSIZE] */
        MAX_HISTOGRAM_BUCKETS = (MPI_POST_RECV_UPPERSIZE - MPI_POST_RECV_LOWERSIZE)/MSG_HISTOGRAM_BINSIZE+2;
        CpvAccess(MSG_HISTOGRAM_ARRAY) = (int *)malloc(sizeof(int)*MAX_HISTOGRAM_BUCKETS);
        memset(CpvAccess(MSG_HISTOGRAM_ARRAY), 0, sizeof(int)*MAX_HISTOGRAM_BUCKETS);
#else
        /* Post some extra recvs to help out with incoming messages */
        /* On some MPIs the messages are unexpected and thus slow */

        CpvAccess(postRecvListHdr) = (MPIPostRecvList *)malloc(sizeof(MPIPostRecvList));

        /* An array of request handles for posted recvs */
        CpvAccess(postRecvListHdr)->msgSizeIdx = -1;
        CpvAccess(postRecvListHdr)->bufCnt = MPI_POST_RECV_COUNT;
        CpvAccess(postRecvListHdr)->postedRecvReqs = (MPI_Request*)malloc(sizeof(MPI_Request)*MPI_POST_RECV_COUNT);
        /* An array of buffers for posted recvs */
        CpvAccess(postRecvListHdr)->postedRecvBufs = (char**)malloc(MPI_POST_RECV_COUNT*sizeof(char *));
        CpvAccess(postRecvListHdr)->next = CpvAccess(postRecvListHdr);
        CpvAccess(curPostRecvPtr) = CpvAccess(postRecvListHdr);

        /* Post Recvs */
        for (i=0; i<MPI_POST_RECV_COUNT; i++) {
            char *tmpbuf = (char *)CmiAlloc(MPI_POST_RECV_SIZE); /* Note: could be aligned allocation?? */
            CpvAccess(postRecvListHdr)->postedRecvBufs[i] = tmpbuf;
            if (MPI_SUCCESS != MPI_Irecv(tmpbuf,
                                         MPI_POST_RECV_SIZE,
                                         MPI_BYTE,
                                         MPI_ANY_SOURCE,
                                         POST_RECV_TAG,
                                         charmComm,
                                         CpvAccess(postRecvListHdr)->postedRecvReqs+i  ))
                CmiAbort("MPI_Irecv failed\n");
        }
#endif
    }
#endif /* end of MPI_POST_RECV  and USE_MPI_CTRLMSG_SCHEME */
	
#if CAPTURE_MSG_HISTOGRAM && !MPI_DYNAMIC_POST_RECV
    CpvInitialize(int *, MSG_HISTOGRAM_ARRAY);
    CpvAccess(MSG_HISTOGRAM_ARRAY) = (int *)malloc(sizeof(int)*MAX_HISTOGRAM_BUCKETS);
    memset(CpvAccess(MSG_HISTOGRAM_ARRAY), 0, sizeof(int)*MAX_HISTOGRAM_BUCKETS);
#endif

#if USE_ASYNC_RECV_FUNC || USE_MPI_CTRLMSG_SCHEME
#if CMK_SMP
    /* allocate the guardian entry only on comm thread considering NUMA */
    if(CmiMyRank() == CmiMyNodeSize()) {
        waitIrecvListHead = waitIrecvListTail = irecvListEntryAllocate();
        waitIrecvListHead->next = NULL;
    }
#else    
    waitIrecvListHead = waitIrecvListTail = irecvListEntryAllocate();
    waitIrecvListHead->next = NULL;
#endif
#endif
#if __FAULT__ 
    CpvInitialize(crashedRankList *, crashedRankHdr);
    CpvInitialize(crashedRankList *, crashedRankPtr);
    CpvAccess(crashedRankHdr) = NULL;
    CpvAccess(crashedRankPtr) = NULL;
#endif

    CpvInitialize(SMSG_LIST *, sent_msgs);
    CpvInitialize(SMSG_LIST *, end_sent);
    CpvInitialize(int, MsgQueueLen);
    CpvAccess(sent_msgs) = NULL;
    CpvAccess(end_sent) = NULL;
    CpvAccess(MsgQueueLen) = 0;
}

void LrtsPostCommonInit(int everReturn) {

#if CMI_MACH_TRACE_USEREVENTS && CMK_TRACE_ENABLED && !CMK_TRACE_IN_CHARM
    CpvInitialize(double, projTraceStart);
    /* only PE 0 needs to care about registration (to generate sts file). */
    if (CmiMyPe() == 0) {
        registerMachineUserEventsFunction(&registerMPITraceEvents);
    }
#endif

}
/* ######End of functions related with starting programs###### */

/***********************************************************************
 *
 * Abort function:
 *
 ************************************************************************/

void LrtsAbort(const char *message) {
    MPI_Abort(charmComm, 1);
    CMI_NORETURN_FUNCTION_END
}

/**************************  TIMER FUNCTIONS **************************/
#if CMK_TIMER_USE_SPECIAL

/* MPI calls are not threadsafe, even the timer on some machines */
static CmiNodeLock  timerLock = 0;
static int _absoluteTime = 0;
static double starttimer = 0; 
static int _is_global =0;

int CmiTimerIsSynchronized(void) {
    int  flag;
    void *v;

    /*  check if it using synchronized timer */
#if MPI_VERSION >= 2
    if (MPI_SUCCESS != MPI_Comm_get_attr(charmComm, MPI_WTIME_IS_GLOBAL, &v, &flag))
#else
    if (MPI_SUCCESS != MPI_Attr_get(charmComm, MPI_WTIME_IS_GLOBAL, &v, &flag))
#endif
        printf("MPI_WTIME_IS_GLOBAL not valid!\n");
    if (flag) {
        _is_global = *(int*)v;
        if (_is_global && CmiMyPe() == 0)
            printf("Charm++> MPI timer is synchronized\n");
    }
    return _is_global;
}

int CmiTimerAbsolute(void) {
    return _absoluteTime;
}

double CmiStartTimer(void) {
    return 0.0;
}

double CmiInitTime(void) {
    return starttimer;
}

void CmiTimerInit(char **argv) {
    _absoluteTime = CmiGetArgFlagDesc(argv,"+useAbsoluteTime", "Use system's absolute time as wallclock time.");
    if (_absoluteTime && CmiMyPe() == 0)
        printf("Charm++> absolute MPI timer is used\n");

#if ! CMK_MEM_CHECKPOINT && ! CMK_MESSAGE_LOGGING
    _is_global = CmiTimerIsSynchronized();
#else
    _is_global = 0;
#endif

    if (_is_global) {
        if (CmiMyRank() == 0) {
            double minTimer;
            starttimer = MPI_Wtime();

            MPI_Allreduce(&starttimer, &minTimer, 1, MPI_DOUBLE, MPI_MIN,
                          charmComm );
            starttimer = minTimer;
        }
    } else { /* we don't have a synchronous timer, set our own start time */
#if ! CMK_MEM_CHECKPOINT && ! CMK_MESSAGE_LOGGING
        CmiBarrier();
        CmiBarrier();
        CmiBarrier();
#endif
        starttimer = MPI_Wtime();
    }

#if 0 && CMK_SMP && CMK_MPI_INIT_THREAD
    if (CmiMyRank()==0 && _thread_provided == MPI_THREAD_SINGLE)
        timerLock = CmiCreateLock();
#endif
    CmiNodeAllBarrier();          /* for smp */
}

/**
 * Since the timerLock is never created, and is
 * always NULL, then all the if-condition inside
 * the timer functions could be disabled right
 * now in the case of SMP. --Chao Mei
 */
double CmiTimer(void) {
    double t;
#if 0 && CMK_SMP
    if (timerLock) CmiLock(timerLock);
#endif

    t = MPI_Wtime();

#if 0 && CMK_SMP
    if (timerLock) CmiUnlock(timerLock);
#endif

    return _absoluteTime?t: (t-starttimer);
}

double CmiWallTimer(void) {
    double t;
#if 0 && CMK_SMP
    if (timerLock) CmiLock(timerLock);
#endif

    t = MPI_Wtime();

#if 0 && CMK_SMP
    if (timerLock) CmiUnlock(timerLock);
#endif

    return _absoluteTime? t: (t-starttimer);
}

double CmiCpuTimer(void) {
    double t;
#if 0 && CMK_SMP
    if (timerLock) CmiLock(timerLock);
#endif
    t = MPI_Wtime() - starttimer;
#if 0 && CMK_SMP
    if (timerLock) CmiUnlock(timerLock);
#endif
    return t;
}

#endif     /* CMK_TIMER_USE_SPECIAL */

void LrtsBarrier(void)
{
    if (MPI_SUCCESS != MPI_Barrier(charmComm))
        CmiAbort("Timernit: MPI_Barrier failed!\n");
}

/* CmiBarrierZero make sure node 0 is the last one exiting the barrier */
int CmiBarrierZero(void) {
    int i;
    if (CmiMyRank() == 0)
    {
        char msg[1];
        MPI_Status sts;
        if (CmiMyNode() == 0)  {
            for (i=0; i<CmiNumNodes()-1; i++) {
                START_EVENT();
                if (MPI_SUCCESS != MPI_Recv(msg,1,MPI_BYTE,MPI_ANY_SOURCE,BARRIER_ZERO_TAG, charmComm,&sts))
                    CmiPrintf("MPI_Recv failed!\n");

                END_EVENT(30);
            }
        } else {
            START_EVENT();

            if (MPI_SUCCESS != MPI_Send((void *)msg,1,MPI_BYTE,0,BARRIER_ZERO_TAG,charmComm))
                printf("MPI_Send failed!\n");

            END_EVENT(20);
        }
    }
    CmiNodeAllBarrier();
    return 0;
}


#if CMK_MEM_CHECKPOINT || CMK_MESSAGE_LOGGING

void mpi_restart_crashed(int pe, int rank)
{
    int vals[2];
    vals[0] = CmiGetPeGlobal(pe,CmiMyPartition());
    vals[1] = CpvAccess(_curRestartPhase)+1;
    MPI_Send((void *)vals,2,MPI_INT,rank,FAIL_TAG,charmComm);
    MPI_Send(petorank, num_workpes, MPI_INT,rank,FAIL_TAG,charmComm);
}

/* notify spare processors to exit */
void mpi_end_spare(void)
{
    int i;
    for (i=nextrank; i<total_pes; i++) {
        int vals[2] = {-1,-1};
        MPI_Send((void *)vals,2,MPI_INT,i,FAIL_TAG,charmComm);
    }
}

int find_spare_mpirank(int _pe,int partition)
{
    if (nextrank == total_pes) {
      CmiAbort("Charm++> No spare processor available.");
    }
    int pe = CmiGetPeGlobal(_pe,partition);
    crashedRankList * crashedRank= (crashedRankList *)(malloc(sizeof(crashedRankList)));
    crashedRank->rank = petorank[pe];
    crashedRank->next=NULL;
    if(CpvAccess(crashedRankHdr)==NULL){
      CpvAccess(crashedRankHdr) = crashedRank;
      CpvAccess(crashedRankPtr) = CpvAccess(crashedRankHdr);
    }else{
      CpvAccess(crashedRankPtr)->next = crashedRank;
      CpvAccess(crashedRankPtr) = crashedRank;
    }
    petorank[pe] = nextrank;
    nextrank++;
    return nextrank-1;
}

int isRankDie(int rank){
  crashedRankList * cur = CpvAccess(crashedRankHdr);
  crashedRankList * head = CpvAccess(crashedRankHdr);
  while(cur!=NULL){
    if(rank == cur->rank){
      CpvAccess(crashedRankHdr) = head;
      return 1;
    }
    cur = cur->next;
  }
  CpvAccess(crashedRankHdr) = head;
  return 0;
}

void CkDieNow(void)
{
#if __FAULT__
    CmiPrintf("[%d] die now.\n", CmiMyPe());

      /* release old messages */
    while (!CheckAllAsyncMsgsSent()) {
        PumpMsgs();
        ReleasePostedMessages();
    }
    MPI_Barrier(charmComm);
 //   MPI_Barrier(charmComm);
    MPI_Finalize();
    exit(0);
#endif
}

#endif

/*======Beginning of Msg Histogram or Dynamic Post-Recv Related Funcs=====*/
#if CAPTURE_MSG_HISTOGRAM || MPI_DYNAMIC_POST_RECV
/* Functions related with capturing msg histogram */

#if MPI_DYNAMIC_POST_RECV
/* Consume all messages in the request buffers */
static void consumeAllMsgs(void)
{
    MPIPostRecvList *ptr = CpvAccess(curPostRecvPtr);
    if (ptr) {
        do {
            int i;
            for (i=0; i<ptr->bufCnt; i++) {
                int done = 0;
                MPI_Status sts;

                /* Indicating this entry has been tested before */
                if (ptr->postedRecvBufs[i] == NULL) continue;

                if (MPI_SUCCESS != MPI_Test(ptr->postedRecvReqs+i, &done, &sts))
                    CmiAbort("consumeAllMsgs failed in MPI_Test!\n");
                if (done) {
                    int nbytes;
                    char *msg;                    
                    
                    if (MPI_SUCCESS != MPI_Get_count(&sts, MPI_BYTE, &nbytes))
                        CmiAbort("consumeAllMsgs failed in MPI_Get_count!\n");
                    /* ready to handle this msg */
                    msg = (ptr->postedRecvBufs)[i];
                    (ptr->postedRecvBufs)[i] = NULL;
                    
                    handleOneRecvedMsg(nbytes, msg);
                } else {
                    if (MPI_SUCCESS != MPI_Cancel(ptr->postedRecvReqs+i))
                        CmiAbort("consumeAllMsgs failed in MPI_Cancel!\n");
                }
            }
            ptr = ptr->next;
        } while (ptr != CpvAccess(curPostRecvPtr));
    }
}

static void recordMsgHistogramInfo(int size)
{
    int idx = 0;
    size -= MPI_POST_RECV_LOWERSIZE;
    if (size > 0)
        idx = (size/MSG_HISTOGRAM_BINSIZE + 1);

    if (idx >= MAX_HISTOGRAM_BUCKETS) idx = MAX_HISTOGRAM_BUCKETS-1;
    CpvAccess(MSG_HISTOGRAM_ARRAY)[idx]++;
}

#define POST_RECV_USE_STATIC_PARAM 0
#define POST_RECV_REPORT_STS 0

#if POST_RECV_REPORT_STS
static int buildDynCallCnt = 0;
#endif

static void buildDynamicRecvBuffers(void)
{
    int i;

    int local_MSG_CNT_THRESHOLD;
    int local_MSG_INC;

#if POST_RECV_REPORT_STS
    buildDynCallCnt++;
#endif

    /* For debugging usage */
    reportMsgHistogramInfo();

    CpvAccess(msgRecvCnt) = 0;
    /* consume all outstanding msgs */
    consumeAllMsgs();

#if POST_RECV_USE_STATIC_PARAM
    local_MSG_CNT_THRESHOLD = MPI_POST_RECV_MSG_CNT_THRESHOLD;
    local_MSG_INC = MPI_POST_RECV_MSG_INC;
#else
    {
        int total = 0;
        int count = 0;
        for (i=1; i<MAX_HISTOGRAM_BUCKETS-1; i++) {
            int tmp = CpvAccess(MSG_HISTOGRAM_ARRAY)[i];
            /* avg is temporarily used for counting how many buckets are non-zero */
            if (tmp > 0)  {
                total += tmp;
                count++;
            }
        }
        if (count == 1) local_MSG_CNT_THRESHOLD = 1; /* Just filter out those zero-count msgs */
        else local_MSG_CNT_THRESHOLD = total / count /3; /* Catch >50% msgs NEED-BETTER-SCHEME HERE!!*/
        local_MSG_INC = total/count; /* Not having a good heuristic right now */
#if POST_RECV_REPORT_STS
        printf("sel_histo[%d]: critia_threshold=%d, critia_msginc=%d\n", CmiMyPe(), local_MSG_CNT_THRESHOLD, local_MSG_INC);
#endif
    }
#endif

    /* First continue to find the first msg range that requires post recv */
    /* Ignore the fist and the last one because they are not tracked */
    MPIPostRecvList *newHdr = NULL;
    MPIPostRecvList *newListPtr = newHdr;
    MPIPostRecvList *ptr = CpvAccess(postRecvListHdr);
    for (i=1; i<MAX_HISTOGRAM_BUCKETS-1; i++) {
        int count = CpvAccess(MSG_HISTOGRAM_ARRAY)[i];
        if (count >= local_MSG_CNT_THRESHOLD) {

#if POST_RECV_REPORT_STS
            /* Report histogram results */
            int low = (i-1)*MSG_HISTOGRAM_BINSIZE + MPI_POST_RECV_LOWERSIZE;
            int high = low + MSG_HISTOGRAM_BINSIZE;
            int reportCnt;
            if (count == local_MSG_CNT_THRESHOLD) reportCnt = 1;
            else reportCnt = (count - local_MSG_CNT_THRESHOLD)/local_MSG_INC + 1;
            printf("sel_histo[%d]-%d: msg size [%.2f, %.2f) with count=%d (%d)\n", CmiMyPe(), buildDynCallCnt, low/1000.0, high/1000.0, count, reportCnt);
#endif
            /* find if this msg idx exists, the "i" is the msgSizeIdx, in the current list */
            int notFound = 1;
            MPIPostRecvList *newEntry = NULL;
            while (ptr) {
                if (ptr->msgSizeIdx < i) {
                    /* free the buffer for this range of msg size */
                    MPIPostRecvList *nextptr = ptr->next;

                    free(ptr->postedRecvReqs);
                    int j;
                    for (j=0; j<ptr->bufCnt; j++) {
                        if ((ptr->postedRecvBufs)[j]) CmiFree((ptr->postedRecvBufs)[j]);
                    }
                    free(ptr->postedRecvBufs);
                    ptr = nextptr;
                } else if (ptr->msgSizeIdx == i) {
                    int newBufCnt, j;
                    int bufSize = i*MPI_POST_RECV_INC + MPI_POST_RECV_LOWERSIZE - 1;
                    newEntry = ptr;
                    /* Do some adjustment according to the current statistics */
                    if (count == local_MSG_CNT_THRESHOLD) newBufCnt = 1;
                    else newBufCnt = (count - local_MSG_CNT_THRESHOLD)/local_MSG_INC + 1;
                    if (newBufCnt != ptr->bufCnt) {
                        /* free old buffers, and allocate new buffers */
                        free(ptr->postedRecvReqs);
                        ptr->postedRecvReqs = (MPI_Request *)malloc(newBufCnt * sizeof(MPI_Request));
                        for (j=0; j<ptr->bufCnt; j++) {
                            if ((ptr->postedRecvBufs)[j]) CmiFree((ptr->postedRecvBufs)[j]);
                        }
                        free(ptr->postedRecvBufs);
                        ptr->postedRecvBufs = (char **)malloc(newBufCnt * sizeof(char *));
                    }

                    /* re-post those buffers */
                    ptr->bufCnt = newBufCnt;
                    for (j=0; j<ptr->bufCnt; j++) {
                        ptr->postedRecvBufs[j] = (char *)CmiAlloc(bufSize);
                        if (MPI_SUCCESS != MPI_Irecv(ptr->postedRecvBufs[j], bufSize, MPI_BYTE,
                                                     MPI_ANY_SOURCE, POST_RECV_TAG+ptr->msgSizeIdx,
                                                     charmComm, ptr->postedRecvReqs+j))
                            CmiAbort("MPI_Irecv failed in buildDynamicRecvBuffers!\n");
                    }

                    /* We already posted bufs for this range of msg size */
                    ptr = ptr->next;
                    /* Need to set ptr to NULL as the buf list comes to an end and the while loop exits */
                    if (ptr == CpvAccess(postRecvListHdr)) ptr = NULL;
                    notFound = 0;
                    break;
                } else {
                    /* The msgSizeIdx is larger than i */
                    break;
                }
                if (ptr == CpvAccess(postRecvListHdr)) {
                    ptr = NULL;
                    break;
                }
            } /* end while(ptr): iterating the posted recv buffer list */

            if (notFound) {
                /* the current range of msg size is not found in the list */
                int j;
                int bufSize = i*MPI_POST_RECV_INC + MPI_POST_RECV_LOWERSIZE - 1;
                newEntry = malloc(sizeof(MPIPostRecvList));
                MPIPostRecvList *one = newEntry;
                one->msgSizeIdx = i;
                if (count == local_MSG_CNT_THRESHOLD) one->bufCnt = 1;
                else one->bufCnt = (count - local_MSG_CNT_THRESHOLD)/local_MSG_INC + 1;
                one->postedRecvReqs = (MPI_Request *)malloc(sizeof(MPI_Request)*one->bufCnt);
                one->postedRecvBufs = (char **)malloc(one->bufCnt * sizeof(char *));
                for (j=0; j<one->bufCnt; j++) {
                    one->postedRecvBufs[j] = (char *)CmiAlloc(bufSize);
                    if (MPI_SUCCESS != MPI_Irecv(one->postedRecvBufs[j], bufSize, MPI_BYTE,
                                                 MPI_ANY_SOURCE, POST_RECV_TAG+one->msgSizeIdx,
                                                 charmComm, one->postedRecvReqs+j))
                        CmiAbort("MPI_Irecv failed in buildDynamicRecvBuffers!\n");
                }
            } /* end if notFound */

            /* Update the new list with the newEntry */
            CmiAssert(newEntry != NULL);
            if (newHdr == NULL) {
                newHdr = newEntry;
                newListPtr = newEntry;
                newHdr->next = newHdr;
            } else {
                newListPtr->next = newEntry;
                newListPtr = newEntry;
                newListPtr->next = newHdr;
            }
        } /* end if the count of this msg size range exceeds the threshold */
    } /* end for loop over the histogram buckets */

    /* Free remaining entries in the list */
    while (ptr) {
        /* free the buffer for this range of msg size */
        MPIPostRecvList *nextptr = ptr->next;

        free(ptr->postedRecvReqs);
        int j;
        for (j=0; j<ptr->bufCnt; j++) {
            if ((ptr->postedRecvBufs)[j]) CmiFree((ptr->postedRecvBufs)[j]);
        }
        free(ptr->postedRecvBufs);
        ptr = nextptr;
        if (ptr == CpvAccess(postRecvListHdr)) break;
    }

    CpvAccess(curPostRecvPtr) = CpvAccess(postRecvListHdr) = newHdr;
    memset(CpvAccess(MSG_HISTOGRAM_ARRAY), 0, sizeof(int)*MAX_HISTOGRAM_BUCKETS);
} /* end of function buildDynamicRecvBuffers */

static void examineMsgHistogramInfo(int size)
{
    int total = CpvAccess(msgRecvCnt)++;
    if (total < MPI_POST_RECV_FREQ) {
        recordMsgHistogramInfo(size);
    } else {
        buildDynamicRecvBuffers();
    }
}
#else
/* case when CAPTURE_MSG_HISTOGRAM is defined */
static void recordMsgHistogramInfo(int size)
{
    int idx = size/MSG_HISTOGRAM_BINSIZE;
    if (idx >= MAX_HISTOGRAM_BUCKETS) idx = MAX_HISTOGRAM_BUCKETS-1;
    CpvAccess(MSG_HISTOGRAM_ARRAY)[idx]++;
}
#endif /* end of MPI_DYNAMIC_POST_RECV */

void reportMsgHistogramInfo()
{
#if MPI_DYNAMIC_POST_RECV
    int i, count;
    count = CpvAccess(MSG_HISTOGRAM_ARRAY)[0];
    if (count > 0) {
        printf("msg_histo[%d]: %d for msg [0, %.2fK)\n", CmiMyNode(), count, MPI_POST_RECV_LOWERSIZE/1000.0);
    }
    for (i=1; i<MAX_HISTOGRAM_BUCKETS-1; i++) {
        int count = CpvAccess(MSG_HISTOGRAM_ARRAY)[i];
        if (count > 0) {
            int low = (i-1)*MSG_HISTOGRAM_BINSIZE + MPI_POST_RECV_LOWERSIZE;
            int high = low + MSG_HISTOGRAM_BINSIZE;
            printf("msg_histo[%d]: %d for msg [%.2fK, %.2fK)\n", CmiMyNode(), count, low/1000.0, high/1000.0);
        }
    }
    count = CpvAccess(MSG_HISTOGRAM_ARRAY)[MAX_HISTOGRAM_BUCKETS-1];
    if (count > 0) {
        printf("msg_histo[%d]: %d for msg [%.2fK, +inf)\n", CmiMyNode(), count, MPI_POST_RECV_UPPERSIZE/1000.0);
    }
#else
    int i;
    for (i=0; i<MAX_HISTOGRAM_BUCKETS; i++) {
        int count = CpvAccess(MSG_HISTOGRAM_ARRAY)[i];
        if (count > 0) {
            int low = i*MSG_HISTOGRAM_BINSIZE;
            int high = low + MSG_HISTOGRAM_BINSIZE;
            printf("msg_histo[%d]: %d for msg [%dK, %dK)\n", CmiMyNode(), count, low/1000, high/1000);
        }
    }
#endif
}
#endif /* end of CAPTURE_MSG_HISTOGRAM || MPI_DYNAMIC_POST_RECV */

void CmiSetupMachineRecvBuffersUser(void)
{
#if MPI_DYNAMIC_POST_RECV
    buildDynamicRecvBuffers();
#endif
}
/*=======End of Msg Histogram or Dynamic Post-Recv Related Funcs======*/


/*@}*/


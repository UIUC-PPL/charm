/** @file
 * Datagram implementation of Converse NET version
 * @ingroup NET
 * converse basic message header:<br>
 * d0 d1 d2 d3:  DgramHeader<br>
 * hdl:          handler<br>
 * xhdl:         extended handler<br>

  moved from machine.C by
  Orion Sky Lawlor, olawlor@acm.org, 7/25/2001
*/

/**
 * @addtogroup NET
 * @{
 */


#if CMK_USE_IBVERBS | CMK_USE_IBUD
#include <infiniband/verbs.h>
#endif

#define DGRAM_HEADER_SIZE 8

#define CmiMsgNext(msg) (*((void**)(msg)))

#define DGRAM_ROOTPE_MASK   (0xFFFFu)
#define DGRAM_SRCPE_MASK    (0xFFFF)
#define DGRAM_MAGIC_MASK    (0xFF)
#define DGRAM_SEQNO_MASK    (0xFFFFu)

#if CMK_NODE_QUEUE_AVAILABLE
#define DGRAM_NODEBROADCAST (0xFA)
//#define DGRAM_NODEMESSAGE   (0xFB)
#endif
#define DGRAM_DSTRANK_MAX   (0xFC)
#define DGRAM_SIMPLEKILL    (0xFD)
#define DGRAM_BROADCAST     (0xFE)
#define DGRAM_ACKNOWLEDGE   (0xFF)

/* DgramHeader overlays the first 4 fields of the converse CMK_MSG_HEADER_BASIC,
   defined in conv-common.h.  As such, its size and alignment are critical. */
typedef struct {
        unsigned int seqno:16;  /* seq number in send window */
        unsigned int srcpe:16;  /* CmiMyPe of the sender */
        unsigned int dstrank:8; /* rank of destination processor */
        unsigned int magic:8;   /* Low 8 bits of charmrun PID */
        unsigned int rootpe:16; /* broadcast root processor */
} DgramHeader;


/* the window size needs to be Cmi_window_size + sizeof(unsigned int) bytes) */
typedef struct { DgramHeader head; char window[1024]; } DgramAck;

unsigned char computeCheckSum(unsigned char *data, size_t len);

#define DgramHeaderMake(ptr, dstrank_, srcpe_, magic_, seqno_, root_) { \
   DgramHeader *header = (DgramHeader *)(ptr);	\
   header->seqno = seqno_; \
   header->srcpe = srcpe_; \
   header->dstrank = dstrank_; \
   header->magic = magic_ & DGRAM_MAGIC_MASK; \
   header->rootpe = root_; \
}

#define DgramHeaderBreak(ptr, dstrank_, srcpe_, magic_, seqno_, root_) { \
   DgramHeader *header = (DgramHeader *)(ptr);	\
   seqno_ = header->seqno; \
   srcpe_ = header->srcpe; \
   dstrank_ = header->dstrank; \
   magic_ = header->magic; \
   root_ = header->rootpe; \
}

#ifdef CMK_RANDOMLY_CORRUPT_MESSAGES
static void randomCorrupt(char *data, int len)
{
  if (0==(rand()%CMK_RANDOMLY_CORRUPT_MESSAGES))
  { /* insert one random bit flip into this message: */
    int badByte=rand()%len;
    int badBit=rand()%8;
    data[badByte]^=(1<<badBit);
  } 
}
#endif

#define PE_BROADCAST_OTHERS (-101)
#define PE_BROADCAST_ALL    (-102)

#if CMK_NODE_QUEUE_AVAILABLE
#define NODE_BROADCAST_OTHERS (-201)
#define NODE_BROADCAST_ALL    (-202)
#endif

/********* Startup and Command-line args ********/
static int    Cmi_max_dgram_size;
static int    Cmi_os_buffer_size;
static int    Cmi_window_size;
static int    Cmi_half_window;
static double Cmi_delay_retransmit;
static double Cmi_ack_delay;
static int    Cmi_dgram_max_data;
static int    Cmi_comm_periodic_delay;
static int    Cmi_comm_clock_delay;
static int writeableAcks,writeableDgrams;/*Write-queue counts (to know when to sleep)*/

static void setspeed_atm(void)
{
  Cmi_max_dgram_size   = 2048;
  Cmi_os_buffer_size   = 50000;
  Cmi_window_size      = 16;       /*20;*/
  Cmi_delay_retransmit = 0.0150;
  Cmi_ack_delay        = 0.0035;
}

static void setspeed_eth(void)
{
  Cmi_max_dgram_size   = 1400;
  Cmi_window_size      = 32;	    /*40*/
  Cmi_os_buffer_size   = Cmi_window_size*Cmi_max_dgram_size;
  Cmi_delay_retransmit = 0.0400;
  Cmi_ack_delay        = 0.0050;
}

static void setspeed_gigabit(void)
{
  /* for gigabit net */
  Cmi_max_dgram_size   = 9000;
  Cmi_window_size      = 8;
  Cmi_os_buffer_size   = 200000;
  Cmi_delay_retransmit = 0.020;
  Cmi_ack_delay        = 0.018;
}

static void extract_args(char **argv)
{
  int ms;
  setspeed_gigabit();
  if (CmiGetArgFlagDesc(argv,"+atm","Tune for a low-latency ATM network"))
    setspeed_atm();
  if (CmiGetArgFlagDesc(argv,"+eth","Tune for an ethernet network"))
    setspeed_eth();
  if (CmiGetArgFlagDesc(argv,"+giga","Tune for a gigabit network"))
    setspeed_gigabit();
  CmiGetArgIntDesc(argv,"+max_dgram_size",&Cmi_max_dgram_size,"Size of each UDP packet");
  CmiGetArgIntDesc(argv,"+window_size",&Cmi_window_size,"Number of unacknowledged packets");
  /* must divide for window protocol to work */
  if ( (DGRAM_SEQNO_MASK+1)%Cmi_window_size != 0)
    CmiAbort("Invalid window size!");
  CmiGetArgIntDesc(argv,"+os_buffer_size",&Cmi_os_buffer_size, "UDP socket's SO_RCVBUF/SO_SNDBUF");
  if (CmiGetArgIntDesc(argv,"+delay_retransmit",&ms, "Milliseconds to wait before retransmit"))
	  Cmi_delay_retransmit=0.001*ms;
  if (CmiGetArgIntDesc(argv,"+ack_delay",&ms, "Milliseconds to wait before ack'ing"))
	  Cmi_ack_delay=0.001*ms;
  extract_common_args(argv);
  Cmi_dgram_max_data = Cmi_max_dgram_size - DGRAM_HEADER_SIZE;
  Cmi_half_window = Cmi_window_size >> 1;
  if ((Cmi_window_size * Cmi_max_dgram_size) > Cmi_os_buffer_size)
    KillEveryone("Window size too big for OS buffer.");
  Cmi_comm_periodic_delay=(int)(1000*Cmi_delay_retransmit);
  if (Cmi_comm_periodic_delay>60) Cmi_comm_periodic_delay=60;
  Cmi_comm_clock_delay=(int)(1000*Cmi_ack_delay);
  if (sizeof(DgramHeader)!=DGRAM_HEADER_SIZE) {
    CmiAbort("DatagramHeader in machine-dgram.C is the wrong size!\n");
  }
}


/*****************************************************************************
 *
 * Communication Structures
 *
 *****************************************************************************/

typedef struct OutgoingMsgStruct
{
  struct OutgoingMsgStruct *next;
  int   src, dst;
  int   size;
  char *data;
  int   refcount;
  int   freemode;
}
*OutgoingMsg;

typedef struct ExplicitDgramStruct
{
  struct ExplicitDgramStruct *next;
  int  srcpe, rank, seqno, broot;
  unsigned int len, dummy; /* dummy to fix bug in rs6k alignment */
  double data[1];
}
*ExplicitDgram;

typedef struct ImplicitDgramStruct
{
  struct ImplicitDgramStruct *next;
  struct OtherNodeStruct *dest;
  int srcpe, rank, seqno, broot;
  char  *dataptr;
  int    datalen;
  OutgoingMsg ogm;
}
*ImplicitDgram;

struct PendingMsgStruct;


#if CMK_USE_IBUD
struct infiOtherNodeData;
struct infiOtherNodeData *initinfiData(int node,int lid,int qpn,int psn);
#endif
#if CMK_USE_IBVERBS
struct infiOtherNodeData;
struct infiOtherNodeData *initInfiOtherNodeData(int node,int addr[3]);
void	infiPostInitialRecvs(void);
#endif

typedef struct FutureMessageStruct {
  char *msg;
  int len;
} *FutureMessage;

typedef struct OtherNodeStruct
{
  int nodestart, nodesize;
  skt_ip_t IP;
  unsigned int mach_id;
  unsigned int dataport;
  struct sockaddr_in addr;

  unsigned int             send_last;    /* seqno of last dgram sent */
  ImplicitDgram           *send_window;  /* datagrams sent, not acked */
  ImplicitDgram            send_queue_h; /* head of send queue */
  ImplicitDgram            send_queue_t; /* tail of send queue */
  unsigned int             send_next;    /* next seqno to go into queue */
  unsigned int             send_good;    /* last acknowledged seqno */
  double                   send_primer;  /* time to send retransmit */
  unsigned int             send_ack_seqno; /* next ack seqno to send */
  int                      retransmit_leash; /*Maximum number of packets to retransmit*/

#if CMK_USE_IBVERBS | CMK_USE_IBUD
	struct infiOtherNodeData *infiData;
#endif

  int                      asm_rank;
  int                      asm_total;
  int                      asm_fill;
  char                    *asm_msg;
  
  int                      recv_ack_cnt; /* number of unacked dgrams */
  double                   recv_ack_time;/* time when ack should be sent */
  unsigned int             recv_expect;  /* next dgram to expect */
  ExplicitDgram           *recv_window;  /* Packets received, not integrated */
  int                      recv_winsz;   /* Number of packets in recv window */
  unsigned int             recv_next;    /* Seqno of first missing packet */
  unsigned int             recv_ack_seqno; /* last ack seqno received */

  unsigned int             stat_total_intr; /* Total Number of Interrupts */
  unsigned int             stat_proc_intr;  /* Processed Interrupts */
  unsigned int             stat_send_pkt;   /* number of packets sent */
  unsigned int             stat_resend_pkt; /* number of packets resent */
  unsigned int             stat_send_ack;   /* number of acks sent */
  unsigned int             stat_recv_pkt;   /* number of packets received */
  unsigned int             stat_recv_ack;   /* number of acks received */
  unsigned int             stat_ack_pkts;   /* packets acked */
  unsigned int             stat_consec_resend; /*Packets retransmitted since last ack*/ 

  int sent_msgs;
  int recd_msgs;
  int sent_bytes;
  int recd_bytes;
}
*OtherNode;

static void OtherNode_init(OtherNode node)
{
    int i;
    node->send_primer = 1.0e30; /*Don't retransmit until needed*/
    node->retransmit_leash = 1; /*Start with short leash*/
    node->send_last=0;
    node->send_window =
      (ImplicitDgram*)malloc(Cmi_window_size*sizeof(ImplicitDgram));
    for (i=0;i<Cmi_window_size;i++) node->send_window[i]=NULL;
    node->send_queue_h=node->send_queue_t=NULL;
    node->send_next=0;
    node->send_good=(unsigned int)(-1);
    node->send_ack_seqno=0;

    /*
    TODO: The initial values of the Ammasso related members will be set by the machine layer
          as the QPs are being created (along with any initial values).  After all the details
          of the layer are figured out, put some defaults here just so they are initialized to
          known values.  (Though, it should not be a problem that they are not initialized here yet.)
    */

    node->asm_rank=0;
    node->asm_total=0;
    node->asm_fill=0;
    node->asm_msg=0;
    
    node->recv_ack_cnt=0;
    node->recv_ack_time=1.0e30;
    node->recv_ack_seqno=0;
    node->recv_expect=0;
    node->recv_window =
      (ExplicitDgram*)malloc(Cmi_window_size*sizeof(ExplicitDgram));
    for (i=0;i<Cmi_window_size;i++) node->recv_window[i]=NULL;    
    node->recv_winsz=0;
    node->recv_next=0;

    node->stat_total_intr=0;
    node->stat_proc_intr=0;
    node->stat_send_pkt=0;
    node->stat_resend_pkt=0;
    node->stat_send_ack=0; 
    node->stat_recv_pkt=0;      
    node->stat_recv_ack=0;        
    node->stat_ack_pkts=0;

    node->sent_msgs = 0;
    node->recd_msgs = 0;
    node->sent_bytes = 0;
    node->recd_bytes = 0;
}

static OtherNode *nodes_by_pe;  /* OtherNodes indexed by processor number */
static OtherNode  nodes;        /* Indexed only by ``node number'' */

#ifdef CMK_USE_SPECIAL_MESSAGE_QUEUE_CHECK
/** Return 1 if our outgoing message queue 
   for this node is longer than this many bytes. */
int CmiLongSendQueue(int forNode,int longerThan) {
	int ret=0;
	ImplicitDgram dg;
	CmiCommLock();
	dg=nodes[forNode].send_queue_h;
	while (longerThan>0 && dg) {
		longerThan-=dg->datalen;
		dg=dg->next;
	}
	CmiCommUnlock();
	return ret;
}
#endif

extern void CmiGmConvertMachineID(unsigned int *mach_id);
extern void CmiAmmassoNodeAddressesStoreHandler(int pe, struct sockaddr_in *addr, int port);

/* initnode node table reply format:
 +------------------------------------------------------- 
 | 4 bytes  |   Number of nodes n                       ^
 |          |   (big-endian binary integer)       4+12*n bytes
 +-------------------------------------------------     |
 ^  |        (one entry for each node)            ^     |
 |  | 4 bytes  |   Number of PEs for this node    |     |
 n  | 4 bytes  |   IP address of this node   12*n bytes |
 |  | 4 bytes  |   Data (UDP) port of this node   |     |
 v  |          |   (big-endian binary integers)   v     v
 ---+----------------------------------------------------
*/
static void node_addresses_store(ChMessage *msg)
{
  ChMessageInt_t *n32=(ChMessageInt_t *)msg->data;
  ChNodeinfo *d=(ChNodeinfo *)(n32+ChInitNodetabFields);
  int nodestart;
  int i,j,n;
  MACHSTATE(1,"node_addresses_store {");	
  Lrts_numNodes=ChMessageInt(n32[0]);

  if ((sizeof(ChMessageInt_t)*ChInitNodetabFields+sizeof(ChNodeinfo)*Lrts_numNodes)
         !=(unsigned int)msg->len)
    {CmiPrintf("Node table has inconsistent length!\n");machine_exit(1);}

  nodes = (OtherNode)malloc(Lrts_numNodes * sizeof(struct OtherNodeStruct));
  nodestart=0;
  for (i=0; i<Lrts_numNodes; i++) {
    nodes[i].nodestart = nodestart;
    nodes[i].nodesize  = ChMessageInt(d[i].nPE);
    MACHSTATE2(3,"node %d nodesize %d",i,nodes[i].nodesize);
    nodes[i].mach_id = ChMessageInt(d[i].mach_id);

    nodes[i].IP=d[i].IP;
    if (i==Lrts_myNode) {
      Cmi_nodestart=nodes[i].nodestart;
      _Cmi_mynodesize=nodes[i].nodesize;
      Cmi_self_IP=nodes[i].IP;
    }

#if !CMK_USE_IBVERBS
    nodes[i].dataport = ChMessageInt(d[i].dataport);
    nodes[i].addr = skt_build_addr(nodes[i].IP,nodes[i].dataport);
#endif

    nodestart+=nodes[i].nodesize;

  }
  _Cmi_numpes=nodestart;
  n = _Cmi_numpes;
#ifdef CMK_CPV_IS_SMP
  n += Lrts_numNodes;
#endif
  nodes_by_pe = (OtherNode*)malloc(n * sizeof(OtherNode));
  _MEMCHECK(nodes_by_pe);
  for (i=0; i<Lrts_numNodes; i++) {
    OtherNode node = nodes + i;
    OtherNode_init(node);
    for (j=0; j<node->nodesize; j++)
      nodes_by_pe[j + node->nodestart] = node;
  }
#ifdef CMK_CPV_IS_SMP
  /* index for communication threads */
  for (i=_Cmi_numpes; i<_Cmi_numpes+Lrts_numNodes; i++) {
    OtherNode node = nodes + i-_Cmi_numpes;
    nodes_by_pe[i] = node;
  }
#endif
  MACHSTATE(1,"} node_addresses_store");
}

#if CMK_USE_IBVERBS || CMK_USE_IBUD
static void store_qpdata(ChMessage *msg)
{
  int i;
  const int qpDataSize = sizeof(ChInfiAddr) * Lrts_numNodes;

  if (qpDataSize != msg->len)
    {CmiPrintf("qpdata table has inconsistent length!");machine_exit(1);}

#if CMK_USE_IBVERBS
  ChInfiAddr *remoteInfiAddr = (ChInfiAddr *)msg->data;
#elif CMK_USE_IBUD
  ChInfiAddr *qp = (ChInfiAddr *)msg->data;
#endif

  for (i=0; i<Lrts_numNodes; i++)
  {
#if CMK_USE_IBVERBS
    if (i != Lrts_myNode)
    {
      int addr[3];
      addr[0] =ChMessageInt(remoteInfiAddr[i].lid);
      addr[1] =ChMessageInt(remoteInfiAddr[i].qpn);
      addr[2] =ChMessageInt(remoteInfiAddr[i].psn);
      nodes[i].infiData = initInfiOtherNodeData(i,addr);
    }
#elif CMK_USE_IBUD
    nodes[i].infiData=initinfiData(i,ChMessageInt(qp[i].lid),ChMessageInt(qp[i].qpn),ChMessageInt(qp[i].psn));
#endif
  }

#if CMK_USE_IBVERBS
  infiPostInitialRecvs();
#endif
}
#endif

/**
 * Printing Net Statistics -- milind
 */
static char statstr[10000];

void printNetStatistics(void)
{
  char tmpstr[1024];
  OtherNode myNode;
  int i;
  unsigned int send_pkt=0, resend_pkt=0, recv_pkt=0, send_ack=0;
  unsigned int recv_ack=0, ack_pkts=0;

  myNode = nodes+CmiMyNode();
  sprintf(tmpstr, "***********************************\n");
  strcpy(statstr, tmpstr);
  sprintf(tmpstr, "Net Statistics For Node %u\n", CmiMyNode());
  strcat(statstr, tmpstr);
  sprintf(tmpstr, "Interrupts: %u \tProcessed: %u\n",
                  myNode->stat_total_intr, myNode->stat_proc_intr);
  strcat(statstr, tmpstr);
  sprintf(tmpstr, "Total Msgs Sent: %u \tTotal Bytes Sent: %u\n",
                  myNode->sent_msgs, myNode->sent_bytes);
  strcat(statstr, tmpstr);
  sprintf(tmpstr, "Total Msgs Recv: %u \tTotal Bytes Recv: %u\n",
                  myNode->recd_msgs, myNode->recd_bytes);
  strcat(statstr, tmpstr);
  sprintf(tmpstr, "***********************************\n");
  strcat(statstr, tmpstr);
  sprintf(tmpstr, "[Num]\tSENDTO\tRESEND\tRECV\tACKSTO\tACKSFRM\tPKTACK\n");
  strcat(statstr,tmpstr);
  sprintf(tmpstr, "=====\t======\t======\t====\t======\t=======\t======\n");
  strcat(statstr,tmpstr);
  for(i=0;i<CmiNumNodes();i++) {
    OtherNode node = nodes+i;
    sprintf(tmpstr, "[%u]\t%u\t%u\t%u\t%u\t%u\t%u\n",
                     i, node->stat_send_pkt, node->stat_resend_pkt,
		     node->stat_recv_pkt, node->stat_send_ack,
		     node->stat_recv_ack, node->stat_ack_pkts);
    strcat(statstr, tmpstr);
    send_pkt += node->stat_send_pkt;
    recv_pkt += node->stat_recv_pkt;
    resend_pkt += node->stat_resend_pkt;
    send_ack += node->stat_send_ack;
    recv_ack += node->stat_recv_ack;
    ack_pkts += node->stat_ack_pkts;
  }
  sprintf(tmpstr, "[TOTAL]\t%u\t%u\t%u\t%u\t%u\t%u\n",
                     send_pkt, resend_pkt,
		     recv_pkt, send_ack,
		     recv_ack, ack_pkts);
  strcat(statstr, tmpstr);
  sprintf(tmpstr, "***********************************\n");
  strcat(statstr, tmpstr);
  CmiPrintf("%s", statstr);
}


/************** free list management *****************/

static ExplicitDgram Cmi_freelist_explicit;
static ImplicitDgram Cmi_freelist_implicit;
/*static OutgoingMsg   Cmi_freelist_outgoing;*/

#define FreeImplicitDgram(dg) {\
  ImplicitDgram d=(dg);\
  d->next = Cmi_freelist_implicit;\
  Cmi_freelist_implicit = d;\
}

#define MallocImplicitDgram(dg) {\
  ImplicitDgram d = Cmi_freelist_implicit;\
  if (d==0) {d = ((ImplicitDgram)malloc(sizeof(struct ImplicitDgramStruct)));\
             _MEMCHECK(d);\
  } else Cmi_freelist_implicit = d->next;\
  dg = d;\
}

#define FreeExplicitDgram(dg) {\
  ExplicitDgram d=(dg);\
  d->next = Cmi_freelist_explicit;\
  Cmi_freelist_explicit = d;\
}

#define MallocExplicitDgram(dg) {\
  ExplicitDgram d = Cmi_freelist_explicit;\
  if (d==0) { d = ((ExplicitDgram)malloc \
		   (sizeof(struct ExplicitDgramStruct) + Cmi_max_dgram_size));\
              _MEMCHECK(d);\
  } else Cmi_freelist_explicit = d->next;\
  dg = d;\
}

/* Careful with these next two, need concurrency control */

#define FreeOutgoingMsg(m) (free(m))
#define MallocOutgoingMsg(m)\
    {(m=(OutgoingMsg)malloc(sizeof(struct OutgoingMsgStruct))); _MEMCHECK(m);}

/****************************************************************************
 *                                                                          
 * CheckSocketsReady
 *
 * Checks both sockets to see which are readable and which are writeable.
 * We check all these things at the same time since this can be done for
 * free with ``select.'' The result is stored in global variables, since
 * this is essentially global state information and several routines need it.
 *
 ***************************************************************************/

static int ctrlskt_ready_read;
static int dataskt_ready_read;
static int dataskt_ready_write;

/******************************************************************************
 *
 * Transmission Code
 *
 *****************************************************************************/

void GarbageCollectMsg(OutgoingMsg ogm)
{
  MACHSTATE2(3,"GarbageCollectMsg called on ogm %p refcount %d",ogm,ogm->refcount);
	if (ogm->refcount == 0) {
    if (ogm->freemode == 'A') {
      ogm->freemode = 'X';
    } else {
      if (ogm->freemode != 'G') CmiFree(ogm->data);
      FreeOutgoingMsg(ogm);
    }
  }
}

void DiscardImplicitDgram(ImplicitDgram dg)
{
  OutgoingMsg ogm;
  ogm = dg->ogm;
  ogm->refcount--;
  GarbageCollectMsg(ogm);
  FreeImplicitDgram(dg);
}

/*
 Check the real-time clock and perform periodic tasks.
 Must be called with comm. lock held.
 */
static void CommunicationsClock(void)
{
  static double Cmi_ack_last, Cmi_check_last;

  MACHSTATE(1,"CommunicationsClock");
  Cmi_clock = GetClock();
  if (Cmi_clock > Cmi_ack_last + 0.5*Cmi_ack_delay) {
    MACHSTATE(2,"CommunicationsClock timing out acks");    
    Cmi_ack_last=Cmi_clock;
    writeableAcks=1;
    writeableDgrams=1;
  }
  
  if (Cmi_clock > Cmi_check_last + Cmi_check_delay) {
    MACHSTATE(4,"CommunicationsClock pinging charmrun");       
    Cmi_check_last = Cmi_clock; 
    ctrl_sendone_nolock("ping",NULL,0,NULL,0); /*Charmrun may have died*/
  }
}

#if CMK_SHARED_VARS_UNAVAILABLE
static void CommunicationsClockCaller(void *ignored)
{
  CmiCommLock();
  CommunicationsClock();
  CmiCommUnlock();
  CcdCallFnAfter((CcdVoidFn)CommunicationsClockCaller,NULL,Cmi_comm_clock_delay);  
}

static void CommunicationPeriodic(void) 
{ /*Poll on the communications server*/
  CommunicationServerNet(0, COMM_SERVER_FROM_SMP);
}

static void CommunicationPeriodicCaller(void *ignored)
{
  CommunicationPeriodic();
  CcdCallFnAfter((CcdVoidFn)CommunicationPeriodicCaller,NULL,Cmi_comm_periodic_delay);
}
#endif

/* common hardware dependent API */
/*void EnqueueOutgoingDgram(OutgoingMsg ogm, char *ptr, int dlen, OtherNode node, int rank, int broot);*/
void DeliverViaNetwork(OutgoingMsg ogm, OtherNode node, int rank, unsigned int broot, int copy);


#if CMK_USE_IBVERBS

#include "machine-ibverbs.C"

#elif CMK_USE_IBUD
#include "machine-ibud.C"

#endif


/*@}*/

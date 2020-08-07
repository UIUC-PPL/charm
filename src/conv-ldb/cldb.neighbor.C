#include <stdlib.h>

#include "converse.h"
#include "cldb.neighbor.h"
#include "queueing.h"
#include "cldb.h"
#include "topology.h"

#define USE_MULTICAST           0
#define IDLE_IMMEDIATE 		1
#define TRACE_USEREVENTS        1

#define PERIOD 20                /* default: 30 */
#define MAXOVERLOAD 1

static int  LBPeriod = PERIOD;                 /* time to call load balancing */
static int  overload_threshold = MAXOVERLOAD;

typedef struct CldProcInfo_s {
  double lastCheck;
  int    sent;			/* flag to disable idle work request */
  int    balanceEvt;		/* user event for balancing */
  int    updateLoadEvt; 
  int    idleEvt;		/* user event for idle balancing */
  int    idleprocEvt;		/* user event for processing idle req */
} *CldProcInfo;

extern char *_lbtopo;			/* topology name string */
int _lbsteal = 0;                       /* work stealing flag */

extern "C" void gengraph(int, int, int, int *, int *);

CpvStaticDeclare(CldProcInfo, CldData);
CpvStaticDeclare(int, CldLoadResponseHandlerIndex);
CpvStaticDeclare(int, CldAskLoadHandlerIndex);
CpvStaticDeclare(int, MinLoad);
CpvStaticDeclare(int, MinProc);
CpvStaticDeclare(int, Mindex);
CpvStaticDeclare(int, start);

#if ! USE_MULTICAST
CpvStaticDeclare(loadmsg *, msgpool);

static
#if CMK_C_INLINE
inline 
#endif
loadmsg *getPool(void){
  loadmsg *msg;
  if (CpvAccess(msgpool)!=NULL)  {
    msg = CpvAccess(msgpool);
    CpvAccess(msgpool) = msg->next;
  }
  else {
    msg = (loadmsg *)CmiAlloc(sizeof(loadmsg));
    CmiSetHandler(msg, CpvAccess(CldLoadResponseHandlerIndex));
  }
  return msg;
}

static
#if CMK_C_INLINE
inline 
#endif
void putPool(loadmsg *msg)
{
  msg->next = CpvAccess(msgpool);
  CpvAccess(msgpool) = msg;
}

#endif

void LoadNotifyFn(int l)
{
  CldProcInfo  cldData = CpvAccess(CldData);
  cldData->sent = 0;
}

const char *CldGetStrategy(void)
{
  return "neighbor";
}

/* since I am idle, ask for work from neighbors */
static void CldBeginIdle(void *dummy)
{
  CpvAccess(CldData)->lastCheck = CmiWallTimer();
}

static void CldEndIdle(void *dummy)
{
  CpvAccess(CldData)->lastCheck = -1;
}

static void CldStillIdle(void *dummy, double curT)
{
  int i;
  double startT;
  requestmsg msg;
  int myload;
  CldProcInfo  cldData = CpvAccess(CldData);

  double now = curT;
  double lt = cldData->lastCheck;
  /* only ask for work every 20ms */
  if (cldData->sent && (lt!=-1 && now-lt< PERIOD*0.001)) return;
  cldData->lastCheck = now;

  myload = CldCountTokens();
  if (myload > 0) return;

  msg.from_pe = CmiMyPe();
  CmiSetHandler(&msg, CpvAccess(CldAskLoadHandlerIndex));
#if CMK_IMMEDIATE_MSG && IDLE_IMMEDIATE
  /* fixme */
  CmiBecomeImmediate(&msg);
  for (i=0; i<CpvAccess(numNeighbors); i++) {
    msg.to_rank = CmiRankOf(CpvAccess(neighbors)[i].pe);
    CmiSyncNodeSend(CmiNodeOf(CpvAccess(neighbors)[i].pe),sizeof(requestmsg),(char *)&msg);
  }
#else
  msg.to_rank = -1;
  CmiSyncMulticast(CpvAccess(neighborGroup), sizeof(requestmsg), &msg);
#endif
  cldData->sent = 1;

#if CMK_TRACE_ENABLED && TRACE_USEREVENTS
  traceUserBracketEvent(cldData->idleEvt, now, CmiWallTimer());
#endif
}

/* immediate message handler, work at node level */
/* send some work to requested proc */
static void CldAskLoadHandler(requestmsg *msg)
{
  int receiver, rank, recvIdx, i;
  int myload = CldCountTokens();
  double now = CmiWallTimer();

  /* only give you work if I have more than 1 */
  if (myload>0) {
    int sendLoad;
    receiver = msg->from_pe;
    rank = CmiMyRank();
    if (msg->to_rank != -1) rank = msg->to_rank;
#if CMK_IMMEDIATE_MSG && IDLE_IMMEDIATE
    /* try the lock */
    if (CmiTryLock(CpvAccessOther(cldLock, rank))) {
      CmiDelayImmediate();		/* postpone immediate message */
      return;
    }
    CmiUnlock(CpvAccessOther(cldLock, rank));  /* release lock, grab later */
#endif
    sendLoad = myload / CpvAccess(numNeighbors) / 2;
    if (sendLoad < 1) sendLoad = 1;
    sendLoad = 1;
    for (i=0; i<CpvAccess(numNeighbors); i++) 
      if (CpvAccess(neighbors)[i].pe == receiver) break;
    
    if(i<CpvAccess(numNeighbors)) {CmiFree(msg); return;}   /* ? */
    CpvAccess(neighbors)[i].load += sendLoad;
    CldMultipleSend(receiver, sendLoad, rank, 0);
#if 0
#if CMK_TRACE_ENABLED && TRACE_USEREVENTS
    /* this is dangerous since projections logging is not thread safe */
    {
    CldProcInfo  cldData = CpvAccessOther(CldData, rank);
    traceUserBracketEvent(cldData->idleprocEvt, now, CmiWallTimer());
    }
#endif
#endif
  }
  CmiFree(msg);
}

/* balancing by exchanging load among neighbors */

void CldSendLoad(void)
{
#if CMK_MULTICORE
  /* directly send load to neighbors */
  double myload = CldCountTokens();
  int nNeighbors = CpvAccess(numNeighbors);
  int i;
  for (i=0; i<nNeighbors; i++) {
    int neighbor_pe = CpvAccess(neighbors)[i].pe;
    int j, found=0;
    for (j=0; j<CpvAccessOther(numNeighbors, neighbor_pe); j++)
      if (CpvAccessOther(neighbors, neighbor_pe)[j].pe == CmiMyPe())
      {
        CpvAccessOther(neighbors, neighbor_pe)[j].load = myload;
        found = 1;
        break;
      }
  }
#else
#if USE_MULTICAST
  loadmsg msg;

  msg.pe = CmiMyPe();
  msg.load = CldCountTokens();
  CmiSetHandler(&msg, CpvAccess(CldLoadResponseHandlerIndex));
  CmiSyncMulticast(CpvAccess(neighborGroup), sizeof(loadmsg), &msg);
  CpvAccess(CldLoadBalanceMessages) += CpvAccess(numNeighbors);
#else
  int i;
  int mype = CmiMyPe();
  int myload = CldCountTokens();
  for(i=0; i<CpvAccess(numNeighbors); i++) {
    loadmsg *msg = getPool();
    msg->fromindex = i;
    msg->toindex = CpvAccess(neighbors)[i].index;
    msg->pe = mype;
    msg->load = myload;
    CmiSyncSendAndFree(CpvAccess(neighbors)[i].pe, sizeof(loadmsg), msg);
  }
#endif
#endif
}

int CldMinAvg(void)
{
  int sum=0, i;
  int myload;

  int nNeighbors = CpvAccess(numNeighbors);
  if (CpvAccess(start) == -1)
    CpvAccess(start) = CmiMyPe() % nNeighbors;

#if 0
    /* update load from neighbors for multicore */
  for (i=0; i<nNeighbors; i++) {
    CpvAccess(neighbors)[i].load = CldLoadRank(CpvAccess(neighbors)[i].pe);
  }
#endif
  CpvAccess(MinProc) = CpvAccess(neighbors)[CpvAccess(start)].pe;
  CpvAccess(MinLoad) = CpvAccess(neighbors)[CpvAccess(start)].load;
  sum = CpvAccess(neighbors)[CpvAccess(start)].load;
  CpvAccess(Mindex) = CpvAccess(start);
  for (i=1; i<nNeighbors; i++) {
    CpvAccess(start) = (CpvAccess(start)+1) % nNeighbors;
    sum += CpvAccess(neighbors)[CpvAccess(start)].load;
    if (CpvAccess(MinLoad) > CpvAccess(neighbors)[CpvAccess(start)].load) {
      CpvAccess(MinLoad) = CpvAccess(neighbors)[CpvAccess(start)].load;
      CpvAccess(MinProc) = CpvAccess(neighbors)[CpvAccess(start)].pe;
      CpvAccess(Mindex) = CpvAccess(start);
    }
  }
  CpvAccess(start) = (CpvAccess(start)+2) % nNeighbors;
  myload = CldCountTokens();
  sum += myload;
  if (myload < CpvAccess(MinLoad)) {
    CpvAccess(MinLoad) = myload;
    CpvAccess(MinProc) = CmiMyPe();
  }
  i = (int)(1.0 + (((float)sum) /((float)(nNeighbors+1))));
  return i;
}

void CldBalance(void *dummy, double curT)
{
  int i, j, overload, numToMove=0, avgLoad;
  int totalUnderAvg=0, numUnderAvg=0, maxUnderAvg=0;

#if CMK_TRACE_ENABLED && TRACE_USEREVENTS
  double startT = curT;
#endif

/*CmiPrintf("[%d] CldBalance %f\n", CmiMyPe(), startT);*/
  avgLoad = CldMinAvg();
/*
  overload = CldLoad() - avgLoad;
  if (overload > CldCountTokens())
    overload = CldCountTokens();
*/
  overload = CldCountTokens() - avgLoad;

  if (overload > overload_threshold) {
    int nNeighbors = CpvAccess(numNeighbors);
    for (i=0; i<nNeighbors; i++)
      if (CpvAccess(neighbors)[i].load < avgLoad) {
        totalUnderAvg += avgLoad-CpvAccess(neighbors)[i].load;
        if (avgLoad - CpvAccess(neighbors)[i].load > maxUnderAvg)
          maxUnderAvg = avgLoad - CpvAccess(neighbors)[i].load;
        numUnderAvg++;
      }
    if (numUnderAvg > 0)  {
      int myrank = CmiMyRank();
      for (i=0; ((i<nNeighbors) && (overload>0)); i++) {
        j = (i+CpvAccess(Mindex))%CpvAccess(numNeighbors);
        if (CpvAccess(neighbors)[j].load < avgLoad) {
          numToMove = (avgLoad - CpvAccess(neighbors)[j].load);
          if (numToMove > overload)
              numToMove = overload;
          overload -= numToMove;
	  CpvAccess(neighbors)[j].load += numToMove;
#if CMK_MULTICORE || CMK_USE_IBVERBS
          CldSimpleMultipleSend(CpvAccess(neighbors)[j].pe, numToMove, myrank);
#else
          CldMultipleSend(CpvAccess(neighbors)[j].pe, 
			  numToMove, myrank, 
#if CMK_SMP
			  0
#else
			  1
#endif
                          );
#endif
        }
      }
    }             /* end of numUnderAvg > 0 */
  }
  CldSendLoad();
#if CMK_TRACE_ENABLED && TRACE_USEREVENTS
  traceUserBracketEvent(CpvAccess(CldData)->balanceEvt, startT, CmiWallTimer());
#endif
}

void CldBalancePeriod(void *dummy, double curT)
{
    CldBalance(NULL, curT);
    CcdCallFnAfterOnPE((CcdVoidFn)CldBalancePeriod, NULL, LBPeriod, CmiMyPe());
}


void CldLoadResponseHandler(loadmsg *msg)
{
  int i;
  
#if CMK_TRACE_ENABLED && TRACE_USEREVENTS
  double startT = CmiWallTimer();
#endif
#if USE_MULTICAST
  for(i=0; i<CpvAccess(numNeighbors); i++)
    if (CpvAccess(neighbors)[i].pe == msg->pe) {
      CpvAccess(neighbors)[i].load = msg->load;
      break;
    }
  CmiFree(msg);
#else
  int index = msg->toindex;
  if (index == -1) {
    for(i=0; i<CpvAccess(numNeighbors); i++)
      if (CpvAccess(neighbors)[i].pe == msg->pe) {
        index = i;
        break;
      }
  }
  if (index != -1) {    /* index can be -1, if neighbors table not init yet */
    CpvAccess(neighbors)[index].load = msg->load;
    if (CpvAccess(neighbors)[index].index == -1) CpvAccess(neighbors)[index].index = msg->fromindex;
  }
  putPool(msg);
#endif
#if CMK_TRACE_ENABLED && TRACE_USEREVENTS
  traceUserBracketEvent(CpvAccess(CldData)->updateLoadEvt, startT, CmiWallTimer());
#endif
}

void CldBalanceHandler(void *msg)
{
  CldRestoreHandler((char *)msg);
  CldPutToken((char *)msg);
}

void CldHandler(void *msg)
{
  CldInfoFn ifn; CldPackFn pfn;
  int queueing, priobits; size_t len; unsigned int *prioptr;
  
  CldRestoreHandler((char *)msg);
  ifn = (CldInfoFn)CmiHandlerToFunction(CmiGetInfo(msg));
  ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
  CsdEnqueueGeneral(msg, CQS_QUEUEING_LIFO, priobits, prioptr);
  /*CsdEnqueueGeneral(msg, queueing, priobits, prioptr);*/
}

void CldEnqueueGroup(CmiGroup grp, void *msg, int infofn)
{
  int queueing, priobits, i; size_t len; unsigned int *prioptr;
  CldInfoFn ifn = (CldInfoFn)CmiHandlerToFunction(infofn);
  CldPackFn pfn;
  ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
  if (pfn) {
    pfn(&msg);
    ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
  }
  CldSwitchHandler((char *)msg, CpvAccess(CldHandlerIndex));
  CmiSetInfo(msg,infofn);

  CmiSyncMulticastAndFree(grp, len, msg);
}

void CldEnqueueWithinNode(void *msg, int infofn)
{
  size_t len;
  int queueing, priobits;
  unsigned int *prioptr;
  CldPackFn pfn;
  CldInfoFn ifn = (CldInfoFn)CmiHandlerToFunction(infofn);
  ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);

  // If message is NOKEEP, do not pack it since its pointer is just going to
  // be shared with the other PEs on this node.
  if (pfn && !CMI_MSG_NOKEEP(msg)) {
    pfn(&msg);
    ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
  }
  CldSwitchHandler((char *)msg, CpvAccess(CldHandlerIndex));
  CmiSetInfo(msg,infofn);

  CmiWithinNodeBroadcast(len, (char *)msg);
}

void CldEnqueueMulti(int npes, const int *pes, void *msg, int infofn)
{
  int queueing, priobits, i; size_t len; unsigned int *prioptr;
  CldInfoFn ifn = (CldInfoFn)CmiHandlerToFunction(infofn);
  CldPackFn pfn;
  ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
  if (pfn) {
    pfn(&msg);
    ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
  }
  CldSwitchHandler((char *)msg, CpvAccess(CldHandlerIndex));
  CmiSetInfo(msg,infofn);
  /*
  for(i=0;i<npes;i++) {
    CmiSyncSend(pes[i], len, msg);
  }
  CmiFree(msg);
  */
  CmiSyncListSendAndFree(npes, pes, len, msg);
}

void CldEnqueue(int pe, void *msg, int infofn)
{
  int queueing, priobits, avg; size_t len; unsigned int *prioptr;
  CldInfoFn ifn = (CldInfoFn)CmiHandlerToFunction(infofn);
  CldPackFn pfn;

  if ((pe == CLD_ANYWHERE) && (CmiNumPes() > 1)) {
    avg = CldMinAvg();
    if (CldCountTokens() < avg)
      pe = CmiMyPe();
    else
      pe = CpvAccess(MinProc);
#if CMK_NODE_QUEUE_AVAILABLE
    if (CmiNodeOf(pe) == CmiMyNode()) {
      CldNodeEnqueue(CmiMyNode(), msg, infofn);
      return;
    }
#endif
    /* always pack the message because the message may be move away
       to a different processor later by CldGetToken() */
    ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
    if (pfn && CmiNumNodes()>1) {
       pfn(&msg);
       ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
    }
    if (pe != CmiMyPe()) {
      CpvAccess(neighbors)[CpvAccess(Mindex)].load++;
      CpvAccess(CldRelocatedMessages)++;
      CmiSetInfo(msg,infofn);
      CldSwitchHandler((char *)msg, CpvAccess(CldBalanceHandlerIndex));
      CmiSyncSendAndFree(pe, len, msg);
    }
    else {
      ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
      CmiSetInfo(msg,infofn);
      CldPutToken((char *)msg);
    }
  } 
  else if ((pe == CmiMyPe()) || (CmiNumPes() == 1)) {
    ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
    /*CmiSetInfo(msg,infofn);*/
    CsdEnqueueGeneral(msg, CQS_QUEUEING_LIFO, priobits, prioptr);
    /*CsdEnqueueGeneral(msg, queueing, priobits, prioptr);*/
  }
  else {
    ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
    if (pfn && CmiNodeOf(pe) != CmiMyNode()) {
      pfn(&msg);
      ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
    }
    CldSwitchHandler((char *)msg, CpvAccess(CldHandlerIndex));
    CmiSetInfo(msg,infofn);
    if (pe==CLD_BROADCAST) 
      CmiSyncBroadcastAndFree(len, msg);
    else if (pe==CLD_BROADCAST_ALL)
      CmiSyncBroadcastAllAndFree(len, msg);
    else CmiSyncSendAndFree(pe, len, msg);
  }
}

void CldNodeEnqueue(int node, void *msg, int infofn)
{
  int queueing, priobits, pe, avg; size_t len; unsigned int *prioptr;
  CldInfoFn ifn = (CldInfoFn)CmiHandlerToFunction(infofn);
  CldPackFn pfn;
  if ((node == CLD_ANYWHERE) && (CmiNumPes() > 1)) {
    avg = CldMinAvg();
    if (CldCountTokens() < avg)
      pe = CmiMyPe();
    else
      pe = CpvAccess(MinProc);
    node = CmiNodeOf(pe);
    if (node != CmiMyNode()){
        CpvAccess(neighbors)[CpvAccess(Mindex)].load++;
        CpvAccess(CldRelocatedMessages)++;
        ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
        if (pfn) {
            pfn(&msg);
            ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
        }
        CmiSetInfo(msg,infofn);
        CldSwitchHandler((char *)msg, CpvAccess(CldBalanceHandlerIndex));
        CmiSyncNodeSendAndFree(node, len, msg);
    }
    else {
      ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
      /* CmiSetInfo(msg,infofn);
       CldPutToken((char *)msg); */
      CsdNodeEnqueueGeneral(msg, queueing, priobits, prioptr);
    }
  }
  else if ((node == CmiMyNode()) || (CmiNumPes() == 1)) {
    ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
//    CmiSetInfo(msg,infofn);
    CsdNodeEnqueueGeneral(msg, queueing, priobits, prioptr);
  } 
  else {
    ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
    if (pfn) {
      pfn(&msg);
      ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
    }
    CldSwitchHandler((char *)msg, CpvAccess(CldHandlerIndex));
    CmiSetInfo(msg,infofn);
    if (node==CLD_BROADCAST) { CmiSyncNodeBroadcastAndFree(len, msg); }
    else if (node==CLD_BROADCAST_ALL){CmiSyncNodeBroadcastAllAndFree(len,msg);}
    else CmiSyncNodeSendAndFree(node, len, msg);
  }
}

void CldReadNeighborData(void)
{
  FILE *fp;
  char filename[25];
  int i, *pes;
  
  if (CmiNumPes() <= 1)
    return;
  sprintf(filename, "graph%d/graph%d", CmiNumPes(), CmiMyPe());
  if ((fp = fopen(filename, "r")) == 0) 
    {
      CmiError("Error opening graph init file on PE: %d\n", CmiMyPe());
      return;
    }
  if (fscanf(fp, "%d", &CpvAccess(numNeighbors)) != 1) {
    CmiAbort("CLD> reading neighbor data failed!");
  }
  CpvAccess(neighbors) = 
    (struct CldNeighborData_s *)calloc(CpvAccess(numNeighbors),
				     sizeof(struct CldNeighborData_s));
  pes = (int *)calloc(CpvAccess(numNeighbors), sizeof(int));
  for (i=0; i<CpvAccess(numNeighbors); i++) {
    if (fscanf(fp, "%d", &(CpvAccess(neighbors)[i].pe)) != 1) {
      CmiAbort("CLD> reading neighbor data failed!");
    }
    pes[i] = CpvAccess(neighbors)[i].pe;
    CpvAccess(neighbors)[i].load = 0;
  }
  fclose(fp);
  CpvAccess(neighborGroup) = CmiEstablishGroup(CpvAccess(numNeighbors), pes);
}

static void CldComputeNeighborData(void)
{
  int i, npes;
  int *pes;
  LBtopoFn topofn;
  void *topo;

  topofn = LBTopoLookup(_lbtopo);
  if (topofn == NULL) {
    CmiPrintf("SeedLB> Fatal error: Unknown topology: %s. Choose from:\n", _lbtopo);
    printoutTopo();
    CmiAbort("SeedLB> Fatal error: Unknown topology: %s", _lbtopo);
  }
  topo = topofn(CmiNumPes());
  npes = getTopoMaxNeighbors(topo);
  pes = (int *)malloc(npes*sizeof(int));
  getTopoNeighbors(topo, CmiMyPe(), pes, &npes);
#if 0
  {
  char buf[512], *ptr;
  sprintf(buf, "Neighors for PE %d (%d): ", CmiMyPe(), npes);
  ptr = buf + strlen(buf);
  for (i=0; i<npes; i++) {
    CmiAssert(pes[i] < CmiNumPes() && pes[i] != CmiMyPe());
    sprintf(ptr, " %d ", pes[i]);
    ptr += strlen(ptr);
  }
  strcat(ptr, "\n");
  CmiPrintf(buf);
  }
#endif

  CpvAccess(numNeighbors) = npes;
  CpvAccess(neighbors) = 
    (struct CldNeighborData_s *)calloc(npes, sizeof(struct CldNeighborData_s));
  for (i=0; i<npes; i++) {
    CpvAccess(neighbors)[i].pe = pes[i];
    CpvAccess(neighbors)[i].load = 0;
#if ! USE_MULTICAST
    CpvAccess(neighbors)[i].index = -1;
#endif
  }
  CpvAccess(neighborGroup) = CmiEstablishGroup(npes, pes);
  free(pes);
}

static void topo_callback(void)
{
  CldComputeNeighborData();
#if CMK_MULTICORE
  CmiNodeBarrier();
#endif
  CldBalancePeriod(NULL, CmiWallTimer());
}

void CldGraphModuleInit(char **argv)
{
  CpvInitialize(CldProcInfo, CldData);
  CpvInitialize(int, numNeighbors);
  CpvInitialize(int, MinLoad);
  CpvInitialize(int, Mindex);
  CpvInitialize(int, MinProc);
  CpvInitialize(int, start);
  CpvInitialize(CmiGroup, neighborGroup);
  CpvInitialize(CldNeighborData, neighbors);
  CpvInitialize(int, CldBalanceHandlerIndex);
  CpvInitialize(int, CldLoadResponseHandlerIndex);
  CpvInitialize(int, CldAskLoadHandlerIndex);

  CpvAccess(start) = -1;
  CpvAccess(CldData) = (CldProcInfo)CmiAlloc(sizeof(struct CldProcInfo_s));
  CpvAccess(CldData)->lastCheck = -1;
  CpvAccess(CldData)->sent = 0;
#if CMK_TRACE_ENABLED
  CpvAccess(CldData)->balanceEvt = traceRegisterUserEvent("CldBalance", -1);
  CpvAccess(CldData)->updateLoadEvt = traceRegisterUserEvent("UpdateLoad", -1);
  CpvAccess(CldData)->idleEvt = traceRegisterUserEvent("CldBalanceIdle", -1);
  CpvAccess(CldData)->idleprocEvt = traceRegisterUserEvent("CldBalanceProcIdle", -1);
#endif

  CpvAccess(MinLoad) = 0;
  CpvAccess(Mindex) = 0;
  CpvAccess(MinProc) = CmiMyPe();
  CpvAccess(CldBalanceHandlerIndex) = 
    CmiRegisterHandler(CldBalanceHandler);
  CpvAccess(CldLoadResponseHandlerIndex) = 
    CmiRegisterHandler((CmiHandler)CldLoadResponseHandler);
  CpvAccess(CldAskLoadHandlerIndex) = 
    CmiRegisterHandler((CmiHandler)CldAskLoadHandler);

  /* communication thread */
  if (CmiMyRank() == CmiMyNodeSize())  return;

  CmiGetArgStringDesc(argv, "+LBTopo", &_lbtopo, "define load balancing topology");
  if (CmiMyPe() == 0) CmiPrintf("Seed LB> Topology %s\n", _lbtopo);

  if (CmiNumPes() > 1) {
#if 0
    FILE *fp;
    char filename[20];
  
    sprintf(filename, "graph%d/graph%d", CmiNumPes(), CmiMyPe());
    if ((fp = fopen(filename, "r")) == 0)
      {
	if (CmiMyPe() == 0) {
	  CmiPrintf("No proper graph%d directory exists in current directory.\n Generating...  ", CmiNumPes());
	  gengraph(CmiNumPes(), (int)(sqrt(CmiNumPes())+0.5), 234);
	  CmiPrintf("done.\n");
	}
	else {
	  while (!(fp = fopen(filename, "r"))) ;
	  fclose(fp);
	}
      }
    else fclose(fp);
    CldReadNeighborData();
#endif
/* 
    CldComputeNeighborData();
#if CMK_MULTICORE
    CmiNodeBarrier();
#endif
    CldBalancePeriod(NULL, CmiWallTimer());
*/
    CcdCallOnCondition(CcdTOPOLOGY_AVAIL, (CcdVoidFn)topo_callback, NULL);

  }

  if (CmiGetArgIntDesc(argv, "+cldb_neighbor_period", &LBPeriod, "time interval to do neighbor seed lb")) {
    CmiAssert(LBPeriod>0);
    if (CmiMyPe() == 0) CmiPrintf("Seed LB> neighbor load balancing period is %d\n", LBPeriod);
  }
  if (CmiGetArgIntDesc(argv, "+cldb_neighbor_overload", &overload_threshold, "neighbor seed lb's overload threshold")) {
    CmiAssert(overload_threshold>0);
    if (CmiMyPe() == 0) CmiPrintf("Seed LB> neighbor overload threshold is %d\n", overload_threshold);
  }

#if 1
  _lbsteal = CmiGetArgFlagDesc(argv, "+workstealing", "Charm++> Enable work stealing at idle time");
  if (_lbsteal) {
  /* register idle handlers - when idle, keep asking work from neighbors */
  CcdCallOnConditionKeep(CcdPROCESSOR_BEGIN_IDLE,
      (CcdVoidFn) CldBeginIdle, NULL);
  CcdCallOnConditionKeep(CcdPROCESSOR_STILL_IDLE,
      (CcdVoidFn) CldStillIdle, NULL);
    if (CmiMyPe() == 0) 
      CmiPrintf("Charm++> Work stealing is enabled. \n");
  }
#endif
}


void CldModuleInit(char **argv)
{
  CpvInitialize(int, CldHandlerIndex);
  CpvInitialize(int, CldRelocatedMessages);
  CpvInitialize(int, CldLoadBalanceMessages);
  CpvInitialize(int, CldMessageChunks);
  CpvAccess(CldHandlerIndex) = CmiRegisterHandler(CldHandler);
  CpvAccess(CldRelocatedMessages) = CpvAccess(CldLoadBalanceMessages) = 
  CpvAccess(CldMessageChunks) = 0;

  CpvInitialize(loadmsg *, msgpool);
  CpvAccess(msgpool) = NULL;

  CldModuleGeneralInit(argv);
  CldGraphModuleInit(argv);

  CpvAccess(CldLoadNotify) = 1;
}

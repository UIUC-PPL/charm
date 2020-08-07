#include <stdlib.h>

#include "converse.h"
#include "cldb.workstealing.h"
#include "queueing.h"
#include "cldb.h"

#define TRACE_USEREVENTS        1
#define LOADTHRESH              3

void CldMultipleSendPrio(int pe, int numToSend, int rank, int immed);

typedef struct CldProcInfo_s {
  int    askEvt;		/* user event for askLoad */
  int    askNoEvt;		/* user event for askNoLoad */
  int    idleEvt;		/* user event for idle balancing */
} *CldProcInfo;

static int WS_Threshold = -1;
static int _steal_prio = 0;
static int _stealonly1 = 0;
static int _steal_immediate = 0;
static int workstealingproactive = 0;

CpvStaticDeclare(CldProcInfo, CldData);
CpvStaticDeclare(int, CldAskLoadHandlerIndex);
CpvStaticDeclare(int, CldAckNoTaskHandlerIndex);
CpvStaticDeclare(int, isStealing);


const char *CldGetStrategy(void)
{
  return "work stealing";
}


static void StealLoad(void)
{
  int i;
  double now;
  requestmsg msg;
  int  victim;
  int mype;
  int numpes;

  if (CpvAccess(isStealing)) return;    /* already stealing, return */

  CpvAccess(isStealing) = 1;

#if CMK_TRACE_ENABLED && TRACE_USEREVENTS
  now = CmiWallTimer();
#endif

  mype = CmiMyPe();
  msg.from_pe = mype;
  numpes = CmiNumPes();
  do{
      victim = (((CrnRand()+mype)&0x7FFFFFFF)%numpes);
  }while(victim == mype);

  CmiSetHandler(&msg, CpvAccess(CldAskLoadHandlerIndex));
#if CMK_IMMEDIATE_MSG
  /* fixme */
  if (_steal_immediate) CmiBecomeImmediate(&msg);
#endif
  /* msg.to_rank = CmiRankOf(victim); */
  msg.to_pe = victim;
  CmiSyncSend(victim, sizeof(requestmsg),(char *)&msg);
  
#if CMK_TRACE_ENABLED && TRACE_USEREVENTS
  traceUserBracketEvent(CpvAccess(CldData)->idleEvt, now, CmiWallTimer());
#endif
}

void LoadNotifyFn(int l)
{
    if(CldCountTokens() <= WS_Threshold)
        StealLoad();
}

/* since I am idle, ask for work from neighbors */
static void CldBeginIdle(void *dummy)
{
    //if (CldCountTokens() == 0) StealLoad();
    CmiAssert(CldCountTokens()==0);
    StealLoad();
}

/* immediate message handler, work at node level */
/* send some work to requested proc */
static void CldAskLoadHandler(requestmsg *msg)
{
  int receiver, rank, recvIdx, i;
  int myload, sendLoad;
  double now;
  /* int myload = CldLoad(); */

#if CMK_TRACE_ENABLED && TRACE_USEREVENTS
  now = CmiWallTimer();
#endif

  /* rank = msg->to_rank; */
  CmiAssert(msg->to_pe!=-1);
  rank = CmiRankOf(msg->to_pe);
  CmiAssert(rank!=-1);
  myload = CldCountTokensRank(rank);

  receiver = msg->from_pe;
  /* only give you work if I have more than 1 */
  if (myload>LOADTHRESH) {
      if(_stealonly1) sendLoad = 1;
      else sendLoad = myload/2; 
      if(sendLoad > 0) {
#if ! CMK_USE_IBVERBS
        if (_steal_prio)
          CldMultipleSendPrio(receiver, sendLoad, rank, 0);
        else
          CldMultipleSend(receiver, sendLoad, rank, 0);
#else
          CldSimpleMultipleSend(receiver, sendLoad, rank);
#endif
      }
      CmiFree(msg);
  }else
  {     /* send ack indicating there is no task */
      int pe = msg->to_pe;
      msg->to_pe = msg->from_pe;
      msg->from_pe = pe;
      /*msg->to_rank = CmiMyRank(); */

      CmiSetHandler(msg, CpvAccess(CldAckNoTaskHandlerIndex));
      CmiSyncSendAndFree(receiver, sizeof(requestmsg),(char *)msg);
  }

#if CMK_TRACE_ENABLED && TRACE_USEREVENTS
  traceUserBracketEvent(CpvAccess(CldData)->askEvt, now, CmiWallTimer());
#endif
}

void  CldAckNoTaskHandler(requestmsg *msg)
{
  double now;
  int victim; 
  /* int notaskpe = msg->from_pe; */
  int mype = CmiMyPe();
  int numpes = CmiNumPes();

#if CMK_TRACE_ENABLED && TRACE_USEREVENTS
  now = CmiWallTimer();
#endif

  do{
      /*victim = (((CrnRand()+notaskpe)&0x7FFFFFFF)%CmiNumPes());*/
      victim = (((CrnRand()+mype)&0x7FFFFFFF)%numpes);
  } while(victim == mype);

  /* reuse msg */
#if CMK_IMMEDIATE_MSG
  /* fixme */
  if (_steal_immediate) CmiBecomeImmediate(msg);
#endif
  /*msg->to_rank = CmiRankOf(victim); */
  msg->to_pe = victim;
  msg->from_pe = mype;
  CmiSetHandler(msg, CpvAccess(CldAskLoadHandlerIndex));
  CmiSyncSendAndFree(victim, sizeof(requestmsg),(char *)msg);

  CpvAccess(isStealing) = 1;

#if CMK_TRACE_ENABLED && TRACE_USEREVENTS
  traceUserBracketEvent(CpvAccess(CldData)->askNoEvt, now, CmiWallTimer());
#endif
}

void CldHandler(void *msg)
{
  CldInfoFn ifn; CldPackFn pfn;
  int queueing, priobits; size_t len; unsigned int *prioptr;
  
  CldRestoreHandler((char *)msg);
  ifn = (CldInfoFn)CmiHandlerToFunction(CmiGetInfo(msg));
  ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
  CsdEnqueueGeneral(msg, queueing, priobits, prioptr);
  /* CsdEnqueueGeneral(msg, CQS_QUEUEING_LIFO, priobits, prioptr); */
}

#define CldPUTTOKEN(msg)  \
           if (_steal_prio)   \
             CldPutTokenPrio(msg);   \
           else            \
             CldPutToken(msg);

void CldBalanceHandler(void *msg)
{
  CldRestoreHandler((char *)msg);
  CldPUTTOKEN((char *)msg);
  CpvAccess(isStealing) = 0;      /* fixme: this may not be right */
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
  CmiSyncListSendAndFree(npes, pes, len, msg);
}

void CldEnqueue(int pe, void *msg, int infofn)
{
  int queueing, priobits, avg; size_t len; unsigned int *prioptr;
  CldInfoFn ifn = (CldInfoFn)CmiHandlerToFunction(infofn);
  CldPackFn pfn;

  if ((pe == CLD_ANYWHERE) && (CmiNumPes() > 1)) {
      pe = CmiMyPe();
    /* always pack the message because the message may be move away
       to a different processor later by CldGetToken() */
    ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
    if (pfn && CmiNumNodes()>1) {
       pfn(&msg);
       ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
    }
    ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
    CmiSetInfo(msg,infofn);
    CldPUTTOKEN((char *)msg);
  } 
  else if ((pe == CmiMyPe()) || (CmiNumPes() == 1)) {
    ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
    CsdEnqueueGeneral(msg, queueing, priobits, prioptr);
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
      pe = CmiMyPe();
      node = CmiNodeOf(pe);
      ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
      CsdNodeEnqueueGeneral(msg, queueing, priobits, prioptr);
  }
  else if ((node == CmiMyNode()) || (CmiNumPes() == 1)) {
    ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
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


void CldGraphModuleInit(char **argv)
{
  CpvInitialize(CldProcInfo, CldData);
  CpvInitialize(int, CldAskLoadHandlerIndex);
  CpvInitialize(int, CldAckNoTaskHandlerIndex);
  CpvInitialize(int, CldBalanceHandlerIndex);

  CpvAccess(CldData) = (CldProcInfo)CmiAlloc(sizeof(struct CldProcInfo_s));
#if CMK_TRACE_ENABLED
  CpvAccess(CldData)->askEvt = traceRegisterUserEvent("CldAskLoad", -1);
  CpvAccess(CldData)->idleEvt = traceRegisterUserEvent("StealLoad", -1);
  CpvAccess(CldData)->askNoEvt = traceRegisterUserEvent("CldAckNoTask", -1);
#endif

  CpvAccess(CldBalanceHandlerIndex) = 
    CmiRegisterHandler(CldBalanceHandler);
  CpvAccess(CldAskLoadHandlerIndex) = 
    CmiRegisterHandler((CmiHandler)CldAskLoadHandler);
  
  CpvAccess(CldAckNoTaskHandlerIndex) = 
    CmiRegisterHandler((CmiHandler)CldAckNoTaskHandler);

  /* communication thread */
  if (CmiMyRank() == CmiMyNodeSize())  return;

  _stealonly1 = CmiGetArgFlagDesc(argv, "+stealonly1", "Charm++> Work Stealing, every time only steal 1 task");
 
  if(CmiGetArgIntDesc(argv, "+WSThreshold", &WS_Threshold, "The number of minimum load before stealing"))
  {
      CmiAssert(WS_Threshold>=0);
  }

  _steal_immediate = CmiGetArgFlagDesc(argv, "+WSImmediate", "Charm++> Work Stealing, steal using immediate messages");

  _steal_prio = CmiGetArgFlagDesc(argv, "+WSPriority", "Charm++> Work Stealing, using priority");

  /* register idle handlers - when idle, keep asking work from neighbors */
  if(CmiNumPes() > 1)
    CcdCallOnConditionKeep(CcdPROCESSOR_BEGIN_IDLE,
      (CcdVoidFn) CldBeginIdle, NULL);
  if(WS_Threshold >= 0 && CmiMyPe() == 0)
      CmiPrintf("Charm++> Steal work when load is fewer than %d. \n", WS_Threshold);
#if CMK_IMMEDIATE_MSG
  if(_steal_immediate && CmiMyPe() == 0)
      CmiPrintf("Charm++> Steal work using immediate messages. \n");
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

  CldModuleGeneralInit(argv);
  CldGraphModuleInit(argv);

  CpvAccess(CldLoadNotify) = 1;

  CpvInitialize(int, isStealing);
  CpvAccess(isStealing) = 0;
}

void CldCallback(void)
{}

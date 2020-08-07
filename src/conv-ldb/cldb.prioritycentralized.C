#include "converse.h"
#include "cldb.prioritycentralized.h"
#include "queueing.h"
#include "cldb.h"

#include "priorityqueue.C"

#define IDLE_IMMEDIATE 		1
#define TRACE_USEREVENTS        0

#define PERIOD 50                /* default: 30 */
#define MAXOVERLOAD 1

#define YH_DEBUG 0
#define THRESHOLD_LOAD 5
#define _U_INT_MAX 2000000000

#define LOAD_WEIGHT 0.1
#define PRIOR_WEIGHT 0.1

CpvDeclare(CldProcInfo, CldData);
extern char *_lbtopo;			/* topology name string */
int _lbsteal = 0;                       /* work stealing flag */

CpvDeclare(MsgHeap, CldManagerLoadQueue);
CpvDeclare(CldSlavePriorInfo*, CldSlavesPriorityQueue); //maintened in master to check which processor has which priority

CpvDeclare(int, CldAskLoadHandlerIndex);
CpvDeclare(int,  CldstorecharemsgHandlerIndex);
CpvDeclare(int, CldHigherPriorityComesHandlerIndex);
CpvDeclare(int, CldReadytoExecHandlerIndex);
CpvDeclare(void*, CldRequestQueue);

void LoadNotifyFn(int l)
{
  CldProcInfo  cldData = CpvAccess(CldData);
  cldData->sent = 0;
}

const char *CldGetStrategy(void)
{
  return "prioritycentralized";
}

void SendTasktoPe(int receiver, void *msg)
{
    CldInfoFn ifn; 
    CldPackFn pfn;
    int len, queueing, priobits, avg;
    unsigned int *prioptr;
    int old_load;
    int new_load;

    ifn = (CldInfoFn)CmiHandlerToFunction(CmiGetInfo(msg));
    ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
    CldRestoreHandler((char *)msg);
    CldSwitchHandler((char *)msg, CpvAccess(CldHigherPriorityComesHandlerIndex));
    CmiSyncSendAndFree(receiver, len, msg);

    old_load = CpvAccess(CldSlavesPriorityQueue)[receiver].load;
    new_load = old_load + 1;
    if(old_load == 0)
    {
        CpvAccess(CldSlavesPriorityQueue)[receiver].average_priority = *prioptr;
    }else
    {
        CpvAccess(CldSlavesPriorityQueue)[receiver].average_priority = CpvAccess(CldSlavesPriorityQueue)[receiver].average_priority/(new_load)*old_load + *prioptr/(new_load);
    }
    CpvAccess(CldSlavesPriorityQueue)[receiver].load = new_load;

#if YH_DEBUG
    CmiPrintf(" P%d====>P%d sending this msg with prior %u  to processor %d len=%d \n", CmiMyPe(), receiver, *prioptr, receiver, len);
#endif
}
/* master processor , what to do when receive a new msg from network */
static void CldStoreCharemsg(void *msg)
{
    /* insert the message into priority queue*/
    CldInfoFn ifn; 
    CldPackFn pfn;
    int len, queueing, priobits, avg;
    unsigned int *prioptr;
    priormsg *p_msg ;
    /* delay request msg */
    requestmsg *request_msg;
    int request_pe;
    void* loadmsg;

    /* check whether there is processor with lower priority, it exists, push this task to that processor */
    /* find the processor with the highest priority */
    int i=0; 
    int index = 1;
    double max_evaluation = 0;
    int old_load;
   
    ifn = (CldInfoFn)CmiHandlerToFunction(CmiGetInfo(msg));
    ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
#if YH_DEBUG
    CmiPrintf(" Step 2: on processor 0, Get new created msg and store it , PRIOR=%u Timer=%f\n", *prioptr, CmiTimer());
#endif
    //check which processor has underload and also the task priority is lower than the msg priority
    for(i=1; i<CmiNumPes();i++)
    {   //underload to avoid overflow
#if YH_DEBUG
        CmiPrintf(" processor %d has load num:%d\n", i, CpvAccess(CldSlavesPriorityQueue)[i].load);
#endif
        old_load = CpvAccess(CldSlavesPriorityQueue)[i].load;
        if(old_load == 0)
        {
            index = i;
            break;
        }
        double evaluation = (CpvAccess(CldSlavesPriorityQueue)[i].average_priority)* PRIOR_WEIGHT * (THRESHOLD_LOAD - CpvAccess(CldSlavesPriorityQueue)[i].load);
        if(evaluation > max_evaluation)
        {
            max_evaluation = evaluation;
            index = i;
        }
    }
    if(old_load == 0 || CpvAccess(CldSlavesPriorityQueue)[index].average_priority > *prioptr)
    {
        //send task to that processor
        SendTasktoPe(index, msg);
#if YH_DEBUG
        CmiPrintf(" Step 2-1: processor 0 send task to idle processor %d, msg prior=%u Timer=%f\n", index, *prioptr, CmiTimer());
#endif
        return;
    }

    p_msg = (priormsg*)malloc(sizeof(priormsg));
    p_msg->priority = *prioptr;
    p_msg->msg = msg;
    /*Lock here? */
    if(heap_isFull(&CpvAccess(CldManagerLoadQueue)))
    {
        CmiPrintf("Queue is already full, message will be lost\n");
    }
    else
        heap_addItem(&CpvAccess(CldManagerLoadQueue), p_msg);
#if YH_DEBUG
        CmiPrintf(" Step 2-3:  processor 0 , all processors are busy , store this msg  msg prior=%u Queuesize=%d Timer=%f\n", *prioptr, heap_size(&CpvAccess(CldManagerLoadQueue)), CmiTimer());
#endif

}
/* immediate message handler, work at node level */
/* send some work to requested proc */
static void CldAskLoadHandler(requestmsg *msg)
{
    /* pickup the msg with the highest priority */
    /* response to the requester chare */
    int receiver, rank, recvIdx, i;
    void* loadmsg;
    CldInfoFn ifn; 
    CldPackFn pfn;
    int len, queueing, priobits, avg; 
    unsigned int *prioptr;
    
    double old_load;
    double new_load;
    double old_average_prior;
    /* only give you work if I have more than 1 */
    receiver = msg->from_pe;
    old_load = CpvAccess(CldSlavesPriorityQueue)[receiver].load;
    old_average_prior = CpvAccess(CldSlavesPriorityQueue)[receiver].average_priority;
#if YH_DEBUG
    CmiPrintf(" Step 6 :%f %d<======= getrequest  from processor queue current size=%d, notidle=%d, load=%d\n", CmiTimer(), receiver, heap_size( &CpvAccess(CldManagerLoadQueue)), msg->notidle, CpvAccess(CldSlavesPriorityQueue)[receiver].load);
#endif
    if(!msg->notidle || old_load == 0 || old_load == 1)
    {
        CpvAccess(CldSlavesPriorityQueue)[receiver].average_priority = _U_INT_MAX;
        CpvAccess(CldSlavesPriorityQueue)[receiver].load = 0;
    }else
    {
        new_load = old_load - 1;
        CpvAccess(CldSlavesPriorityQueue)[receiver].load = new_load;
        CpvAccess(CldSlavesPriorityQueue)[receiver].average_priority = old_average_prior/new_load * old_load - msg->priority/new_load;
    }
   
    old_load = CpvAccess(CldSlavesPriorityQueue)[receiver].load;
    if(old_load < THRESHOLD_LOAD)
    {
        priormsg *p_msg = heap_extractMin(&CpvAccess(CldManagerLoadQueue));
        if(p_msg == 0)
        {
#if YH_DEBUG
            CmiPrintf(" Step 6-1 :%f Queue is empty no task %d<======= getrequest  from processor queue current size=%d\n", CmiTimer(), receiver, heap_size( &CpvAccess(CldManagerLoadQueue)));
#endif
        return;
        }
        
        loadmsg = p_msg->msg;
        SendTasktoPe(receiver, loadmsg);
    }
}

/***********************/
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
    if(CmiMyPe() == 0) 
    {
#if YH_DEBUG
        CmiPrintf(" Processor %d is idle, queue size=%d \n", CmiMyPe(), heap_size(&CpvAccess(CldManagerLoadQueue)) );
#endif
        return;
    }else
    {
#if YH_DEBUG
        CmiPrintf("Processor %d, has task number of %d\n", CmiMyPe(), CpvAccess(CldData)->load); 
#endif
    }

    int i;
    double startT;
    requestmsg msg;
    CldProcInfo  cldData = CpvAccess(CldData);
    double now = curT;
    double lt = cldData->lastCheck;
   
    cldData->load  = 0;
    msg.notidle = 0;
    if ((lt!=-1 && now-lt< PERIOD*0.001) ) return;
#if YH_DEBUG
    CmiPrintf("Step 1000: processor %d task is already zero ", CmiMyPe());
#endif

    cldData->lastCheck = now;
    msg.from_pe = CmiMyPe();
    CmiSetHandler(&msg, CpvAccess(CldAskLoadHandlerIndex));

    cldData->sent = 1;
#if YH_DEBUG
    CmiPrintf("Step 1000: processor %d task is already zero sentidle=%d", CmiMyPe(), (&msg)->notidle);
#endif
    CmiSyncSend(0, sizeof(requestmsg), &msg);
}
void CldReadytoExec(void *msg)
{

    CldProcInfo  cldData = CpvAccess(CldData);
    CldRestoreHandler((char *)msg);
    CmiHandleMessage(msg);
    cldData->load = cldData->load - 1;

    requestmsg r_msg;

    r_msg.notidle = 1;
    r_msg.from_pe = CmiMyPe();
    CmiSetHandler(&r_msg, CpvAccess(CldAskLoadHandlerIndex));
    CmiSyncSend(0, sizeof(requestmsg), &r_msg);

#if YH_DEBUG
    CmiPrintf(" Step final: message is handled on processor %d, task left=%d", CmiMyPe(), cldData->load);
#endif
}
void HigherPriorityWork(void *msg)
{
    //wrap this msg with token and put it into token queue
    
    CldInfoFn ifn;
    CldPackFn pfn;
    int len, queueing, priobits; 
    unsigned int *prioptr;
    CldProcInfo  cldData = CpvAccess(CldData);
    ifn = (CldInfoFn)CmiHandlerToFunction(CmiGetInfo(msg));
    ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
    CldRestoreHandler((char *)msg);
    CldSwitchHandler((char *)msg, CpvAccess(CldReadytoExecHandlerIndex));
    CsdEnqueueGeneral(msg, queueing, priobits, prioptr);
    cldData->load = cldData->load  + 1;

#if YH_DEBUG
    CmiPrintf(" Step 3:  processor %d, Task arrives and put it into charm++ queue, prior=%u Timer=%f\n", CmiMyPe(), *prioptr, CmiTimer());
#endif
}


void CldEnqueue(int pe, void *msg, int infofn)
{
  size_t len;
  int queueing, priobits;
  unsigned int *prioptr;
  CldInfoFn ifn = (CldInfoFn)CmiHandlerToFunction(infofn);
  CldPackFn pfn;
 
  ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
  CmiSetInfo(msg,infofn);
#if YH_DEBUG
  CmiPrintf(" Step 1: Creation New msg on pe %d priority=%u Timer:%f (msg len=%d)\n", CmiMyPe(),  *prioptr, CmiTimer(), len);
#endif

  if ((pe == CLD_ANYWHERE) && (CmiNumPes() > 1)) {
      pe = CmiMyPe();
    /* always pack the message because the message may be move away
       to a different processor later by CldGetToken() */
      CldSwitchHandler((char *)msg, CpvAccess(CldstorecharemsgHandlerIndex));
      if(pe == 0)
      {
          CldStoreCharemsg(msg);
      }else{
          if (pfn && CmiNumNodes()>1) {
              pfn(&msg);
              ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
          }
#if YH_DEBUG
          CmiPrintf(" Step 1-1: Creation New msg on pe%d ==> p0  priority=%u Timer:%f (msg len=%d)\n", CmiMyPe(),  *prioptr, CmiTimer(), len);
#endif

          CmiSyncSendAndFree(0, len, msg);
      }
  }else if((pe == CmiMyPe()) || (CmiNumPes() == 1) ) {
  
      CsdEnqueueGeneral(msg, CQS_QUEUEING_IFIFO, priobits, prioptr);
  }else {
      if (pfn && CmiNodeOf(pe) != CmiMyNode()) {
          pfn(&msg);
          ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
      }
      if (pe==CLD_BROADCAST) 
          CmiSyncBroadcastAndFree(len, msg);
      else if (pe==CLD_BROADCAST_ALL)
          CmiSyncBroadcastAllAndFree(len, msg);
      else CmiSyncSendAndFree(pe, len, msg);

  }
}

void CldHandler(char *msg)
{
  size_t len;
  int queueing, priobits;
  unsigned int *prioptr;
  CldInfoFn ifn; CldPackFn pfn;
  CldRestoreHandler(msg);
  ifn = (CldInfoFn)CmiHandlerToFunction(CmiGetInfo(msg));
  ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
  CsdEnqueueGeneral(msg, queueing, priobits, prioptr);
}

void CldEnqueueGroup(CmiGroup grp, void *msg, int infofn)
{
  size_t len;
  int queueing, priobits;
  unsigned int *prioptr;
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

void  CldOtherInit()
{

  CpvInitialize(CldProcInfo, CldData);
  CpvAccess(CldData) = (CldProcInfo)CmiAlloc(sizeof(struct CldProcInfo_s));
  CpvAccess(CldData)->lastCheck = -1;
  CpvAccess(CldData)->sent = 0;
  CpvAccess(CldData)->load = 0;
#if 1
  _lbsteal = 1;//CmiGetArgFlagDesc(argv, "+workstealing", "Charm++> Enable work stealing at idle time");
  if (_lbsteal) {
  /* register idle handlers - when idle, keep asking work from neighbors */
  CcdCallOnConditionKeep(CcdPROCESSOR_BEGIN_IDLE,
      (CcdVoidFn) CldBeginIdle, NULL);
  CcdCallOnConditionKeep(CcdPROCESSOR_STILL_IDLE,
      (CcdVoidFn) CldStillIdle, NULL);
  CcdCallOnConditionKeep(CcdPROCESSOR_END_IDLE,
      (CcdVoidFn) CldEndIdle, NULL);
    if (CmiMyPe() == 0) 
      CmiPrintf("Charm++> Work stealing is enabled. \n");
  }
#endif
    

  if (CmiMyPe() == 0){
      int numpes = CmiNumPes();
      CpvAccess(CldSlavesPriorityQueue) = (CldSlavePriorInfo*)CmiAlloc(sizeof(CldSlavePriorInfo) * numpes);
      int i=0;
      for(i=0; i<numpes; i++){
          CpvAccess(CldSlavesPriorityQueue)[i].average_priority = _U_INT_MAX;
          CpvAccess(CldSlavesPriorityQueue)[i].load = 0;
      }
  }
}

void CldModuleInit(char **argv)
{

  CpvInitialize(int, CldHandlerIndex);
  CpvAccess(CldHandlerIndex) = CmiRegisterHandler((CmiHandler)CldHandler);
  /* Yanhua */
  CpvInitialize(int, CldAskLoadHandlerIndex);
  CpvInitialize(int, CldstorecharemsgHandlerIndex);
  CpvInitialize(int, CldHigherPriorityComesHandlerIndex);
  CpvInitialize(int, CldReadytoExecHandlerIndex);
  CpvInitialize(MsgHeap, CldManagerLoadQueue);
  CpvInitialize(CldSlavePriorInfo*, CldSlavesPriorityQueue);
  CpvInitialize(void*, CldRequestQueue);

  CpvAccess(CldstorecharemsgHandlerIndex) = CmiRegisterHandler(CldStoreCharemsg);
  CpvAccess(CldHigherPriorityComesHandlerIndex) = CmiRegisterHandler(HigherPriorityWork);
  CpvAccess(CldAskLoadHandlerIndex) = CmiRegisterHandler((CmiHandler)CldAskLoadHandler);
  CpvAccess(CldReadytoExecHandlerIndex) = CmiRegisterHandler((CmiHandler)CldReadytoExec);
  CpvAccess(CldRequestQueue) = (void *)CqsCreate();
  CldModuleGeneralInit(argv);
  
  CldOtherInit();
  
  CpvAccess(CldLoadNotify) = 1;
  //CpvAccess(tokenqueue)->head->succ = CpvAccess(tokenqueue)->tail;

 
}



void CldNodeEnqueue(int node, void *msg, int infofn)
{
  size_t len;
  int queueing, priobits;
  unsigned int *prioptr;
  CldInfoFn ifn = (CldInfoFn)CmiHandlerToFunction(infofn);
  CldPackFn pfn;
  if (node == CLD_ANYWHERE) {
    node = (((CrnRand()+CmiMyNode())&0x7FFFFFFF)%CmiNumNodes());
    if (node != CmiMyNode())
      CpvAccess(CldRelocatedMessages)++;
  }
  if (node == CmiMyNode() && !CmiImmIsRunning()) {
    ifn(msg, &pfn, &len, &queueing, &priobits, &prioptr);
    CsdNodeEnqueueGeneral(msg, queueing, priobits, prioptr);
  } else {
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
  size_t len;
  int queueing, priobits;
  unsigned int *prioptr;
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


void CldCallback()
{}

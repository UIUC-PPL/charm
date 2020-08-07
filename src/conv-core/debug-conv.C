/*
Converse-level debugger support

Collected from convcore.C, conv-ccs.C, register.C by
Orion Sky Lawlor, olawlor@acm.org, 4/10/2001
 */
#include <stdio.h> /*for sscanf*/
#include <string.h> /*for strcmp*/
#include "converse.h"
#include "conv-trace.h"
#include "queueing.h"
#include "conv-ccs.h"
#include <errno.h>

#ifdef _WIN32
# include <io.h>
# define write _write
#endif

CpvCExtern(int, freezeModeFlag);
CpvStaticDeclare(int, continueFlag);
CpvStaticDeclare(int, stepFlag);
CpvCExtern(void *, debugQueue);
CpvDeclare(void*, conditionalQueue);
int conditionalPipe[2] = {0, 0};
int _debugHandlerIdx;

char ** memoryBackup;

/** Specify if we are replaying the processor from message logs, thus disable delivering of messages */
extern int _replaySystem;
int _replaySystem = 0;
int _conditionalDelivery = 0;

#undef ConverseDeliver
int ConverseDeliver(int pe) {
  return !_replaySystem && (!_conditionalDelivery || pe==CmiMyPe());
}

#if ! CMK_HAS_NTOHL
#ifndef _WIN32
uint32_t ntohl(uint32_t netlong) {
#else
u_long ntohl(u_long netlong) {
#endif
  union { uint32_t i; unsigned char c[4]; } uaw;
  uaw.i = netlong;
  netlong = uaw.c[0]<<24 + uaw.c[1]<<16 + uaw.c[2]<<8 + uaw.c[3];
  return netlong;
}
#else
#if defined _WIN32
#include <winsock.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif
#endif

/***************************************************
  The CCS interface to the debugger
*/

#include <string.h>

#include "pup_c.h"

void check_memory_leaks(LeakSearchInfo *info);

CpvDeclare(int, CpdSearchLeaks_Index);
CpvDeclare(int, CpdSearchLeaksDone_Index);
CpvStaticDeclare(CcsDelayedReply, leakSearchDelayedReply);

void CpdSearchLeaksDone(void *msg) {
  CmiInt4 ok = 1;
  CcsSendDelayedReply(CpvAccess(leakSearchDelayedReply), 4, &ok);
  CmiFree(msg);
}

void CpdSearchLeaks(char * msg) {
  LeakSearchInfo *info = (LeakSearchInfo *)(msg+CmiMsgHeaderSizeBytes);
  if (CmiMyPe() == info->pe || (info->pe == -1 && CmiMyPe() == 0)) {
#if CMK_64BIT
      info->begin_data = (char*)(
      (((CmiUInt8)ntohl(((int*)&info->begin_data)[0]))<<32) + ntohl(((int*)&info->begin_data)[1]));
      info->end_data = (char*)(
      (((CmiUInt8)ntohl(((int*)&info->end_data)[0]))<<32) + ntohl(((int*)&info->end_data)[1]));
      info->begin_bss = (char*)(
      (((CmiUInt8)ntohl(((int*)&info->begin_bss)[0]))<<32) + ntohl(((int*)&info->begin_bss)[1]));
      info->end_bss = (char*)(
      (((CmiUInt8)ntohl(((int*)&info->end_bss)[0]))<<32) + ntohl(((int*)&info->end_bss)[1]));
#else
      info->begin_data = (char*)(ntohl((int)info->begin_data));
      info->end_data = (char*)(ntohl((int)info->end_data));
      info->begin_bss = (char*)(ntohl((int)info->begin_bss));
      info->end_bss = (char*)(ntohl((int)info->end_bss));
#endif
    info->quick = ntohl(info->quick);
    info->pe = ntohl(info->pe);
    CpvAccess(leakSearchDelayedReply) = CcsDelayReply();
    if (info->pe == -1) {
      CmiSetXHandler(msg, CpvAccess(CpdSearchLeaks_Index));
      CmiSetHandler(msg, _debugHandlerIdx);
      CmiSyncBroadcast(CmiMsgHeaderSizeBytes+sizeof(LeakSearchInfo), msg);
    }
  }
  check_memory_leaks(info);
  if (info->pe == CmiMyPe()) CpdSearchLeaksDone(msg);
  else if (info->pe == -1) {
    void *reduceMsg = CmiAlloc(0);
    CmiSetHandler(reduceMsg, CpvAccess(CpdSearchLeaksDone_Index));
    CmiReduce(reduceMsg, CmiMsgHeaderSizeBytes, CmiReduceMergeFn_random);
    CmiFree(msg);
  }
  else CmiAbort("Received allocationTree request for another PE!");
}

void * (*CpdDebugGetAllocationTree)(int *) = NULL;
void (*CpdDebug_pupAllocationPoint)(pup_er p, void *data) = NULL;
void (*CpdDebug_deleteAllocationPoint)(void *ptr) = NULL;
void * (*CpdDebug_MergeAllocationTree)(size_t *size, void *data, void **remoteData, int numRemote) = NULL;
CpvDeclare(int, CpdDebugCallAllocationTree_Index);
CpvStaticDeclare(CcsDelayedReply, allocationTreeDelayedReply);

static void CpdDebugReturnAllocationTree(void *tree) {
  pup_er sizer = pup_new_sizer();
  char *buf;
  pup_er packer;
  int i;
  CpdDebug_pupAllocationPoint(sizer, tree);
  buf = (char *)malloc(pup_size(sizer));
  packer = pup_new_toMem(buf);
  CpdDebug_pupAllocationPoint(packer, tree);
  /*CmiPrintf("size=%d tree:",pup_size(sizer));
  for (i=0;i<100;++i) CmiPrintf(" %02x",((unsigned char*)buf)[i]);
  CmiPrintf("\n");*/
  CcsSendDelayedReply(CpvAccess(allocationTreeDelayedReply), pup_size(sizer),buf);
  pup_destroy(sizer);
  pup_destroy(packer);
  free(buf);
}

static void CpdDebugCallAllocationTree(char *msg)
{
  int numNodes;
  int forPE;
  void *tree;
  if (CpdDebugGetAllocationTree == NULL) {
    CmiPrintf("Error> Invoked CpdDebugCalloAllocationTree but no function initialized.\nDid you forget to link in memory charmdebug?\n");
    CcsSendReply(0, NULL);
    return;
  }
  sscanf(msg+CmiMsgHeaderSizeBytes, "%d", &forPE);
  if (CmiMyPe() == forPE) CpvAccess(allocationTreeDelayedReply) = CcsDelayReply();
  if (forPE == -1 && CmiMyPe()==0) {
    CpvAccess(allocationTreeDelayedReply) = CcsDelayReply();
    CmiSetXHandler(msg, CpvAccess(CpdDebugCallAllocationTree_Index));
    CmiSetHandler(msg, _debugHandlerIdx);
    CmiSyncBroadcast(CmiMsgHeaderSizeBytes+strlen(msg+CmiMsgHeaderSizeBytes)+1, msg);
  }
  tree = CpdDebugGetAllocationTree(&numNodes);
  if (forPE == CmiMyPe()) CpdDebugReturnAllocationTree(tree);
  else if (forPE == -1) CmiReduceStruct(tree, CpdDebug_pupAllocationPoint, CpdDebug_MergeAllocationTree,
                                CpdDebugReturnAllocationTree, CpdDebug_deleteAllocationPoint);
  else CmiAbort("Received allocationTree request for another PE!");
  CmiFree(msg);
}

void * (*CpdDebugGetMemStat)(void) = NULL;
void (*CpdDebug_pupMemStat)(pup_er p, void *data) = NULL;
void (*CpdDebug_deleteMemStat)(void *ptr) = NULL;
void * (*CpdDebug_mergeMemStat)(size_t *size, void *data, void **remoteData, int numRemote) = NULL;
CpvDeclare(int, CpdDebugCallMemStat_Index);
CpvStaticDeclare(CcsDelayedReply, memStatDelayedReply);

static void CpdDebugReturnMemStat(void *stat) {
#if CMK_CCS_AVAILABLE
  pup_er sizerNet = pup_new_network_sizer();
  pup_er sizer = pup_new_fmt(sizerNet);
  char *buf;
  pup_er packerNet;
  pup_er packer;
  int i;
  CpdDebug_pupMemStat(sizer, stat);
  buf = (char *)malloc(pup_size(sizer));
  packerNet = pup_new_network_pack(buf);
  packer = pup_new_fmt(packerNet);
  CpdDebug_pupMemStat(packer, stat);
  /*CmiPrintf("size=%d tree:",pup_size(sizer));
  for (i=0;i<100;++i) CmiPrintf(" %02x",((unsigned char*)buf)[i]);
  CmiPrintf("\n");*/
  CcsSendDelayedReply(CpvAccess(memStatDelayedReply), pup_size(sizer),buf);
  pup_destroy(sizerNet);
  pup_destroy(sizer);
  pup_destroy(packerNet);
  pup_destroy(packer);
  free(buf);
#endif
}

static void CpdDebugCallMemStat(char *msg) {
  int forPE;
  void *stat;
  if (CpdDebugGetMemStat == NULL) {
    CmiPrintf("Error> Invoked CpdDebugCalloMemStat but no function initialized.\nDid you forget to link in memory charmdebug?\n");
    CcsSendReply(0, NULL);
    return;
  }
  sscanf(msg+CmiMsgHeaderSizeBytes, "%d", &forPE);
  if (CmiMyPe() == forPE) CpvAccess(memStatDelayedReply) = CcsDelayReply();
  if (forPE == -1 && CmiMyPe()==0) {
    CpvAccess(memStatDelayedReply) = CcsDelayReply();
    CmiSetXHandler(msg, CpvAccess(CpdDebugCallMemStat_Index));
    CmiSetHandler(msg, _debugHandlerIdx);
    CmiSyncBroadcast(CmiMsgHeaderSizeBytes+strlen(msg+CmiMsgHeaderSizeBytes)+1, msg);
  }
  stat = CpdDebugGetMemStat();
  if (forPE == CmiMyPe()) CpdDebugReturnMemStat(stat);
  else if (forPE == -1) CmiReduceStruct(stat, CpdDebug_pupMemStat, CpdDebug_mergeMemStat,
                                CpdDebugReturnMemStat, CpdDebug_deleteMemStat);
  else CmiAbort("Received allocationTree request for another PE!");
  CmiFree(msg);
}

static void CpdDebugHandlerStatus(char *msg) {
#if ! CMK_NO_SOCKETS    
  ChMessageInt_t reply[2];
  reply[0] = ChMessageInt_new(CmiMyPe());
  reply[1] = ChMessageInt_new(CpdIsFrozen() ? 0 : 1);
  CcsSendReply(2*sizeof(ChMessageInt_t), reply);
#endif
  CmiFree(msg);
}

static void CpdDebugHandlerFreeze(char *msg) {
  CpdFreeze();
  CmiFree(msg);
}


/* Deliver a single message in the queue while not unfreezing the program */
void CpdNext(void) {

}

/* This converse handler is used by the debugger itself, to send messages
 * even when the scheduler is in freeze mode.
 */
void handleDebugMessage(void *msg) {
  CmiSetHandler(msg, CmiGetXHandler(msg));
  CmiHandleMessage(msg);
}

/* Special scheduler-type loop only executed while in
freeze mode-- only executes CCS requests.
*/
void CcsServerCheck(void);
extern int _isCcsHandlerIdx(int idx);
int (*CpdIsDebugMessage)(void *);
void * (*CpdGetNextMessage)(CsdSchedulerState_t*);

void CpdFreezeModeScheduler(void)
{
#if CMK_CCS_AVAILABLE
    void *msg;
    void *debugQ=CpvAccess(debugQueue);
    CsdSchedulerState_t state;
    CsdSchedulerState_new(&state);

    /* While frozen, queue up messages */
    while (CpvAccess(freezeModeFlag)) {
#if NODE_0_IS_CONVHOST
      if (CmiMyPe()==0) CcsServerCheck(); /*Make sure we can get CCS messages*/
#endif
      msg = CpdGetNextMessage(&state);

      if (msg!=NULL) {
        /*int hIdx=CmiGetHandler(msg);*/
	  /*
	  if(_isCcsHandlerIdx(hIdx))
	  / *A CCS request-- handle it immediately* /
          {
	    CmiHandleMessage(msg);
          }
	  else if (hIdx == _debugHandlerIdx ||
	          (hIdx == CmiGetReductionHandler() && CmiGetReductionDestination() == CpdDebugReturnAllocationTree)) {
	    / * Debug messages should be handled immediately * /
	    CmiHandleMessage(msg);
	  } else */
      if (conditionalPipe[1]!=0 && _conditionalDelivery==0) {
        // Since we are conditionally delivering, forward all messages to the child
        int bytes = SIZEFIELD(msg); // reqLen+((int)(reqData-((char*)hdr)))+CmiReservedHeaderSize;
        if (write(conditionalPipe[1], &bytes, 4) != 4) {
          CmiAbort("Writing msg len to child failed!");
        }
        if (write(conditionalPipe[1], msg, bytes) != bytes) {
          CmiAbort("Writing msg data to child failed!");
        }
      }
      if (CpdIsDebugMessage(msg)) {
        CmiHandleMessage(msg);
	  }
	  else
	  /*An ordinary charm++ message-- queue it up*/
	    CdsFifo_Enqueue(debugQ, msg);
      } else CmiNotifyIdle();
    }
    /* Before leaving freeze mode, execute the messages
       in the order they would have executed before.*/
    while (!CdsFifo_Empty(debugQ))
    {
	char *queuedMsg = (char *)CdsFifo_Dequeue(debugQ);
        CmiHandleMessage(queuedMsg);
    }
#endif
}

void CpdMemoryMarkClean(char *msg);

void CpdInit(void)
{
  CpvInitialize(int, freezeModeFlag);
  CpvAccess(freezeModeFlag) = 0;

  CpvInitialize(void *, debugQueue);
  CpvAccess(debugQueue) = CdsFifo_Create();

  CpvInitialize(void *, conditionalQueue);
  CpvAccess(conditionalQueue) = CdsFifo_Create();
  
  CcsRegisterHandler("debug/converse/freeze", (CmiHandler)CpdDebugHandlerFreeze);
  CcsRegisterHandler("debug/converse/status", (CmiHandler)CpdDebugHandlerStatus);
  CcsSetMergeFn("debug/converse/status", CcsMerge_concat);

  CcsRegisterHandler("debug/memory/allocationTree", (CmiHandler)CpdDebugCallAllocationTree);
  CpvInitialize(int, CpdDebugCallAllocationTree_Index);
  CpvAccess(CpdDebugCallAllocationTree_Index) = CmiRegisterHandler((CmiHandler)CpdDebugCallAllocationTree);
  
  CcsRegisterHandler("debug/memory/stat", (CmiHandler)CpdDebugCallMemStat);
  CpvInitialize(int, CpdDebugCallMemStat_Index);
  CpvAccess(CpdDebugCallMemStat_Index) = CmiRegisterHandler((CmiHandler)CpdDebugCallMemStat);

  CcsRegisterHandler("debug/memory/leak",(CmiHandler)CpdSearchLeaks);
  CpvInitialize(int, CpdSearchLeaks_Index);
  CpvAccess(CpdSearchLeaks_Index) = CmiRegisterHandler((CmiHandler)CpdSearchLeaks);
  CpvInitialize(int, CpdSearchLeaksDone_Index);
  CpvAccess(CpdSearchLeaksDone_Index) = CmiRegisterHandler((CmiHandler)CpdSearchLeaksDone);
  
  CcsRegisterHandler("debug/memory/mark",(CmiHandler)CpdMemoryMarkClean);
  CcsSetMergeFn("debug/memory/mark", CcsMerge_concat);

  _debugHandlerIdx = CmiRegisterHandler((CmiHandler)handleDebugMessage);
#if 0
  CpdInitializeObjectTable();
  CpdInitializeHandlerArray();
  CpdInitializeBreakPoints();

  /* To allow start in freeze state: */
  msgListCleanup();
  msgListCache();
#endif

}

/* If CharmDebug is attached, try to send it a message and wait */
void CpdAborting(const char *message) {
#if CMK_CCS_AVAILABLE
  if (cmiArgDebugFlag) {
    CpdNotify(CPD_ABORT, message);
    CpdFreeze();
  }
#endif
}

/*****************************************************************************
 *  Benchmark to measure performance of CmiSyncBroadcast
 *
 *  Does two types of benchmarking-
 *
 *  1. A flurry of Bcasts followed by a reduction
 *
 *  2. Singleton broadcast followed by reduction (clocks synchronized
 *                                                across processors)
 *
 *  Author- Nikhil Jain
 *  Date- Dec/26/2011
 *
 *****************************************************************************/

#include "converse.h"
#include "commbench.h"

typedef double* pdouble;

CpvStaticDeclare(int, numiter);
CpvStaticDeclare(int, nextidx);
CpvStaticDeclare(int, bcast_handler);
CpvStaticDeclare(int, bcast_reply);
CpvStaticDeclare(int, bcast_central);
CpvStaticDeclare(int, reduction_handler);
CpvStaticDeclare(int, sync_starter);
CpvStaticDeclare(int, sync_reply);
CpvStaticDeclare(double, starttime);
CpvStaticDeclare(double, lasttime);
CpvStaticDeclare(pdouble, timediff);
CpvStaticDeclare(int, currentPe);

#define MAXITER 512

static struct testdata {
  int size;
  int numiter;
  double time;
} sizes[] = {
  {4, MAXITER, 0.0},
  {16, MAXITER, 0.0},
  {64, MAXITER, 0.0},
  {256, MAXITER, 0.0},
  {1024, MAXITER, 0.0},
  {4096, MAXITER, 0.0},
  {16384, MAXITER, 0.0},
  {65536, MAXITER, 0.0},
  {262144, MAXITER / 2, 0.0},
  {1048576, MAXITER / 4, 0.0},
  {-1, -1, 0.0},
};

typedef struct _timemsg {
  char head[CmiMsgHeaderSizeBytes];
  double time;
  int srcpe;
} * ptimemsg;

typedef struct _timemsg timemsg;

static const char sync_outstr[] = "[broadcast] (%s) %le seconds per %d bytes\n";

static void* reduceMessage(size_t* size, void* data, void** remote, int count) {
  (void)size;
  (void)remote;
  (void)count;
  return data;
}

static void print_results(const char* func) {
  int i = 0;

  while (sizes[i].size != (-1)) {
    CmiPrintf(sync_outstr, func, sizes[i].time / sizes[i].numiter,
        sizes[i].size);
    i++;
  }
}

static void bcast_handler(void* msg) {
  int idx = CpvAccess(nextidx);
  void* red_msg;

  CpvAccess(numiter)++;
  if (CpvAccess(numiter) < sizes[idx].numiter) {
    if (CmiMyPe() == 0) {
      CmiSyncBroadcastAllAndFree(CmiMsgHeaderSizeBytes + sizes[idx].size, msg);
    } else
      CmiFree(msg);

  } else {
    CmiFree(msg);
    red_msg = CmiAlloc(CmiMsgHeaderSizeBytes);
    CmiSetHandler(red_msg, CpvAccess(reduction_handler));
    CmiReduce(red_msg, CmiMsgHeaderSizeBytes, reduceMessage);
    if (CmiMyPe() != 0) {
      CpvAccess(nextidx) = idx + 1;
      CpvAccess(numiter) = 0;
    }
  }
}

static void reduction_handler(void* msg) {
  int i = 0;
  int idx = CpvAccess(nextidx);
  EmptyMsg emsg;
  CmiInitMsgHeader(emsg.core, sizeof(EmptyMsg));

  sizes[idx].time = CmiWallTimer() - CpvAccess(starttime);
  CmiFree(msg);
  CpvAccess(numiter) = 0;
  idx++;
  if (sizes[idx].size == (-1)) {
    print_results("Consecutive CmiSyncBroadcastAllAndFree");
    CpvAccess(nextidx) = 0;
    CpvAccess(numiter) = 0;
    while (sizes[i].size != (-1)) {
      sizes[i].time = 0;
      i++;
    }
    CmiSetHandler(&emsg, CpvAccess(sync_reply));
    CpvAccess(lasttime) = CmiWallTimer();
    CmiSyncSend(CpvAccess(currentPe), sizeof(EmptyMsg), &emsg);
    return;
  } else {
    CpvAccess(nextidx) = idx;
    msg = CmiAlloc(CmiMsgHeaderSizeBytes + sizes[idx].size);
    CmiSetHandler(msg, CpvAccess(bcast_handler));
    CpvAccess(starttime) = CmiWallTimer();
    CmiSyncBroadcastAllAndFree(CmiMsgHeaderSizeBytes + sizes[idx].size, msg);
  }
}

/* on PE 0 */
static void sync_starter(void* msg) {
  EmptyMsg emsg;
  CmiInitMsgHeader(emsg.core, sizeof(EmptyMsg));
  ptimemsg tmsg = (ptimemsg)msg;

  double midTime = (CmiWallTimer() + CpvAccess(lasttime)) / 2;
  CpvAccess(timediff)[CpvAccess(currentPe)] = midTime - tmsg->time;
  CmiFree(msg);

  CpvAccess(currentPe)++;
  if (CpvAccess(currentPe) < CmiNumPes()) {
    CmiSetHandler(&emsg, CpvAccess(sync_reply));
    CpvAccess(lasttime) = CmiWallTimer();
    CmiSyncSend(CpvAccess(currentPe), sizeof(EmptyMsg), &emsg);
  } else {
    msg = CmiAlloc(CmiMsgHeaderSizeBytes + sizes[0].size);
    CmiSetHandler(msg, CpvAccess(bcast_reply));
    CpvAccess(currentPe) = 0;
    CpvAccess(starttime) = CmiWallTimer();
    CmiSyncBroadcastAllAndFree(CmiMsgHeaderSizeBytes + sizes[0].size, msg);
  }
}

static void sync_reply(void* msg) {
  ptimemsg tmsg = (ptimemsg)CmiAlloc(sizeof(timemsg));
  tmsg->time = CmiWallTimer();
  CmiSetHandler(tmsg, CpvAccess(sync_starter));
  CmiSyncSendAndFree(0, sizeof(timemsg), tmsg);
  CmiFree(msg);
}

static void bcast_reply(void* msg) {
  ptimemsg tmsg = (ptimemsg)CmiAlloc(sizeof(timemsg));
  tmsg->time = CmiWallTimer();
  tmsg->srcpe = CmiMyPe();
  CmiSetHandler(tmsg, CpvAccess(bcast_central));
  CmiSyncSendAndFree(0, sizeof(timemsg), tmsg);
  CmiFree(msg);
}

static void bcast_central(void* msg) {
  EmptyMsg emsg;
  CmiInitMsgHeader(emsg.core, sizeof(EmptyMsg));
  ptimemsg tmsg = (ptimemsg)msg;
  CmiAssert(CmiMyPe() == 0);
  if (CpvAccess(currentPe) == 0) {
    CpvAccess(lasttime) =
      tmsg->time - CpvAccess(starttime) + CpvAccess(timediff)[tmsg->srcpe];
  } else if ((tmsg->time - CpvAccess(starttime) +
        CpvAccess(timediff)[tmsg->srcpe]) > CpvAccess(lasttime)) {
    CpvAccess(lasttime) =
      tmsg->time - CpvAccess(starttime) + CpvAccess(timediff)[tmsg->srcpe];
  }
  CmiFree(msg);
  CpvAccess(currentPe)++;
  if (CpvAccess(currentPe) == CmiNumPes()) {
    sizes[CpvAccess(nextidx)].time += CpvAccess(lasttime);
    CpvAccess(numiter)++;
    if (CpvAccess(numiter) < sizes[CpvAccess(nextidx)].numiter) {
      msg = CmiAlloc(CmiMsgHeaderSizeBytes + sizes[CpvAccess(nextidx)].size);
      CpvAccess(currentPe) = 0;
      CmiSetHandler(msg, CpvAccess(bcast_reply));
      CpvAccess(starttime) = CmiWallTimer();
      CmiSyncBroadcastAllAndFree(
          CmiMsgHeaderSizeBytes + sizes[CpvAccess(nextidx)].size, msg);
    } else {
      CpvAccess(numiter) = 0;
      CpvAccess(nextidx)++;
      if (sizes[CpvAccess(nextidx)].size == (-1)) {
        print_results("CmiSyncBroadcastAllAndFree");
        CmiSetHandler(&emsg, CpvAccess(ack_handler));
        CmiSyncSend(0, sizeof(EmptyMsg), &emsg);
        return;
      } else {
        msg = CmiAlloc(CmiMsgHeaderSizeBytes + sizes[CpvAccess(nextidx)].size);
        CpvAccess(currentPe) = 0;
        CmiSetHandler(msg, CpvAccess(bcast_reply));
        CpvAccess(starttime) = CmiWallTimer();
        CmiSyncBroadcastAllAndFree(
            CmiMsgHeaderSizeBytes + sizes[CpvAccess(nextidx)].size, msg);
      }
    }
  }
}

void broadcast_init(void) {
  void* msg;

  msg = CmiAlloc(CmiMsgHeaderSizeBytes + sizes[0].size);
  CmiSetHandler(msg, CpvAccess(bcast_handler));
  CpvAccess(starttime) = CmiWallTimer();
  CmiSyncBroadcastAllAndFree(CmiMsgHeaderSizeBytes + sizes[0].size, msg);
}

void broadcast_moduleinit(void) {
  CpvInitialize(int, numiter);
  CpvInitialize(int, nextidx);
  CpvInitialize(double, starttime);
  CpvInitialize(double, lasttime);
  CpvInitialize(pdouble, timediff);
  CpvInitialize(int, currentPe);
  CpvInitialize(int, bcast_handler);
  CpvInitialize(int, bcast_reply);
  CpvInitialize(int, bcast_central);
  CpvInitialize(int, reduction_handler);
  CpvInitialize(int, sync_starter);
  CpvInitialize(int, sync_reply);
  CpvAccess(numiter) = 0;
  CpvAccess(nextidx) = 0;
  CpvAccess(currentPe) = 0;
  CpvAccess(timediff) = (pdouble)malloc(CmiNumPes() * sizeof(double));
  CpvAccess(bcast_handler) = CmiRegisterHandler((CmiHandler)bcast_handler);
  CpvAccess(bcast_reply) = CmiRegisterHandler((CmiHandler)bcast_reply);
  CpvAccess(bcast_central) = CmiRegisterHandler((CmiHandler)bcast_central);
  CpvAccess(reduction_handler) =
    CmiRegisterHandler((CmiHandler)reduction_handler);
  CpvAccess(sync_starter) = CmiRegisterHandler((CmiHandler)sync_starter);
  CpvAccess(sync_reply) = CmiRegisterHandler((CmiHandler)sync_reply);
}

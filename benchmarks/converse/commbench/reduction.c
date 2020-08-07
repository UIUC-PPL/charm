/*****************************************************************************
 *
 *  Benchmarks to measure performance of CmiReduce
 *
 *  Clocks are synchronized first up, followed by singleton CmiReduce
 *  after which the performance is measured by message collection are
 *  a central point.
 *
 *
 *  Author- Nikhil Jain
 *
 *****************************************************************************/

#include "converse.h"
#include "commbench.h"

typedef double* pdouble;

CpvStaticDeclare(int, numiter);
CpvStaticDeclare(int, nextidx);
CpvStaticDeclare(int, reduction_starter);
CpvStaticDeclare(int, reduction_handler);
CpvStaticDeclare(int, reduction_central);
CpvStaticDeclare(int, sync_starter);
CpvStaticDeclare(int, sync_reply);
CpvStaticDeclare(int, flip);
CpvStaticDeclare(double, starttime);
CpvStaticDeclare(double, endtime);
CpvStaticDeclare(double, lasttime);
CpvStaticDeclare(pdouble, timediff);
CpvStaticDeclare(int, currentPe);

/* change it if adding values to sizes */
#define MAXSIZE 1048576

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
  {262144, MAXITER, 0.0},
  {1048576, MAXITER, 0.0},
  {-1, -1, 0.0},
};

typedef struct _varmsg {
  char head[CmiMsgHeaderSizeBytes];
  char contribution[MAXSIZE];
} * varmsg;

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

static void reduction_starter(void* msg) {
  int idx = CpvAccess(nextidx);
  varmsg red_msg;
  ptimemsg tmsg;
  CmiFree(msg);

  if (CpvAccess(flip)) {
    tmsg = (ptimemsg)CmiAlloc(sizeof(timemsg));
    tmsg->time = CpvAccess(starttime);
    ;
    tmsg->srcpe = CmiMyPe();
    CmiSetHandler(tmsg, CpvAccess(reduction_central));
    CmiSyncSend(0, sizeof(timemsg), tmsg);
    CmiFree(tmsg);
    CpvAccess(flip) = 0;
  } else {
    red_msg = (varmsg)CmiAlloc(sizeof(struct _varmsg));
    CmiSetHandler(red_msg, CpvAccess(reduction_handler));
    CpvAccess(starttime) = CmiWallTimer();
    CmiReduce(red_msg, CmiMsgHeaderSizeBytes + sizes[idx].size, reduceMessage);
    CpvAccess(flip) = 1;
    if (CmiMyPe() != 0) {
      CpvAccess(numiter)++;
      if (CpvAccess(numiter) == sizes[idx].numiter) {
        CpvAccess(nextidx) = idx + 1;
        CpvAccess(numiter) = 0;
      }
    }
  }
}

static void reduction_handler(void* msg) {
  EmptyMsg emsg;
  CmiInitMsgHeader(emsg.core, sizeof(EmptyMsg));

  CpvAccess(endtime) = CmiWallTimer();

  CmiFree(msg);
  CmiSetHandler(&emsg, CpvAccess(reduction_starter));
  CmiSyncBroadcastAll(sizeof(EmptyMsg), &emsg);
}

static void reduction_central(void* msg) {
  EmptyMsg emsg;
  ptimemsg tmsg = (ptimemsg)msg;

  CmiInitMsgHeader(emsg.core, sizeof(EmptyMsg));
  if (CpvAccess(currentPe) == 0) {
    CpvAccess(lasttime) =
      CpvAccess(endtime) - tmsg->time - CpvAccess(timediff)[tmsg->srcpe];
  } else if ((CpvAccess(endtime) - tmsg->time -
        CpvAccess(timediff)[tmsg->srcpe]) > CpvAccess(lasttime)) {
    CpvAccess(lasttime) =
      CpvAccess(endtime) - tmsg->time - CpvAccess(timediff)[tmsg->srcpe];
  }
  CmiFree(msg);
  CpvAccess(currentPe)++;
  if (CpvAccess(currentPe) == CmiNumPes()) {
    sizes[CpvAccess(nextidx)].time += CpvAccess(lasttime);
    CpvAccess(numiter)++;
    if (CpvAccess(numiter) < sizes[CpvAccess(nextidx)].numiter) {
      CpvAccess(currentPe) = 0;
      CmiSetHandler(&emsg, CpvAccess(reduction_starter));
      CmiSyncBroadcastAll(sizeof(EmptyMsg), &emsg);
    } else {
      CpvAccess(numiter) = 0;
      CpvAccess(nextidx)++;
      if (sizes[CpvAccess(nextidx)].size == (-1)) {
        print_results("CmiReduce");
        CmiSetHandler(&emsg, CpvAccess(ack_handler));
        CmiSyncSend(0, sizeof(EmptyMsg), &emsg);
        return;
      } else {
        CpvAccess(currentPe) = 0;
        CmiSetHandler(&emsg, CpvAccess(reduction_starter));
        CmiSyncBroadcastAll(sizeof(EmptyMsg), &emsg);
      }
    }
  }
}

static void sync_starter(void* msg) {
  EmptyMsg emsg;
  ptimemsg tmsg = (ptimemsg)msg;

  CmiInitMsgHeader(emsg.core, sizeof(EmptyMsg));
  double midTime = (CmiWallTimer() + CpvAccess(lasttime)) / 2;
  CpvAccess(timediff)[CpvAccess(currentPe)] = midTime - tmsg->time;
  CmiFree(msg);

  CpvAccess(currentPe)++;
  if (CpvAccess(currentPe) < CmiNumPes()) {
    CmiSetHandler(&emsg, CpvAccess(sync_reply));
    CpvAccess(lasttime) = CmiWallTimer();
    CmiSyncSend(CpvAccess(currentPe), sizeof(EmptyMsg), &emsg);
  } else {
    CmiSetHandler(&emsg, CpvAccess(reduction_starter));
    CpvAccess(currentPe) = 0;
    CmiSyncBroadcastAll(sizeof(EmptyMsg), &emsg);
  }
}

static void sync_reply(void* msg) {
  ptimemsg tmsg = (ptimemsg)CmiAlloc(sizeof(timemsg));
  tmsg->time = CmiWallTimer();

  CmiFree(msg);
  CmiSetHandler(tmsg, CpvAccess(sync_starter));
  CmiSyncSendAndFree(0, sizeof(timemsg), tmsg);
}

void reduction_init(void) {
  EmptyMsg emsg;
  CmiInitMsgHeader(emsg.core, sizeof(EmptyMsg));

  CmiSetHandler(&emsg, CpvAccess(sync_reply));
  CpvAccess(lasttime) = CmiWallTimer();
  CmiSyncSend(CpvAccess(currentPe), sizeof(EmptyMsg), &emsg);
}

void reduction_moduleinit(void) {
  CpvInitialize(int, numiter);
  CpvInitialize(int, nextidx);
  CpvInitialize(int, flip);
  CpvInitialize(int, currentPe);
  CpvInitialize(double, starttime);
  CpvInitialize(double, lasttime);
  CpvInitialize(double, endtime);
  CpvInitialize(pdouble, timediff);
  CpvInitialize(int, sync_starter);
  CpvInitialize(int, sync_reply);
  CpvInitialize(int, reduction_starter);
  CpvInitialize(int, reduction_handler);
  CpvInitialize(int, reduction_central);
  CpvAccess(numiter) = 0;
  CpvAccess(nextidx) = 0;
  CpvAccess(currentPe) = 0;
  CpvAccess(flip) = 0;
  CpvAccess(timediff) = (pdouble)malloc(CmiNumPes() * sizeof(double));
  CpvAccess(reduction_starter) =
    CmiRegisterHandler((CmiHandler)reduction_starter);
  CpvAccess(reduction_handler) =
    CmiRegisterHandler((CmiHandler)reduction_handler);
  CpvAccess(reduction_central) =
    CmiRegisterHandler((CmiHandler)reduction_central);
  CpvAccess(sync_starter) = CmiRegisterHandler((CmiHandler)sync_starter);
  CpvAccess(sync_reply) = CmiRegisterHandler((CmiHandler)sync_reply);
}

#include <converse.h>

static int reduction_debug = 0;

#define DebugPrintf  if (reduction_debug) CmiPrintf

void Cpm_megacon_ack(CpmDestination);

typedef struct _mesg {
  char head[CmiMsgHeaderSizeBytes];
  int sum;
} *mesg;

struct twoInts {
  int positive;
  int negative;
};

static void pupTwoInts(pup_er p, void *data) {
  struct twoInts *d = (struct twoInts *)data;
  DebugPrintf("[%d] called pupTwoInts on %p (%d/%d)\n",CmiMyNode(),data,d->positive,d->negative);
  pup_int(p, &d->positive);
  pup_int(p, &d->negative);
}

static void deleteTwoInts(void *data) {
  DebugPrintf("[%d] called deleteTwoInts %p (%d/%d)\n",CmiMyNode(),data,
      ((struct twoInts*)data)->positive,((struct twoInts*)data)->negative);
  free(data);
}

static void *mergeTwoInts(size_t *size, void *data, void **remote, int count) {
  int i;
  struct twoInts *local = (struct twoInts *)data;
  DebugPrintf("[%d] called mergeTwoInts with local(%d/%d), %d remote(",CmiMyNode(),
      local->positive,local->negative,count);
  for (i=0; i<count; ++i) {
    struct twoInts *d = (struct twoInts *)remote[i];
    DebugPrintf("%d/%d,",d->positive,d->negative);
    local->positive += d->positive;
    local->negative += d->negative;
  }
  DebugPrintf(")\n");
  return data;
}

CpvStaticDeclare(int, broadcast_msg_idx);
CpvStaticDeclare(int, reduction_msg_idx);
CpvStaticDeclare(int, broadcast_struct_idx);

static void * addMessage(size_t *size, void *data, void **remote, int count) {
  mesg msg = (mesg)data;
  int i;
  DebugPrintf("[%d] called addMessage with local(%d), %d remote(",CmiMyNode(),msg->sum,count);
  for (i=0; i<count; ++i) {
    DebugPrintf("%d,",((mesg)remote[i])->sum);
    msg->sum += ((mesg)remote[i])->sum;
  }
  DebugPrintf(")\n");
  return data;
}

static void reduction_msg(mesg m) {
  DebugPrintf("[%d] reduction_msg\n", CmiMyNode());
  int i, sum=0;
  CmiAssert(CmiMyNode() == 0);
  for (i=0; i<CmiNumNodes(); ++i) sum += i+1;
  if (m->sum != sum) {
    CmiPrintf("Sum not matching: received %d, expecting %d\n", m->sum, sum);
    exit(1);
  }
  CmiSetHandler(m, CpvAccess(broadcast_struct_idx));
  CmiSyncNodeBroadcastAllAndFree(sizeof(struct _mesg),m);
}

static void broadcast_msg(mesg m) {
  DebugPrintf("[%d] broadcast_msg\n", CmiMyNode());
  m->sum = CmiMyNode()+1;
  CmiSetHandler(m, CpvAccess(reduction_msg_idx));
  CmiNodeReduce(m, sizeof(struct _mesg), addMessage);
}

static void reduction_struct(void *data) {
  DebugPrintf("[%d] reduction_struct\n", CmiMyNode());
  int i, sum=0;
  struct twoInts *two = (struct twoInts *)data;
  CmiAssert(CmiMyNode() == 0);
  for (i=0; i<CmiNumNodes(); ++i) sum += i+1;
  if (two->positive != sum || two->negative != -2*sum) {
    CmiPrintf("Sum not matching: received %d/%d, expecting %d/%d\n",
        two->positive, two->negative, sum, -2*sum);
    exit(1);
  }
  Cpm_megacon_ack(CpmSend(0));
}

static void broadcast_struct(mesg m) {
  DebugPrintf("[%d] broadcast_struct\n", CmiMyNode());
  struct twoInts *two = (struct twoInts*)malloc(sizeof(struct twoInts));
  CmiFree(m);
  DebugPrintf("[%d] allocated struct %p\n",CmiMyNode(),two);
  two->positive = CmiMyNode()+1;
  two->negative = -2*(CmiMyNode()+1);
  CmiNodeReduceStruct(two, pupTwoInts, mergeTwoInts, reduction_struct, deleteTwoInts);
}

void nodereduction_init(void)
{
  if (CmiMyRank() == 0)
  {
    mesg msg = (mesg)CmiAlloc(sizeof(struct _mesg));
    CmiSetHandler(msg, CpvAccess(broadcast_msg_idx));
    CmiSyncNodeBroadcastAllAndFree(sizeof(struct _mesg),msg);
  }
}

void nodereduction_moduleinit(void)
{
  CpvInitialize(int, broadcast_msg_idx);
  CpvInitialize(int, reduction_msg_idx);
  CpvInitialize(int, broadcast_struct_idx);
  CpvAccess(broadcast_msg_idx) = CmiRegisterHandler((CmiHandler)broadcast_msg);
  CpvAccess(reduction_msg_idx) = CmiRegisterHandler((CmiHandler)reduction_msg);
  CpvAccess(broadcast_struct_idx) = CmiRegisterHandler((CmiHandler)broadcast_struct);
}

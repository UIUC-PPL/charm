#include <stdio.h>
#include <time.h>
#include <converse.h>

CpvDeclare(int, vecsend_index);

#define CmiMsgHeaderSizeInts \
  ((CmiMsgHeaderSizeBytes+sizeof(int)-1)/sizeof(int))

void Cpm_megacon_ack(CpmDestination);

void vecsend_handler(int *msg)
{
  int i, next;
  size_t sizes[1];
  char *msgs[1];

  if (CmiMyPe()==0) {
    for (i=CmiMsgHeaderSizeInts; i<3600; i++) {
      if (msg[i] != i) {
        CmiPrintf("Failure in vecsend test, data corrupted.\n");
        exit(1);
      }
    }
    Cpm_megacon_ack(CpmSend(0));
  } else {
    next = (CmiMyPe()+1) % CmiNumPes();
    sizes[0] = 3600*sizeof(int);
    msgs[0] = (char *)msg;
    CmiSyncVectorSend(next, 1, sizes, msgs);
  }
  CmiFree(msg);
}

void vecsend_init()
{
  int i, *msg;
  size_t *sizes;
  int **msgs;
  if (CmiNumPes()<2) {
    CmiPrintf("note: vecsend requires at least 2 processors, skipping test.\n");
    Cpm_megacon_ack(CpmSend(0));
  } else {
    sizes = (size_t *) CmiAlloc(4*sizeof(size_t));
    msgs = (int **) CmiAlloc(4*sizeof(int *));
    sizes[0] = 1000; sizes[1] = 500; sizes[2] = 2000; sizes[3] = 100;
    for(i=0;i<4;i++) msgs[i] = (int *)CmiAlloc(sizes[i]*sizeof(int));
    for(i=0;i<4;i++) sizes[i] *= sizeof(int);
    for (i=CmiMsgHeaderSizeInts; i<1000; i++) msgs[0][i] = i;
    for (i=0; i<500; i++) msgs[1][i] = i+1000;
    for (i=0; i<2000; i++) msgs[2][i] = i+1500;
    for (i=0; i<100; i++) msgs[3][i] = i+3500;
    CmiSetHandler(msgs[0], CpvAccess(vecsend_index));
    CmiSyncVectorSendAndFree(1, 4, sizes, (char **) msgs);
  }
}

void vecsend_moduleinit()
{
  CpvInitialize(int, vecsend_index);
  CpvAccess(vecsend_index) = CmiRegisterHandler((CmiHandler)vecsend_handler);
}

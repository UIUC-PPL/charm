#include <stdio.h>
#include <converse.h>

void Cpm_megacon_ack(CpmDestination);

typedef struct {
  int me;
} multisend_info;

typedef struct
{
  char core[CmiMsgHeaderSizeBytes];
  int me;
  double data[10];
} multisendmsg;

#define nMulti 8
CpvDeclare(int*, multisend_index);
CpvDeclare(int, multisend_done_index);

CpvDeclare(int, multisend_replies);

void multisend_fail()
{
  CmiAbort("data corrupted in multisend.\n");
}

static void checkMsg(multisendmsg *msg) {
  int i;
  for(i=0;i<10-msg->me;i++) {
    if(msg->data[i] != (double) (i+msg->me))
      multisend_fail();
  }
}

void multisend_handler(multisendmsg *msg,multisend_info *info)
{
  if(msg->me != info->me)
    multisend_fail();
  checkMsg(msg);
  /* Forward message back to master: */
  CmiSetHandler(msg, CpvAccess(multisend_done_index));
  CmiSyncSendAndFree(0, sizeof(multisendmsg)-sizeof(double)*msg->me, msg);
}

void multisend_done_handler(multisendmsg *msg)
{
  checkMsg(msg);
  CmiFree(msg);

  CpvAccess(multisend_replies)++;
  if(CpvAccess(multisend_replies)==nMulti)
    Cpm_megacon_ack(CpmSend(0));
}

void multisend_init(void)
{

#if 0
  if(CmiNumPes() < 2) {
    CmiPrintf("Multisend requires at least 2 processors. skipping...\n");
    Cpm_megacon_ack(CpmSend(0));
  } else
#endif
  {
    int m,i;
    size_t sizes[nMulti];
    char *msgs[nMulti];
    multisendmsg first; /* Allocate one message on the stack (because you can!) */
    CmiInitMsgHeader(first.core, sizeof(multisendmsg));
    for (m=0;m<nMulti;m++) {
      multisendmsg *msg;
      if (m==0) msg=&first;
      else msg=(multisendmsg *)CmiAlloc(sizeof(multisendmsg));
      CmiSetHandler(msg, CpvAccess(multisend_index)[m]);
      msg->me=m;
      for (i=0;i<10-m;i++) msg->data[i]=(double)(i+m);
      sizes[m]=sizeof(multisendmsg)-sizeof(double)*m;
      msgs[m]=(char *)msg;
    }
    CmiMultipleSend(1%CmiNumPes(), nMulti, sizes, msgs);
    for (m=0;m<nMulti;m++)
      if (m!=0)
        CmiFree(msgs[m]);
  }
  CpvAccess(multisend_replies) = 0;
}

void multisend_moduleinit()
{
  int m;
  CpvInitialize(int*, multisend_index);
  CpvInitialize(int, multisend_done_index);
  CpvInitialize(int, multisend_replies);

  CpvAccess(multisend_index)=(int *)CmiAlloc(nMulti*sizeof(int));
  for (m=0;m<nMulti;m++) {
    multisend_info *i=(multisend_info *)CmiAlloc(sizeof(multisend_info));
    i->me=m;
    CpvAccess(multisend_index)[m]=CmiRegisterHandlerEx(
        (CmiHandlerEx)multisend_handler,i);
  }

  CpvAccess(multisend_done_index) =
    CmiRegisterHandler((CmiHandler)multisend_done_handler);
}


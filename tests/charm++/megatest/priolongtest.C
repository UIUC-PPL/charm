#include "priolongtest.h"

void priolongtest_init(void)
{
  CProxy_priolongtest_chare::ckNew();
}

void priolongtest_moduleinit(void) {}

priolongtest_chare::priolongtest_chare(void)
{
  CProxy_priolongtest_chare self(thishandle);
  // max is 9223372036854775807LL
  lastprio = -922337203685477550LL;
  CmiInt8 lastsend=lastprio+NMSG+1;
  for(CmiInt8 i=lastprio; i<lastsend; i++) {
    priolongtest_msg *m = new (8*sizeof(CmiInt8)) priolongtest_msg(i);
    memcpy((CmiInt8 *) CkPriorityPtr(m), &i,sizeof(CmiInt8));
    CkSetQueueing(m, CK_QUEUEING_LFIFO);
    self.recv(m);
  }
  for(CmiInt8 i=NMSG-1; i>=0LL; i--) {
    priolongtest_msg *m = new (8*sizeof(CmiInt8)) priolongtest_msg(i);
    memcpy((CmiInt8 *) CkPriorityPtr(m), &i,sizeof(CmiInt8));
    CkSetQueueing(m, CK_QUEUEING_LFIFO);
    self.recv(m);
  }

}

void
priolongtest_chare::recv(priolongtest_msg *m)
{
  if(m->prio < lastprio)
    {
      CkPrintf("priolongtest: message %lld after %lld \n",m->prio, lastprio);
      CkAbort("priolongtest: message received out of order\n");
    }
  //  CkPrintf("priolongtest: got %lld prev was %lld \n",m->prio, lastprio);
  lastprio = m->prio;
  delete m;
  if(lastprio == NMSG-1)
    megatest_finish();
}

#if !defined(CMK_RANDOMIZED_MSGQ) && !defined(CK_ENABLE_MSG_PRIORITIES)
MEGATEST_REGISTER_TEST(priolongtest,"ebohm",1) 
#endif

#include "priolongtest.def.h"

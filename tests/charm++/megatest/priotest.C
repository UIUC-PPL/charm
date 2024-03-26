#include "priotest.h"

void priotest_init(void)
{
  CProxy_priotest_chare::ckNew();
}

void priotest_moduleinit(void) {}

priotest_chare::priotest_chare(void)
{
  CProxy_priotest_chare self(thishandle);
  lastprio = -1;
  for(int i=NMSG-1; i>=0; i--) {
    priotest_msg *m = new (8*sizeof(int)) priotest_msg(i);
    *((int *)CkPriorityPtr(m)) = i;
    CkSetQueueing(m, CK_QUEUEING_IFIFO);
    self.recv(m);
  }
}

void
priotest_chare::recv(priotest_msg *m)
{
  if(m->prio < lastprio)
    CkAbort("priotest: message received out of order\n");
  lastprio = m->prio;
  delete m;
  if(lastprio == NMSG-1)
    megatest_finish();
}

#if !defined(CMK_RANDOMIZED_MSGQ) && !defined(CK_ENABLE_MSG_PRIORITIES)
MEGATEST_REGISTER_TEST(priotest,"mlind",1)
#endif

#include "priotest.def.h"

#include "varsizetest.h"

static int nextseq = 0;

void varsizetest_init(void) 
{
  if(CkNumPes()<2) {
    CkError("varsize: requires at least 2 processors\n");
    megatest_finish();
  } else
    CProxy_varsizetest_main::ckNew(0);
}

void varsizetest_moduleinit(void) {}

void *varsizetest_Msg::alloc(int mnum, size_t size, size_t *sizes, int pbits, GroupDepNum groupDepNum)
{
  // CkPrintf("Msg::alloc called with size=%d, sizes[0]=%d, sizes[1]=%d\n",
  //         size, sizes[0], sizes[1]);
  size_t stmp = sizes[0]*sizeof(int)+sizes[1]*sizeof(float);
  varsizetest_Msg *m = (varsizetest_Msg *) CkAllocMsg(mnum, size+stmp, pbits, groupDepNum);
  m->isize = sizes[0];
  m->fsize = sizes[1];
  m->iarray = (int *)((char *)m+size);
  m->farray = (float *)((char *)m+size+sizes[0]*sizeof(int));
  return (void *) m;
}

void *varsizetest_Msg::pack(varsizetest_Msg *m)
{
  // CkPrintf("M::pack called\n");
  return (void *) m;
}

varsizetest_Msg *varsizetest_Msg::unpack(void *buf)
{
  //CkPrintf("M::unpack called\n");
  varsizetest_Msg *m = (varsizetest_Msg *) buf;
  m->iarray = (int *)((char *)m+sizeof(varsizetest_Msg));
  m->farray = (float *)((char *)m+sizeof(varsizetest_Msg)+(m->isize*sizeof(int)));
  return m;
}

varsizetest_main::varsizetest_main(void)
{
  size_t sizes[2];
  sizes[0] = 100; sizes[1] = 300;
  varsizetest_Msg *m = new (sizes,0) varsizetest_Msg(nextseq++);
  m->myMain = thishandle;
  CProxy_varsizetest_test::ckNew(m, 1);
}

void varsizetest_main::exit(varsizetest_Msg *m)
{
  if(!m->check())
    CkAbort("varsizetest failed!\n");
  delete m;
  delete this;
  megatest_finish();
}

varsizetest_Msg::varsizetest_Msg(int s)
{
  seqnum = s;
  int i;
  for(i=0; i<isize; i++)
    iarray[i] = i*i*seqnum;
  for(i=0; i<fsize;i++)
    farray[i] = 2.0*i*i*seqnum;
}

int varsizetest_Msg::check(void)
{
  int i;
  for(i=0; i<isize; i++)
    if(iarray[i] != i*i*seqnum) {
      return 0;
    }
  for(i=0; i<fsize; i++)
    if((fabs(farray[i]) - fabs(2.0*i*i*seqnum))>10.0) {
      CkPrintf("farray[%d] should be %0.8f, is instead %0.8f\n",i,farray[i],2.0*i*i*seqnum);
      return 0;
    }
  return 1;
}

varsizetest_test::varsizetest_test(varsizetest_Msg *m)
{
  CkChareID mhandle = m->myMain;
  int currentSeqnum = m->seqnum;
  if(!m->check()) {
    CkAbort("varsizetest failed!\n");
    megatest_finish();
    return;
  }
  delete m;
  size_t sizes[2];
  sizes[0] = 300;
  sizes[1] = 100;
  m = new (sizes,0) varsizetest_Msg(currentSeqnum);
  CProxy_varsizetest_main mainproxy(mhandle);
  mainproxy.exit(m); 
  delete this;
}

MEGATEST_REGISTER_TEST(varsizetest,"mjlang",1)
#include "varsizetest.def.h"

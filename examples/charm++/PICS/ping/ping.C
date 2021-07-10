#include <string.h> // for strlen, and strcmp
#include <charm++.h>
#include "picsautoperfAPI.h"
//#include "picsautotunerAPI.h"
#define NITER 20
#define PAYLOAD 1024
#define TUNE_FREQ 4

#include "ping.decl.h"
class PingMsg : public CMessage_PingMsg
{
public:
  char *x;
};


CProxy_Main mainProxy;
int maxIter;
int payload;
int workLoad;

class Main: public CBase_Main{

  Main_SDAG_CODE

  double startTimer;
  int totalCnt;
  CProxy_Ping1 arr1;
  int iter;
  int dv, minv, maxv;

public:
  Main(CkMigrateMessage *m) {}
  Main(CkArgMsg* m) {
    if(CkNumPes()>2) {
      CkAbort("Run this program on 1 or 2 processors only.\n");
    }

    iter = 0;
    maxIter=NITER;
    payload=PAYLOAD;
    workLoad = 1024;
    dv = 1;
    minv = 1;
    maxv = 32;
    if(m->argc>1)
      payload=atoi(m->argv[1]);
    if(m->argc>2)
      maxIter=atoi(m->argv[2]);
    if(m->argc>3)
      workLoad =atoi(m->argv[3]);
    if(m->argc>4)
      dv=atoi(m->argv[4]);
    CkPrintf("ping with payload: %d workload:%d maxIter: %d\n", payload, workLoad, maxIter);
    mainProxy = thishandle;
    arr1 = CProxy_Ping1::ckNew(2);
    delete m;
    thisProxy.prepare();
  };

  void prepare() {
    char *names[] = {"PING"};
    PicsConfig pc;
    pc.numPhases = 1;
    pc.phaseNames = names;
    PICS_configure(pc, CkCallbackResumeThread());
    //PICS_registerTunableParameterFields("PIPELINE_NUM", TP_INT, dv, minv, maxv, 1, PICS_EFF_GRAINSIZE, -1, OP_ADD, TS_SIMPLE, 1);
    thisProxy.run();
  }

};

double doWork(int cnt) {
  double sum = 0;
  for(int  i=0; i<cnt; i++)
  {
    sum += i/3*(i-7)/11;
  }
  return sum;
}

class Ping1 : public CBase_Ping1
{
  int cnt;
  double sum;
public:
  Ping1() {
    cnt = 0;
    sum = 0;
  }

  Ping1(CkMigrateMessage *m) {}

  void start() {
    int valid = 0;
    int frags = 1;
    //int frags = (int)PICS_getTunedParameter("PIPELINE_NUM", &valid);
    CkPrintf("getTunned is    %d\n", frags);
    if(!valid)
      frags = 1;
    int size = payload/frags;
    for(int i=0; i<frags; i++)
    {
      sum += doWork(workLoad/frags);
      PingMsg *msg = new (size) PingMsg();
      memset(msg->x, sum, sizeof(double));
      thisProxy[1].recv(msg);
    }
  }

  void recv(PingMsg *msg)
  {
    int valid = 0;
    int frags = 1;
    //(int)PICS_getTunedParameter("PIPELINE_NUM", &valid);
    if(!valid)
      frags = 1;
    cnt++;
    sum += doWork(workLoad/frags);
    if(cnt == frags)
    {
      mainProxy.report(sum);
      cnt = 0;
    }
    delete msg;
  }
};

#include "ping.def.h"

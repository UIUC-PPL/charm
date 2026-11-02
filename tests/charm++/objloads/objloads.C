// Server half of the CkPerfObjLoads test (client.C queries it over CCS).
//
// Four arrays of n elements keep every PE busy until the deadline: two
// arrays of the same class (same indices, so their elements must not merge),
// one of a derived class (its entries are the base's, so its elements must
// not split), and one whose elements run a [threaded] entry that yields
// between spins (its time arrives in resumed thread segments). The load
// database's instrumentation is on, so the client can compare the trace's
// "CkPerfObjLoads" with "LBObjLoads" object by object.
//
//   ./objloads ++server ++server-port <port> +pe P [n] [seconds]
#include "objloads.decl.h"
#include "LBManager.h"

/* readonly */ CProxy_Main mainProxy;
/* readonly */ double deadline;

static void spin(double seconds) {
  double t = CkWallTimer();
  while (CkWallTimer() - t < seconds) {}
}

class Worker : public CBase_Worker {
 public:
  Worker() {}
  Worker(CkMigrateMessage *m) : CBase_Worker(m) {}
  void step() {
    spin(0.001);
    if (CkWallTimer() < deadline)
      thisProxy[thisIndex].step();
    else
      contribute(CkCallback(CkReductionTarget(Main, done), mainProxy));
  }
  void run() {  // [threaded]
    while (CkWallTimer() < deadline) {
      spin(0.001);
      CthYield();
    }
    contribute(CkCallback(CkReductionTarget(Main, done), mainProxy));
  }
};

class Derived : public CBase_Derived {
 public:
  Derived() {}
  Derived(CkMigrateMessage *m) : CBase_Derived(m) {}
};

class Switch : public CBase_Switch {
 public:
  Switch() { LBTurnInstrumentOn(); }  // per PE, so LBObjLoads has figures
};

class Main : public CBase_Main {
  int pending;
 public:
  Main(CkArgMsg *m) {
    int n = 8;
    double seconds = 10.0;
    if (m->argc > 1) n = atoi(m->argv[1]);
    if (m->argc > 2) seconds = atof(m->argv[2]);
    delete m;
    CProxy_Switch::ckNew();
    mainProxy = thisProxy;
    deadline = CkWallTimer() + seconds;
    CProxy_Worker a = CProxy_Worker::ckNew(n);
    CProxy_Worker b = CProxy_Worker::ckNew(n);
    CProxy_Worker t = CProxy_Worker::ckNew(n);
    CProxy_Derived d = CProxy_Derived::ckNew(n);
    a.step();
    b.step();
    d.step();
    t.run();
    pending = 4;
    CkPrintf("objloads: %d elements in 4 arrays on %d PEs, running %.0f s\n",
             4 * n, CkNumPes(), seconds);
  }
  void done() {
    if (--pending == 0) {
      CkPrintf("objloads: done\n");
      CkExit();
    }
  }
};

#include "objloads.def.h"

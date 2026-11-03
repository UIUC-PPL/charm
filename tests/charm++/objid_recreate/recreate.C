#include <stdio.h>
#include "recreate.decl.h"

// The home PE's view of an element that dies and is created again under the same
// id (packed kind). Every PE drives the home-side handlers of the array's CkLocMgr
// directly, for the indices it is the home of, in the orders the network can
// produce; no element exists and nothing is sent. Afterwards whichPe(id) must name
// the live life. Scenarios (doc/internals/process-location-cache.md, section 3.5):
//
//   0  a life dies: the entry is cleared
//   1  the new life's creation update reaches the home before the old life's
//      death notice, which names a higher epoch
//   2  a creation update of the old life reaches the home after its death notice
//   3  the death notice names a move the home never saw; the move's update comes
//      after it
//   4  the new life has moved twice when the old life's notice arrives
//   5  a dead life's late move update replaces the new life's entry (higher
//      epoch) before the dead life's notice arrives
//   6  a dead life's late move update, higher epoch, arrives after its notice,
//      when the new life's entry is in place (the ordering found by the
//      independent model of PR #4123)
//
// The checks (count, failures) are summed to Main.

/*readonly*/ CProxy_Main mainProxy;
/*readonly*/ CProxy_Elem arrProxy;

static const int N = 96;  // indices; each PE handles the ones it is the home of

class Elem : public CBase_Elem
{
public:
  Elem() {}
  Elem(CkMigrateMessage*) {}
};

class Main : public CBase_Main
{
public:
  Main(CkArgMsg* m)
  {
    delete m;
    CkPrintf("objid_recreate: %d PEs in %d process(es)\n", CkNumPes(), CkNumNodes());
    mainProxy = thisProxy;
    CkArrayOptions opts;
    opts.setBounds(1024);  // packed kind: the id is the index
    arrProxy = CProxy_Elem::ckNew(opts);
    CProxy_Driver driver = CProxy_Driver::ckNew();
    driver.run();
  }

  void done(CkReductionMsg* msg)
  {
    const long* v = (const long*)msg->getData();
    CkPrintf("objid_recreate: %ld checks, %ld failures\n", v[0], v[1]);
    if (v[1] != 0) CkAbort("objid_recreate: failures\n");
    delete msg;
    CkPrintf("All done\n");
    CkExit();
  }
};

class Driver : public CBase_Driver
{
  CkLocMgr* lm;
  CkArrayIndex1D idx;
  CmiUInt8 id;
  long checked = 0, failed = 0;

  void upd(int pe, int epoch, CmiUInt8 life)
  {
    CkLocEntry e;
    e.id = id;
    e.pe = pe;
    e.epoch = epoch;
    e.life = life;
    lm->updateLocation(idx, e);
  }
  void dead(int pe, int epoch, CmiUInt8 life) { lm->reclaimRemote(idx, id, pe, epoch, life); }
  void expect(int pe, int scenario, const char* when)
  {
    ++checked;
    const int got = lm->whichPe(id);
    if (got != pe)
    {
      ++failed;
      CkPrintf("PE %d index %d scenario %d, %s: location %d, expected %d\n", CkMyPe(),
               idx.index[0], scenario, when, got, pe);
    }
  }

public:
  Driver() {}

  void run()
  {
    lm = arrProxy.ckLocMgr();
    const int P = CkNumPes();
    const int a = (CkMyPe() + 1) % P, b = (CkMyPe() + 2) % P, c = (CkMyPe() + 3) % P;
    for (int k = 0; k < N; ++k)
    {
      idx = CkArrayIndex1D(k);
      if (lm->homePe(idx) != CkMyPe()) continue;
      id = lm->lookupID(idx);
      const CmiUInt8 L1 = 1000 + 2 * k, L2 = L1 + 1;
      const int s = k % 7;
      switch (s)
      {
        case 0:
          upd(a, 0, L1);
          expect(a, s, "after the creation update");
          dead(a, 0, L1);
          expect(-1, s, "after the death notice");
          break;
        case 1:
          upd(a, 1, L1);
          upd(b, 0, L2);
          dead(a, 1, L1);
          expect(b, s, "new life's update, then the old life's notice");
          break;
        case 2:
          upd(a, 1, L1);
          dead(a, 1, L1);
          expect(-1, s, "after the death notice");
          upd(c, 0, L1);
          expect(-1, s, "stale creation update of the dead life");
          upd(b, 0, L2);
          expect(b, s, "new life's creation update");
          break;
        case 3:
          upd(a, 1, L1);
          dead(c, 2, L1);
          expect(-1, s, "notice of a death after a move the home never saw");
          upd(c, 2, L1);
          expect(-1, s, "the move's update after the notice");
          upd(b, 0, L2);
          expect(b, s, "new life's creation update");
          break;
        case 4:
          upd(a, 1, L1);
          upd(b, 2, L2);
          dead(a, 1, L1);
          expect(b, s, "old life's notice after the new life moved twice");
          break;
        case 5:
          upd(b, 0, L2);
          upd(a, 1, L1);
          dead(a, 1, L1);
          expect(b, s, "dead life's move update replaced the new life, then its notice");
          break;
        case 6:
          upd(a, 1, L1);
          dead(a, 1, L1);
          upd(b, 0, L2);
          upd(a, 1, L1);
          expect(b, s, "dead life's move update after its notice, over the new life");
          break;
      }
    }
    long v[2] = {checked, failed};
    contribute(sizeof(v), v, CkReduction::sum_long, CkCallback(CkIndex_Main::done(NULL), mainProxy));
  }
};

#include "recreate.def.h"

// objid_boundrecreate: a destroyed element of a bound array re-created by
// [createhere] from a PE whose process still holds the dead element's location
// (charm #4031; doc/internals/object-ids.md, section 3). Needs two processes.
//
// A and B are bound arrays; C is a single array. Element idx of each lives on
// PE P in the home's process; PE r in the other process sends to it, so r's
// process records "idx at P"; all three die; r re-creates A[idx] and C[idx] by
// createhere. Both must be created on r: the home approved the creation, which
// means no element of the id is alive, so the creator does not follow its entry.
// From Ritvik Rao's reproducer in the review of #4138.
#include "boundrecreate.decl.h"

/* readonly */ CProxy_Main mainProxy;
/* readonly */ CProxy_A aProxy, bProxy, cProxy;
/* readonly */ int idx;
static const int N = 8;
static CProxy_Sender senders;

static int whichOf(const CkArrayID& id) {
  if (id == aProxy.ckGetArrayID()) return 0;
  if (id == bProxy.ckGetArrayID()) return 1;
  return 2;
}

class Main : public CBase_Main {
  int stage = 0, P = -1, r = -1, home = -1;
  int createdPe[3] = {-1, -1, -1};

public:
  Main(CkArgMsg* m) {
    delete m;
    mainProxy = thisProxy;
    if (CkNumNodes() < 2 || CkNodeSize(0) < 2) {
      CkPrintf("objid_boundrecreate: needs 2 processes with 2 or more PEs in the first; "
               "nothing to check with %d processes, %d PEs\n", CkNumNodes(), CkNumPes());
      CkExit();
      return;
    }
    CkArrayOptions ao; ao.setBounds(N);
    aProxy = CProxy_A::ckNew(ao);
    CkArrayOptions bo; bo.setBounds(N); bo.bindTo(aProxy);
    bProxy = CProxy_A::ckNew(bo);
    CkArrayOptions co; co.setBounds(N);
    cProxy = CProxy_A::ckNew(co);
    // An index whose home is in process 0, placed on another PE of process 0.
    idx = -1;
    for (int i = 0; i < N && idx < 0; i++) {
      int h = aProxy.ckLocMgr()->homePe(CkArrayIndex1D(i));
      if (CkNodeOf(h) == 0) { idx = i; home = h; }
    }
    CkEnforce(idx >= 0);
    P = (home == CkNodeFirst(0)) ? CkNodeFirst(0) + 1 : CkNodeFirst(0);
    r = CkNodeFirst(1);
    CkPrintf("objid_boundrecreate: idx %d home %d placed on %d, sender %d\n", idx, home, P, r);
    aProxy[idx].insert(0, P); bProxy[idx].insert(1, P); cProxy[idx].insert(2, P);
    aProxy.doneInserting(); bProxy.doneInserting(); cProxy.doneInserting();
    senders = CProxy_Sender::ckNew();
    CkStartQD(CkCallback(CkIndex_Main::quiet(), thisProxy));
  }
  void quiet() {
    switch (stage++) {
      case 0: senders[r].ping(); break;
      case 1: senders[r].report(1); return;  // located() restarts QD
      case 2: aProxy[idx].die(); bProxy[idx].die(); cProxy[idx].die(); break;
      case 3: senders[r].report(3); return;
      case 4: senders[r].revive(); break;
      case 5:
        CkPrintf("objid_boundrecreate: re-created by createhere from PE %d: bound A on PE %d, "
                 "single C on PE %d\n", r, createdPe[0], createdPe[2]);
        if (createdPe[0] != r || createdPe[2] != r) {
          CkPrintf("objid_boundrecreate FAIL: %s re-created on the PE the element died on\n",
                   createdPe[0] != r ? "the bound array's element was" : "the single array's element was");
          CkAbort("misplaced re-creation");
        }
        CkPrintf("objid_boundrecreate PASS\n");
        CkExit();
        return;
    }
    CkStartQD(CkCallback(CkIndex_Main::quiet(), thisProxy));
  }
  void located(int s, int peA, int peC) {
    CkPrintf("objid_boundrecreate: stage %d, PE %d's entry for idx: A -> %d, C -> %d\n", s, r,
             peA, peC);
    CkStartQD(CkCallback(CkIndex_Main::quiet(), thisProxy));
  }
  void created(int which, int pe) { createdPe[which] = pe; }
};

class Sender : public CBase_Sender {
public:
  Sender() {}
  void ping() { aProxy[idx].ping(); cProxy[idx].ping(); }
  void report(int s) {
    mainProxy.located(s, aProxy.ckLocMgr()->whichPe(CkArrayIndex1D(idx)),
                      cProxy.ckLocMgr()->whichPe(CkArrayIndex1D(idx)));
  }
  void revive() { aProxy[idx].reviveHere(CkMyPe()); cProxy[idx].reviveHere(CkMyPe()); }
};

class A : public CBase_A {
public:
  A() {}
  A(int which) {}
  A(CkMigrateMessage* m) {}
  void ping() {}
  void die() { ckDestroy(); }
  void reviveHere(int fromPe) { mainProxy.created(whichOf(thisProxy.ckGetArrayID()), CkMyPe()); }
};

#include "boundrecreate.def.h"

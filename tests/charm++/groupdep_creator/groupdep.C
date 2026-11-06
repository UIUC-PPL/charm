// groupdep_creator: a group creation's dependence holds on the creating PE too
// (charm #4150). PE 1 creates group A and asks PE 0 to create group B with a
// dependence on A. PE 0 may take the request before A's creation broadcast;
// B's constructor must still find A's branch. Each round PE 1 first makes PE 0
// busy for a moment, so both messages are queued there together, and under
// +randomized_msgq the scheduler takes them in either order. Needs 2 or more PEs.
#include "groupdep.decl.h"

/* readonly */ CProxy_Main mainProxy;
/* readonly */ int rounds;

class Main : public CBase_Main {
  int round = 0;
  CProxy_Starter starters;

public:
  Main(CkArgMsg* m) {
    rounds = 20;
    CmiGetArgInt(m->argv, "-r", &rounds);
    delete m;
    if (CkNumPes() < 2) {
      CkPrintf("groupdep_creator: needs 2 or more PEs; nothing to check\n");
      CkExit();
      return;
    }
    mainProxy = thisProxy;
    starters = CProxy_Starter::ckNew();
    starters[1].begin(round);
  }

  // Every B branch found A's branch in its constructor.
  void built(int n) {
    CkEnforce(n == CkNumPes());
    if (++round == rounds) {
      CkPrintf("groupdep_creator PASS: %d rounds, %d PEs, B's constructor found A on "
               "every PE every time\n", rounds, CkNumPes());
      CkExit();
      return;
    }
    starters[1].begin(round);
  }
};

class Starter : public CBase_Starter {
public:
  Starter() {}
  // On PE 1: create A, then ask PE 0 to create B depending on A. The request can
  // overtake A's creation broadcast on PE 0.
  void begin(int round) {
    thisProxy[0].stall();
    CkGroupID aID = CProxy_A::ckNew(round);
    thisProxy[0].makeB(aID, round);
  }
  // On PE 0: spin so that A's creation and the makeB request queue up behind it.
  void stall() {
    const double t0 = CkWallTimer();
    while (CkWallTimer() - t0 < 0.002) {}
  }
  // On PE 0: the creating PE of B. Without the dependence check for the creating
  // PE's own branch, B's constructor ran here before A's branch existed.
  void makeB(CkGroupID aID, int round) {
    CkEntryOptions opts;
    opts.setGroupDepID(aID);
    CProxy_B::ckNew(aID, round, &opts);
  }
};

class A : public CBase_A {
public:
  A(int round) {}
};

class B : public CBase_B {
public:
  B(CkGroupID aID, int round) {
    if (CkLocalBranch(aID) == NULL) {
      CkPrintf("groupdep_creator FAIL: round %d: B constructed on PE %d before A's branch "
               "existed there\n", round, CkMyPe());
      CkAbort("group dependence not honored");
    }
    int one = 1;
    contribute(sizeof(int), &one, CkReduction::sum_int,
               CkCallback(CkReductionTarget(Main, built), mainProxy));
  }
};

#include "groupdep.def.h"

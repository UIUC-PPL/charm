// Replication as a balancing decision, host only (bins are PEs, the load is
// measured wall time). nWorkers workers on the PEs; worker 0 carries most of
// the load for the first half of the run and little after. Each worker owns a
// running sum over nItems items per step, computed as two partitions (even
// and odd items) added in a fixed order. Under +balancer ShedLB the balancer
// asks for a copy of worker 0 (ckClone) while it is heavy -- the two copies
// then each compute one partition and exchange partials -- and takes the
// copy back (ckRetire) once it is light. The final sums must equal what a
// sequential evaluation gives, and the run must have seen at least one clone
// and one retire; with -norep (declared maximum 1) neither may happen.
#include "replica_test.decl.h"
#include <cmath>
#include <cstring>
#include <vector>

/* readonly */ CProxy_Main mainProxy;
/* readonly */ CProxy_Worker workers;
/* readonly */ int nWorkers;
/* readonly */ int nSteps;
/* readonly */ int lbPeriod;
/* readonly */ int nItems;
/* readonly */ int repMax;
/* readonly */ double unitMs;

// Worker w's work in step s, in units: worker 0 is heavy in the first half.
static double unitsOf(int w, int s) {
  if (w != 0) return 1.0;
  return s <= nSteps / 2 ? 0.6 * (nWorkers - 1) / 0.4 : 1.0;
}
// The value item i of worker w contributes in step s: a pure function.
static double itemValue(int w, int s, int i) {
  return std::sin(0.37 * w + 0.11 * s + 0.0071 * i) * (1.0 + 0.001 * i);
}
// One partition's sum over the items, in item order: what one copy computes.
static double partitionSum(int w, int s, int p) {
  double v = 0.0;
  for (int i = p; i < nItems; i += 2) v += itemValue(w, s, i);
  return v;
}
static void burn(double ms) {
  const double t0 = CkWallTimer();
  volatile double x = 0.0;
  while ((CkWallTimer() - t0) * 1e3 < ms) x += 1.0;
}

class Main : public CBase_Main {
  int doneCount = 0, clones = 0, retires = 0;
  bool norep = false;
  double worst = 0.0;

 public:
  Main(CkArgMsg* m) {
    mainProxy = thisProxy;
    nWorkers = 2 * CkNumPes();
    nSteps = 60;   // heavy for 30 steps: a clone at the first round, a retire at the fourth, 20 steps of slack
    lbPeriod = 10;
    nItems = 4096;
    repMax = 2;
    unitMs = 2.0;
    for (int i = 1; i < m->argc; i++) {
      if (strcmp(m->argv[i], "-norep") == 0) { repMax = 1; norep = true; }
      else if (strcmp(m->argv[i], "-steps") == 0 && i + 1 < m->argc) nSteps = atoi(m->argv[++i]);
      else if (strcmp(m->argv[i], "-ms") == 0 && i + 1 < m->argc) unitMs = atof(m->argv[++i]);
    }
    delete m;
    CkPrintf("replica_test: %d workers on %d PEs, %d steps, balancing every %d, worker 0 heavy "
             "for %d steps, copies allowed: %d\n", nWorkers, CkNumPes(), nSteps, lbPeriod,
             nSteps / 2, repMax);
    CkArrayOptions opts(nWorkers, 1);
    opts.setBounds(nWorkers, 2);   // room for (w, 1) in the element id
    workers = CProxy_Worker::ckNew(opts);
    workers.start();
  }
  void cloned(int w, int step, int pe) {
    clones++;
    CkPrintf("replica_test: worker %d cloned at step %d onto PE %d\n", w, step, pe);
  }
  void retired(int w, int step, int pe) {
    retires++;
    CkPrintf("replica_test: worker %d's copy on PE %d retired at step %d\n", w, pe, step);
  }
  void done(int w, double acc, int steps) {
    double expect = 0.0;
    for (int s = 1; s <= nSteps; s++) expect += partitionSum(w, s, 0) + partitionSum(w, s, 1);
    const double err = std::fabs(acc - expect);
    if (err > worst) worst = err;
    if (acc != expect)
      CkAbort("worker %d: sum %.17g differs from the sequential %.17g\n", w, acc, expect);
    if (++doneCount < nWorkers) return;
    CkPrintf("replica_test: all %d sums equal the sequential evaluation bit for bit; %d clone(s), "
             "%d retire(s)\n", nWorkers, clones, retires);
    if (norep) {
      if (clones || retires) CkAbort("-norep: the balancer must not replicate a worker that declared one copy\n");
    } else {
      if (clones < 1) CkAbort("expected the balancer to replicate worker 0 while it was heavy\n");
      if (retires < 1) CkAbort("expected the balancer to retire worker 0's copy once it was light\n");
    }
    CkPrintf("replica_test: pass\n");
    CkExit();
  }
};

class Worker : public CBase_Worker {
  Worker_SDAG_CODE
 public:
  int step = 0;
  double acc = 0.0;
  bool atLB = false;
  bool awaitingResume = false;
  bool resumedByLB = false;
  bool replicated = false;
  double own = 0.0, peerV = 0.0;
  bool peerRetiring = false;
  int cloneToPe = -1, cloneRank = -1;
  bool retirePending = false, retiring = false;

  Worker() { usesAtSync = true; }
  Worker(CkMigrateMessage* m) { usesAtSync = true; }
  int w() const { return thisIndex.x; }
  int r() const { return thisIndex.y; }

  void pup(PUP::er& p) {
    p | step; p | acc; p | atLB; p | awaitingResume; p | replicated; p | own; p | peerV;
    p | peerRetiring; p | cloneToPe; p | cloneRank; p | retirePending;
    _sdag_pup(p);
  }

  void start() { thisProxy[thisIndex].run(); }
  // The new copy: starts at `s` with the sum so far.
  void takeState(int s, double a) {
    step = s - 1;
    acc = a;
    replicated = true;
    thisProxy[thisIndex].run();
  }

  void beginStep() {
    step++;
    resumedByLB = false;
    peerRetiring = false;
    atLB = false;
    // Every copy declares its place; the balancer reads it with the loads.
    setLBReplica(w(), r(), replicated ? 2 : 1, repMax, 0);
    if (lbPeriod > 0 && step % lbPeriod == 0 && step < nSteps) {
      // The load this copy carried since the last balancing step, declared
      // (the model-based path, as the MoE example declares its device time):
      // the work a copy does is its share of the worker's units.
      double sec = 0.0;
      for (int s = step - lbPeriod + 1; s <= step; s++)
        sec += unitsOf(w(), s) * unitMs * 1e-3 / (replicated ? 2.0 : 1.0);
      setObjTime(sec);
      atLB = true;
      awaitingResume = true;
      AtSync();
    }
  }
  void ResumeFromSync() {
    if (!awaitingResume) return;   // a copy inserted during the resume asked for nothing
    awaitingResume = false;
    resumedByLB = true;
    thisProxy[thisIndex].resumed();
  }

  // The balancer's decisions, acted on at the end of the step it released
  // this copy in.
  void ckClone(int toPe, int rank) {
    if (replicated || r() != 0) CkAbort("worker %d.%d: clone asked while already two copies\n", w(), r());
    cloneToPe = toPe;
    cloneRank = rank;
  }
  void ckRetire() {
    if (!replicated || r() == 0) CkAbort("worker %d.%d: retire asked of the only copy\n", w(), r());
    retirePending = true;
    CkPrintf("replica_test: worker %d.%d on PE %d: retire decision at step %d\n", w(), r(), CkMyPe(), step);
  }

  void compute() {
    const double u = unitsOf(w(), step);
    if (replicated) {
      own = partitionSum(w(), step, r());
      burn(unitMs * u / 2);
      workers(w(), 1 - r()).partial(step, own, (resumedByLB && retirePending) ? 1 : 0);
    } else {
      const double p0 = partitionSum(w(), step, 0), p1 = partitionSum(w(), step, 1);
      burn(unitMs * u);
      acc += p0 + p1;
    }
  }
  void notePeer(double v, int retiring_) {
    peerV = v;
    peerRetiring = retiring_ != 0;
    // Partition order, a fixed order on both copies.
    acc += (r() == 0) ? own + peerV : peerV + own;
  }
  void endStep() {
    if (replicated && resumedByLB && retirePending) retiring = true;
    if (peerRetiring) replicated = false;
    if (!replicated && resumedByLB && cloneToPe >= 0 && r() == 0) {
      const int toPe = cloneToPe, rank = cloneRank;
      cloneToPe = cloneRank = -1;
      replicated = true;
      workers(w(), rank).insert(toPe);
      workers(w(), rank).takeState(step + 1, acc);
      mainProxy.cloned(w(), step, toPe);
    }
    if (retiring) {
      mainProxy.retired(w(), step, CkMyPe());
      step = nSteps;   // leaves the loop; finish() destroys the copy
    }
  }
  void finish() {
    if (r() == 0) mainProxy.done(w(), acc, step);
    else thisProxy[thisIndex].die();   // off this entry method: a message to itself
  }
  void die() { ckDestroy(); }
};

#include "replica_test.def.h"

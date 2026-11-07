// In-memory checkpoint and rollback with no failed process.
//
// A 1-D array of Worker elements advances one step per broadcast; each step
// ends in a reduction to Main. At step 5 Main takes an in-memory checkpoint
// (CkStartMemCheckpoint). At step 8, the first time only, Main calls
// CkRollbackMemCheckpoint(): every element is restored to its step-5 state
// (step counter, accumulator, and a heap-allocated history vector), and the
// checkpoint's callback (Main::afterCheckpoint) arrives a second time. Steps
// 6-8 therefore run twice (N+1 times with -rollbacks N, which rolls back to
// the same checkpoint at each of the first N arrivals at step 8). Main and the Tally group are not rolled back (only
// array elements are), so they can count the repeats: Main counts the
// reductions per step and the callbacks, and Tally counts per element how
// often step 6 ran. The final accumulator sum must equal a run with no
// rollback.
//
// With -lb the elements call AtSync at step 6 (run with +balancer RotateLB):
// elements migrate between the checkpoint and the rollback, which the
// runtime refuses with a "rollback refused" abort (testrestriction target).

#include "memckpt_rollback.decl.h"
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

CProxy_Main mainProxy;
CProxy_Tally tallyProxy;
int numElements;
int lbMode;
int numRollbacks;  // -rollbacks N: roll back to the same checkpoint N times (default 1)

static const int kCheckpointStep = 5;
static const int kRollbackStep = 8;
static const int kLastStep = 10;
static const long kMod = 1000000007L;

static long nextAcc(long acc, int idx, int step) { return (acc * 31 + idx * 7 + step) % kMod; }

class Main : public CBase_Main {
  CProxy_Worker workers;
  int stepRuns[kLastStep + 1] = {0};
  int callbacks = 0;
  bool checkpointed = false;
  int rollbacks = 0;

 public:
  Main(CkArgMsg *m) {
    numElements = 8;
    lbMode = 0;
    numRollbacks = 1;
    for (int i = 1; i < m->argc; i++) {
      if (std::string(m->argv[i]) == "-lb") lbMode = 1;
      if (std::string(m->argv[i]) == "-rollbacks" && i + 1 < m->argc) numRollbacks = atoi(m->argv[++i]);
    }
    delete m;
    mainProxy = thisProxy;
    tallyProxy = CProxy_Tally::ckNew();
    workers = CProxy_Worker::ckNew(numElements);
    workers.doStep(1);
  }

  // data[0] is the sum of the elements' step numbers, data[1] of their accumulators
  void stepDone(int n, long *data) {
    int step = (int)(data[0] / numElements);
    long sum = data[1];
    stepRuns[step]++;
    if (step == kCheckpointStep && !checkpointed) {
      checkpointed = true;
      CkCallback cb(CkIndex_Main::afterCheckpoint(), thisProxy);
      CkStartMemCheckpoint(cb);
      return;
    }
    if (step == kRollbackStep && rollbacks < numRollbacks) {
      rollbacks++;
      CkPrintf("memckpt_rollback: rolling back at step %d\n", step);
      // Nothing else is in flight; the runtime waits for quiescence anyway.
      CkRollbackMemCheckpoint();
      return;
    }
    if (step < kLastStep) {
      workers.doStep(step + 1);
      return;
    }
    long expect = 0;
    for (int i = 0; i < numElements; i++) {
      long acc = 0;
      for (int s = 1; s <= kLastStep; s++) acc = nextAcc(acc, i, s);
      expect += acc;
    }
    if (sum != expect) CkAbort("memckpt_rollback: final sum %ld, expected %ld", sum, expect);
    for (int s = 1; s <= kLastStep; s++) {
      int want = (s > kCheckpointStep && s <= kRollbackStep) ? numRollbacks + 1 : 1;
      if (stepRuns[s] != want)
        CkAbort("memckpt_rollback: step %d completed %d times, expected %d", s, stepRuns[s], want);
    }
    if (callbacks != numRollbacks + 1)
      CkAbort("memckpt_rollback: checkpoint callback arrived %d times, expected %d", callbacks,
              numRollbacks + 1);
    tallyProxy.check();
  }

  // Sent once when the checkpoint completes and once more after the rollback.
  // Main's own state is not reverted by the rollback; it restarts the steps
  // from the checkpoint step itself.
  void afterCheckpoint() {
    callbacks++;
    CkPrintf("memckpt_rollback: checkpoint callback %d\n", callbacks);
    workers.doStep(kCheckpointStep + 1);
  }

  void tallyDone(int good, int bad) {
    if (good != numElements || bad != 0)
      CkAbort("memckpt_rollback: %d elements ran step 6 %d times, %d did not", good,
              numRollbacks + 1, bad);
    CkPrintf("memckpt_rollback passed\n");
    CkExit();
  }
};

// Not rolled back: counts per element how many times step 6 ran.
class Tally : public CBase_Tally {
 public:
  std::map<int, int> ranStep6;
  Tally() {}
  void check() {
    int res[2] = {0, 0};
    for (auto &e : ranStep6) res[e.second == numRollbacks + 1 ? 0 : 1]++;
    contribute(2 * sizeof(int), res, CkReduction::sum_int,
               CkCallback(CkReductionTarget(Main, tallyDone), mainProxy));
  }
};

class Worker : public CBase_Worker {
  int step = 0;
  long acc = 0;
  std::vector<long> *history;  // heap state that must be restored

 public:
  Worker() : history(new std::vector<long>) { usesAtSync = (lbMode != 0); }
  Worker(CkMigrateMessage *m) : CBase_Worker(m), history(new std::vector<long>) {}
  ~Worker() { delete history; }

  void pup(PUP::er &p) {
    p | step;
    p | acc;
    p | *history;
  }

  void doStep(int s) {
    if (s != step + 1)
      CkAbort("memckpt_rollback: element %d at step %d got step %d", thisIndex, step, s);
    if ((int)history->size() != step)
      CkAbort("memckpt_rollback: element %d history has %d entries at step %d", thisIndex,
              (int)history->size(), step);
    step = s;
    acc = nextAcc(acc, thisIndex, s);
    history->push_back(acc);
    if (s == kCheckpointStep + 1) tallyProxy.ckLocalBranch()->ranStep6[thisIndex]++;
    if (lbMode && s == kCheckpointStep + 1) {
      AtSync();
      return;
    }
    finishStep();
  }

  void ResumeFromSync() { finishStep(); }

  void finishStep() {
    long data[2] = {step, acc};
    contribute(2 * sizeof(long), data, CkReduction::sum_long,
               CkCallback(CkReductionTarget(Main, stepDone), mainProxy));
  }
};

#include "memckpt_rollback.def.h"

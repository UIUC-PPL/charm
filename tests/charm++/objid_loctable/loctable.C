#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include <bitset>
#include "cklocnode.h"
#include "loctable.decl.h"

// The process-level location and index tables (src/ck-core/cklocnode.h) under
// concurrent use by every PE of a process, on private table instances, so the
// runtime's own tables are not disturbed. Each phase runs on every PE at once and
// ends with a sum reduction of (checked, violations, extra) to Main, which
// compares the sums with what the protocol promises. Phases:
//
//   1  updates with increasing epochs by every PE on the same nKeys keys, each
//      write followed by a read that must never show an older epoch than this PE
//      saw before (no growth: the table is sized for the keys)
//   2  every key ends at the highest epoch written; then nKeys*4 keys go into a
//      16-slot table from every PE at once (growth under contention)
//   3  every grown-table key is found, the table grew, and no key is lost
//   4  every PE registers as a waiter on fresh keys and claims the request:
//      exactly one PE per process is told to send it
//   5  rank 0 of each process fills those keys: the waiter bits it takes name
//      every PE of the process exactly once, and the fill reopened the claim
//   6  index table: every PE binds the same (collection, index) -> id for nKeys
//      indices; exactly one bind per process reports "no previous binding"
//   7  rank 0 rebinds every index to a new id and is told the old one; every
//      lookup afterwards (phase 8) gives the new id
//   9  rank 0, in order: a fill lands between two ranks' misses and their
//      registrations, the first registrant finds the location, the entry is
//      cleared by a death, the second re-reads nothing; the second must be told
//      to send the request (the order of a review comment on #4123; the waiter
//      word alone cannot tell it, section 3.3)
//  10  rank 0, in order: a rank misses and registers, a fill lands before its
//      claim, it claims and sends, and the reply writes nothing (the entry is
//      already as new); after a death the next miss must be told to send
//
// doc/internals/process-location-cache.md, sections 2 and 3.

/*readonly*/ CProxy_Main mainProxy;
/*readonly*/ CProxy_Tables tablesProxy;
/*readonly*/ int nKeys;
/*readonly*/ int nRounds;

static const int PHASES = 10;

class Tables : public CBase_Tables
{
public:
  ck::objid::LocTable big;    // sized for nKeys: no growth
  ck::objid::LocTable small;  // 16 slots: grows
  ck::objid::IndexTable index;
  Tables() : big(16), small(4), index(4) {}
};

class Main : public CBase_Main
{
  int phase;
  CProxy_Worker workers;

public:
  Main(CkArgMsg* m)
  {
    nKeys = 1000;
    nRounds = 20;
    if (m->argc > 1) nKeys = atoi(m->argv[1]);
    if (m->argc > 2) nRounds = atoi(m->argv[2]);
    delete m;
    CkPrintf("objid_loctable: %d PEs in %d process(es), %d keys, %d rounds\n", CkNumPes(),
             CkNumNodes(), nKeys, nRounds);
    mainProxy = thisProxy;
    tablesProxy = CProxy_Tables::ckNew();
    workers = CProxy_Worker::ckNew();
    phase = 1;
    workers.run(phase);
  }

  void phaseDone(CkReductionMsg* msg)
  {
    const long* v = (const long*)msg->getData();
    const long checked = v[0], violations = v[1], extra = v[2];
    const long P = CkNumPes(), N = CkNumNodes(), ranks = P / N;
    long wantExtra = -1;  // -1: not checked
    const char* what = "";
    switch (phase)
    {
      case 1: what = "concurrent updates, epochs never go backwards"; break;
      case 2: what = "every key at the final epoch; grown-table inserts"; break;
      case 3: what = "grown table: every key found, growth happened"; wantExtra = N; break;
      case 4: what = "waiters: one request claim per key per process"; wantExtra = (long)nKeys * N; break;
      case 5:  // bit r stands for ranks r, r+64, ...: at most 64 bits per key
        what = "fill takes one waiter bit per PE and reopens the claim";
        wantExtra = (long)nKeys * N * (ranks < 64 ? ranks : 64);
        break;
      case 6: what = "index table: one unbound bind per key per process"; wantExtra = (long)nKeys * N; break;
      case 7: what = "index rebinding reports the old id"; wantExtra = (long)nKeys * N; break;
      case 8: what = "index lookups give the new id"; break;
      case 9: what = "fill between miss and registration, then a death: the request is still sent"; break;
      case 10: what = "a reply that writes nothing still reopens the claim"; break;
    }
    CkPrintf("phase %d: %s: %ld checked, %ld violations, extra %ld (expect %ld)\n", phase,
             what, checked, violations, extra, wantExtra);
    if (violations != 0) CkAbort("objid_loctable: violations\n");
    if (wantExtra >= 0 && extra != wantExtra) CkAbort("objid_loctable: count mismatch\n");
    delete msg;
    if (++phase > PHASES)
    {
      CkPrintf("All done\n");
      CkExit();
      return;
    }
    workers.run(phase);
  }
};

class Worker : public CBase_Worker
{
  std::vector<int> lastEpoch;
  static uint64_t bigKey(int k) { return 1 + (uint64_t)k; }
  static uint64_t smallKey(int k) { return 1000001 + (uint64_t)k; }
  static uint64_t waitKey(int k) { return 2000001 + (uint64_t)k; }
  static uint64_t raceKey(int k) { return 3000001 + (uint64_t)k; }
  static uint64_t replyKey(int k) { return 4000001 + (uint64_t)k; }

public:
  Worker() : lastEpoch(nKeys, -1) {}

  void run(int phase)
  {
    Tables* t = tablesProxy.ckLocalBranch();
    const int rank = CkMyRank(), P = CkMyNodeSize();
    long checked = 0, violations = 0, extra = 0;
    switch (phase)
    {
      case 1:
        for (int r = 1; r <= nRounds; ++r)
          for (int k = 0; k < nKeys; ++k)
          {
            t->big.update(bigKey(k), CkMyPe(), r * P + rank, false);
            const ck::objid::LocTable::Value v = t->big.lookup(bigKey(k));
            ++checked;
            if (!v.hasLocation() || v.epoch() < lastEpoch[k] || v.epoch() < r * P + rank ||
                CkNodeOf(v.pe()) != CkMyNode())
              ++violations;
            lastEpoch[k] = v.epoch();
          }
        break;
      case 2:
        for (int k = 0; k < nKeys; ++k)
        {
          ++checked;
          if (t->big.epoch(bigKey(k)) != nRounds * P + P - 1) ++violations;
        }
        for (int k = 0; k < 4 * nKeys; ++k) t->small.update(smallKey(k), CkMyPe(), rank, false);
        break;
      case 3:
        for (int k = 0; k < 4 * nKeys; ++k)
        {
          ++checked;
          const ck::objid::LocTable::Value v = t->small.lookup(smallKey(k));
          if (!v.hasLocation() || v.epoch() != P - 1) ++violations;
        }
        if (rank == 0)
        {
          extra = t->small.segments() > 1 ? 1 : 0;
          if (t->small.used() < (size_t)(4 * nKeys)) ++violations;  // a key was lost
        }
        break;
      case 4:
        for (int k = 0; k < nKeys; ++k)
        {
          ++checked;
          t->big.registerWaiter(waitKey(k), rank);
          if (t->big.claimRequest(waitKey(k))) ++extra;
        }
        break;
      case 5:
        if (rank == 0)
          for (int k = 0; k < nKeys; ++k)
          {
            ++checked;
            uint64_t bits = t->big.store(waitKey(k), CkMyPe(), 0);
            extra += (long)std::bitset<64>(bits).count();
            if (t->big.takeWaiters(waitKey(k)) != 0) ++violations;  // taken once only
            if (!t->big.claimRequest(waitKey(k))) ++violations;     // the fill reopened it
          }
        break;
      case 6:
        for (int k = 0; k < nKeys; ++k)
        {
          ++checked;
          const uint64_t id = 1 + (uint64_t)k;
          const uint64_t prev = t->index.bind(1, CkArrayIndex2D(k, 7), id);
          if (prev == 0) ++extra;
          else if (prev != id) ++violations;
          uint64_t got = 0;
          if (!t->index.lookup(1, CkArrayIndex2D(k, 7), got) || got != id) ++violations;
          if (t->index.lookup(2, CkArrayIndex2D(k, 7), got)) ++violations;  // other collection
          if (t->index.lookup(1, CkArrayIndex2D(k, 8), got)) ++violations;  // other index
        }
        break;
      case 7:
        if (rank == 0)
          for (int k = 0; k < nKeys; ++k)
          {
            ++checked;
            const uint64_t prev = t->index.bind(1, CkArrayIndex2D(k, 7), 100000 + (uint64_t)k);
            if (prev == 1 + (uint64_t)k) ++extra;
            else ++violations;
          }
        break;
      case 8:
        for (int k = 0; k < nKeys; ++k)
        {
          ++checked;
          uint64_t got = 0;
          if (!t->index.lookup(1, CkArrayIndex2D(k, 7), got) || got != 100000 + (uint64_t)k)
            ++violations;
        }
        if (rank == 0 && t->index.segments() < 2) ++violations;  // 16 slots must have grown
        break;
      case 9:
        if (rank == 0)
          for (int k = 0; k < nKeys; ++k)
          {
            ++checked;
            const uint64_t key = raceKey(k);
            const int c = CkMyPe();  // the element's PE; ranks 1 and 2 play the two senders
            if (t->big.store(key, c, 0) != 0) ++violations;  // nobody registered yet
            t->big.registerWaiter(key, 1);                   // first registrant
            t->big.registerWaiter(key, 2);
            if (t->big.pe(key) != c) ++violations;           // rank 1 re-reads, finds c, sends
            if (!t->big.clear(key, c, 0)) ++violations;      // the element dies on c
            if (t->big.pe(key) != -1) ++violations;          // rank 2 re-reads nothing
            if (!t->big.claimRequest(key)) ++violations;     // ... and must send the request
            if (t->big.claimRequest(key)) ++violations;      // a third miss would not
            const uint64_t bits = t->big.store(key, c, 1);   // re-creation: wakes both
            if (bits != 6) ++violations;
            if (!t->big.claimRequest(key)) ++violations;     // the fill reopened the claim
          }
        break;
      case 10:
        if (rank == 0)
          for (int k = 0; k < nKeys; ++k)
          {
            ++checked;
            const uint64_t key = replyKey(k);
            const int c = CkMyPe();  // the element's PE; rank 1 plays the sender
            t->big.registerWaiter(key, 1);                   // rank 1 misses and registers
            if (t->big.pe(key) != -1) ++violations;          // ... and re-reads nothing
            if (t->big.store(key, c, 0) != 2) ++violations;  // a fill lands: wakes rank 1
            if (!t->big.claimRequest(key)) ++violations;     // rank 1 claims after it, sends
            bool written = true;
            t->big.update(key, c, 0, false, &written, true); // the reply: nothing newer
            if (written) ++violations;
            if (!t->big.clear(key, c, 0)) ++violations;      // the element dies on c
            if (!t->big.claimRequest(key)) ++violations;     // the next miss must send
          }
        break;
    }
    long v[3] = {checked, violations, extra};
    contribute(sizeof(v), v, CkReduction::sum_long,
               CkCallback(CkIndex_Main::phaseDone(NULL), mainProxy));
  }
};

#include "loctable.def.h"

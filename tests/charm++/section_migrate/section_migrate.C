// CkMulticast array section whose members, and the element that owns the
// section proxy (the root), migrate during the run.
//
// Element 0 owns the section proxy, built once over the indices i with
// i % 3 != 2 (element 0 is a member). Each round:
//   - element 0 multicasts work(round) to the section;
//   - each member checks that the round is the next one it expects (so a
//     duplicate, a stale or a missing delivery is caught) and contributes two
//     section reductions with the cookie from that message: sum_int of
//     {index, 1, round} and max_int of {index, round};
//   - main receives both results through the reduction client that element 0
//     set with setReductionClient, and checks their values and their
//     reduction numbers (2*round and 2*round+1, so the numbering continues
//     across resetSection);
//   - after both results arrive, every element migrates, by migrateMe
//     (default) or by AtSync load balancing (-lb, run with +balancer
//     RotateLB), and the next round starts once the migration is complete.
// When element 0 unpacks on its new PE it calls resetSection and sets the
// reduction client again, as the manual describes.
//
// By default the section is delegated by hand to a CkMulticastMgr made by main
// (the manual's "manual delegation"); -auto uses the array's own manager,
// which every section proxy of the array is delegated to when it is created.
//
// With -inflight, the odd rounds instead migrate while their multicast and
// contributions are in flight: with migrateMe the members other than element 0
// move (-root moves element 0 too), with -lb every element moves. Between
// rounds element 0 then moves only after a whole round has gone through its
// current tree, the only point at which the program knows that tree is ready.
//
// Options:
//   -lb        migrate by AtSync load balancing instead of migrateMe
//   -inflight  odd rounds migrate in flight (see above)
//   -root      with -inflight and migrateMe, element 0 also moves in flight
//   -stillroot with migrateMe, element 0 never moves, so the root keeps its
//              tree and rebuilds it after members have moved
//   -double    element 0 migrates twice in a row (migrateMe: it sends itself
//              a second hop from ckJustMigrated; -lb: two load balancing steps
//              back to back), before its new tree can finish setting up
//   -every     with -inflight, every round migrates in flight
//   -auto      use the array's automatic section delegation
//   -r R       rounds (default 12)     -n N   elements (default 24)
//
// At the end each element checks how many work messages it received (rounds
// for a member, 0 otherwise) and main checks the member count.
// A watchdog aborts the run after 20 s without progress.

#include "section_migrate.decl.h"
#include "ckmulticast.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <vector>

/*readonly*/ CProxy_Main mainProxy;
/*readonly*/ CProxy_Elem elemProxy;
/*readonly*/ CkGroupID mcastGid;
/*readonly*/ int numElems;
/*readonly*/ int useLB;
/*readonly*/ int rootHops;
/*readonly*/ int autoDelegate;

static const double kWatchdogSecs = 20.0;

static inline bool isMember(int i) { return i % 3 != 2; }

static inline unsigned long long mix64(unsigned long long z) {
  z += 0x9E3779B97F4A7C15ULL;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

struct WorkMsg : public CkMcastBaseMsg, public CMessage_WorkMsg {
  int round;
  WorkMsg(int r) : round(r) {}
};

static void watchdogFire(void*, double) { mainProxy.checkProgress(); }

static CkCallback* newClient() {
  return new CkCallback(CkIndex_Main::redDone(NULL), mainProxy);
}

class Main : public CBase_Main {
  int numRounds = 12;
  bool inflight = false;
  bool every = false;
  bool rootInFlight = false;
  bool stillRoot = false;
  int round = 0;
  int redSeen = 0;      // bit k: reduction 2*round+k arrived
  bool migDone = false;
  int syncsLeft = 0;
  int memberCount = 0, sumIdx = 0, maxIdx = -1;
  double lastProgress;

  void progress() { lastProgress = CkWallTimer(); }

  bool inFlightRound() { return inflight && (every || round % 2 == 1); }

  void triggerMigration(bool inFlight) {
    migDone = false;
    if (useLB) {
      syncsLeft = rootHops;
      elemProxy.doSync();
    } else {
      elemProxy.hop(round, stillRoot || (inFlight && !rootInFlight));
      CkStartQD(CkCallback(CkIndex_Main::qdDone(), thisProxy));
    }
  }

  void maybeNext() {
    if (redSeen != 3 || !migDone) return;
    round++;
    thisProxy.startRound();
  }

public:
  Main(CkArgMsg* m) {
    numElems = 24;
    useLB = 0;
    rootHops = 1;
    autoDelegate = 0;
    for (int i = 1; i < m->argc; i++) {
      if (!strcmp(m->argv[i], "-lb")) useLB = 1;
      else if (!strcmp(m->argv[i], "-inflight")) inflight = true;
      else if (!strcmp(m->argv[i], "-double")) rootHops = 2;
      else if (!strcmp(m->argv[i], "-every")) every = true;
      else if (!strcmp(m->argv[i], "-root")) rootInFlight = true;
      else if (!strcmp(m->argv[i], "-stillroot")) stillRoot = true;
      else if (!strcmp(m->argv[i], "-auto")) autoDelegate = 1;
      else if (!strcmp(m->argv[i], "-r") && i + 1 < m->argc) numRounds = atoi(m->argv[++i]);
      else if (!strcmp(m->argv[i], "-n") && i + 1 < m->argc) numElems = atoi(m->argv[++i]);
      else CkAbort("section_migrate: unknown argument %s", m->argv[i]);
    }
    delete m;
    for (int i = 0; i < numElems; i++)
      if (isMember(i)) { memberCount++; sumIdx += i; maxIdx = i; }
    CkPrintf("section_migrate: %d PEs, %d processes, %d elements (%d members), "
             "%d rounds, %s%s%s%s, %s delegation\n", CkNumPes(), CkNumNodes(),
             numElems, memberCount, numRounds, useLB ? "AtSync LB" : "migrateMe",
             inflight ? (every ? ", in flight every round" : ", in flight in odd rounds") : "",
             rootInFlight ? " including the root" : stillRoot ? ", root stays" : "",
             rootHops == 2 ? ", root hops twice" : "",
             autoDelegate ? "automatic" : "manual");
    mainProxy = thisProxy;
    mcastGid = CProxy_CkMulticastMgr::ckNew(2);
    elemProxy = CProxy_Elem::ckNew(numElems);
    progress();
    CcdCallFnAfter(watchdogFire, NULL, 1000);
    CkStartQD(CkCallback(CkIndex_Main::setup(), thisProxy));
  }

  void setup() { elemProxy[0].makeSection(); }

  void sectionReady() { round = 0; startRound(); }

  void startRound() {
    progress();
    if (round == numRounds) {
      elemProxy.finish(numRounds);
      return;
    }
    redSeen = 0;
    migDone = false;
    elemProxy[0].send(round);
    if (inFlightRound()) triggerMigration(true);
  }

  void redDone(CkReductionMsg* m) {
    progress();
    int redNo = m->getRedNo();
    int kind = redNo % 2;
    int* v = (int*)m->getData();
    int n = m->getSize() / sizeof(int);
    if (redNo / 2 != round || (redSeen & (1 << kind)))
      CkAbort("section_migrate: round %d got reduction number %d (already seen "
              "0x%x); expected %d or %d once each", round, redNo, redSeen,
              2 * round, 2 * round + 1);
    if (kind == 0) {
      if (n != 3 || v[0] != sumIdx || v[1] != memberCount || v[2] != memberCount * round)
        CkAbort("section_migrate: round %d sum: got {%d,%d,%d}, expected {%d,%d,%d}",
                round, v[0], v[1], v[2], sumIdx, memberCount, memberCount * round);
    } else {
      if (n != 2 || v[0] != maxIdx || v[1] != round)
        CkAbort("section_migrate: round %d max: got {%d,%d}, expected {%d,%d}",
                round, v[0], v[1], maxIdx, round);
    }
    delete m;
    redSeen |= 1 << kind;
    if (redSeen == 3 && !inFlightRound()) triggerMigration(false);
    maybeNext();
  }

  void qdDone() { migDone = true; maybeNext(); }

  void synced() {
    progress();
    if (--syncsLeft > 0) { elemProxy.doSync(); return; }
    migDone = true;
    maybeNext();
  }

  void finished(int members) {
    if (members != memberCount)
      CkAbort("section_migrate: %d members finished, expected %d", members, memberCount);
    CkPrintf("section_migrate PASS\n");
    CkExit();
  }

  void checkProgress() {
    double idle = CkWallTimer() - lastProgress;
    // One tick before the abort, every element prints its state.
    if (idle > kWatchdogSecs - 1.5 && idle <= kWatchdogSecs - 0.5)
      elemProxy.report();
    if (idle > kWatchdogSecs) {
      CkAbort("section_migrate: no progress for %.0f s at round %d (reductions "
              "seen 0x%x, migration %s)", kWatchdogSecs, round, redSeen,
              migDone ? "done" : "pending");
    }
    CcdCallFnAfter(watchdogFire, NULL, 1000);
  }
};

static CkMulticastMgr* mcastMgr(const CkArrayID& aid) {
  CkGroupID g = autoDelegate ? CProxy_CkArray(aid).ckLocalBranch()->getmCastMgr()
                             : mcastGid;
  return CProxy_CkMulticastMgr(g).ckLocalBranch();
}

class Elem : public CBase_Elem {
  CProxySection_Elem sec;   // element 0 only
  bool hasSection = false;
  CkSectionInfo cookie;
  int nextRound = 0;
  int worksReceived = 0;
  int hopsLeft = 0, hopRound = 0;

  int pickDest(int r, int h) {
    int P = CkNumPes();
    int step = 1 + (int)(mix64(((unsigned long long)(unsigned)r << 40) ^
                               ((unsigned long long)(unsigned)h << 32) ^
                               (unsigned)thisIndex) % (P - 1));
    return (CkMyPe() + step) % P;
  }

public:
  Elem() { usesAtSync = useLB; }
  Elem(CkMigrateMessage* m) : CBase_Elem(m) {}

  void pup(PUP::er& p) {
    p | sec; p | hasSection; p | cookie;
    p | nextRound; p | worksReceived; p | hopsLeft; p | hopRound;
    if (p.isUnpacking() && hasSection) {
      if (autoDelegate) {
        sec.resetSection();
        sec.setReductionClient(newClient());
      } else {
        CkMulticastMgr* mg = mcastMgr(sec.ckGetArrayID());
        mg->resetSection(sec);
        mg->setReductionClient(sec, newClient());
      }
    }
  }

  void makeSection() {
    std::vector<CkArrayIndex> members;
    for (int i = 0; i < numElems; i++)
      if (isMember(i)) members.emplace_back(CkArrayIndex1D(i));
    sec = CProxySection_Elem::ckNew(thisProxy.ckGetArrayID(), members);
    if (autoDelegate) {
      sec.setReductionClient(newClient());
    } else {
      CkMulticastMgr* mg = mcastMgr(thisProxy.ckGetArrayID());
      sec.ckSectionDelegate(mg);
      mg->setReductionClient(sec, newClient());
    }
    hasSection = true;
    mainProxy.sectionReady();
  }

  void send(int r) { sec.work(new WorkMsg(r)); }

  void work(WorkMsg* m) {
    int r = m->round;
    if (!isMember(thisIndex))
      CkAbort("section_migrate: element %d is not a member but got round %d", thisIndex, r);
    if (r != nextRound)
      CkAbort("section_migrate: element %d got round %d, expected %d", thisIndex, r, nextRound);
    nextRound++;
    worksReceived++;
    CkGetSectionInfo(cookie, m);
    delete m;
    CkMulticastMgr* mg = mcastMgr(thisProxy.ckGetArrayID());
    int s[3] = { thisIndex, 1, r };
    mg->contribute(sizeof(s), s, CkReduction::sum_int, cookie);
    int x[2] = { thisIndex, r };
    mg->contribute(sizeof(x), x, CkReduction::max_int, cookie);
  }

  // Element 0 hops rootHops times in a row, the others once.
  void hop(int r, bool skipRoot) {
    if (CkNumPes() < 2 || (skipRoot && thisIndex == 0)) return;
    hopsLeft = (thisIndex == 0 ? rootHops : 1) - 1;
    hopRound = r;
    migrateMe(pickDest(r, hopsLeft));
  }

  void hopAgain() {
    hopsLeft--;
    migrateMe(pickDest(hopRound, hopsLeft));
  }

  void ckJustMigrated() {
    CBase_Elem::ckJustMigrated();
    if (hopsLeft > 0) thisProxy[thisIndex].hopAgain();
  }

  void doSync() { AtSync(); }

  void report() {
    CkPrintf("section_migrate: element %d on PE %d: next round %d, received %d, "
             "cookie PE %d redNo %d%s\n", thisIndex, CkMyPe(), nextRound,
             worksReceived, cookie.get_pe(), cookie.get_redNo(),
             hasSection ? " (section root)" : "");
  }

  void ResumeFromSync() {
    contribute(CkCallback(CkReductionTarget(Main, synced), mainProxy));
  }

  void finish(int rounds) {
    int expected = isMember(thisIndex) ? rounds : 0;
    if (worksReceived != expected)
      CkAbort("section_migrate: element %d received %d work messages, expected %d",
              thisIndex, worksReceived, expected);
    int one = isMember(thisIndex) ? 1 : 0;
    contribute(sizeof(int), &one, CkReduction::sum_int,
               CkCallback(CkReductionTarget(Main, finished), mainProxy));
  }
};

#include "section_migrate.def.h"

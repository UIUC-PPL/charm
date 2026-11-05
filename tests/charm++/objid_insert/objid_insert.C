// objid_insert: insertion, location lookup, stale-cache forwarding, demand
// creation and deletion of chare array elements under the 64-bit object id
// redesign (charm #3994, doc/objid64-design.md).
//
// Run twice: default = unbounded 2D array (hashed id kind, design section 3:
// ids minted from a per-process tranche, home = rank 0 of process
// indexHashKey(idx) % CkNumNodes(), section 3.3); -b = setBounds(1024,1024)
// (packed kind, section 2: the id is the packed index, home = map homePe).
//
// What is checked, and why:
//  Phase 1  Every PE inserts K elements (pe,k) locally and K elements
//           (pe+100,k) on the next PE. A local insert must be synchronous
//           (section 5, goal G5): ckLocal() is non-null right after insert.
//           Every element registers (index, element id, PE) with main, which
//           checks that all ids are distinct (section 3.1 uniqueness).
//  Phase 2  "Cold sends": every PE sends ping(round, fromPe) by index to every
//           element, including elements it never touched, so the source has
//           no id or location and must go through the home (section 4.1,
//           4.3; sendMsg -> handleUnknown -> bufferForLocation). Each element
//           enforces that each sender arrives exactly once per round; main
//           checks the ping total.
//  Phase 3  Elements migrate to pseudo-random other PEs, then phase 2 runs
//           over one-hop-stale caches; then two migrations back to back and
//           phase 2 again over two-hop-stale caches, which exercises the
//           "more than one hop -> forward to homePe(id)" rule in
//           CkArray::recvMsg (section 4.3). Five cycles, different seeds.
//  Phase 4  Demand creation, [createhome]: revive sent from every PE to
//           indices (300+pe,k) that were never inserted; each must be created
//           exactly once, at its home PE (section 5, "Demand creation").
//           With -n only PEs other than the home send, so the creation must
//           come from a message that reaches the home carrying only an id
//           (packed kind: the sender has the id and forwards; recvMsg ->
//           handleUnknownByID at the home). Then phase 2 over the enlarged set.
//  Phase 4b Demand creation, [createhere]: reviveHere sent to indices
//           (500+pe,k) by ONE PE each, the PE after the index's home; the
//           element must be created exactly once, on that sender. One sender
//           because two concurrent createhere requesters are not serialized by
//           the home (a limitation that predates the redesign). Then phase 2.
//  Phase 5  Half the elements ckDestroy() themselves; phase 2 over the
//           survivors (section 4.2 reclaimRemote, section 5 "Deletion").
//  Phase 6  Re-creation of the destroyed elements by revive (createhome,
//           every PE sends): every sender still holds their ids and stale
//           locations, so the messages arrive by id at a PE that has no
//           record of the element, and for the hashed kind cannot name its
//           index; only the home can (section 3.4). Each must be created
//           exactly once, at its home. Then phase 2 over everything and a
//           final count.
//  Phase 7  Re-creation under a new id (needs 3 or more PEs). Phase 1 also
//           inserts K "fresh" elements (700+pe,k) on each PE. One PE per fresh
//           index, freshPe(x,y) (neither its inserter nor its home), never
//           sends to it, and fresh elements never migrate or die in phase 5,
//           so that PE never learns the index's id. Index bindings are shared
//           by the PEs of a process (doc/internals/process-location-cache.md),
//           so when a process holds neither the inserter nor the home the
//           fresh PE is taken from it and its whole process abstains. Phase 7
//           destroys every fresh element, then freshPe(x,y) inserts (x,y)
//           locally: with such an isolated process the hashed kind has no
//           idx -> id binding there and mints a NEW id (main enforces it
//           differs from the old one), while every other process still holds
//           the old id and a stale location; without one (fewer than three
//           processes) the binding is known and the old id must be reused, as
//           in the packed kind, where the id is the packed index. Then every PE pings every element, so each old-id
//           holder's message lands on a PE with no element for that id and
//           must reach the new one: forwarded to the home and re-addressed
//           there by default, or, under +messageBufferingThreshold 0 (every
//           message counts as large), held where it lands until the home's
//           reply by index names the new id, and re-addressed there
//           (CkLocMgr::retiredIds, CkArray::releaseRetiredMsgs). A second ping
//           round runs over the bindings the first one taught.
//           Re-creation by createhere is NOT tested: it fails for two reasons
//           that predate the redesign and are out of its scope. The creating
//           PE's insertElement follows its own stale location entry for the
//           dead element (the isRemote redirect meant for bound siblings) and
//           forwards the constructor elsewhere; and a re-created element
//           starts again at epoch 0, so every stale cache rejects its location
//           (debug builds assert in CkLocCache::insert on the creating PE).
//
// Options: -b packed kind; -k K elements per PE per pattern (default 16);
//          +messageBufferingThreshold 0 (runtime option): every message is held
//             at a PE without the element instead of being forwarded;
//          -n phase 4 and 6 createhome senders exclude the home PE (needs
//             2 or more PEs);
//          -w watchdog stall seconds (default 60, 0 disables);
//          -D skip demand creation (phases 4, 4b, 6).
//
// Phase 4 in the hashed kind exposed a defect that predates the redesign: a
// non-home source with no id for a never-created index buffers the message
// (bufferForCreation) and sends requestDemandCreation(idx, ctor, pe = home) to
// the home, which creates the element but, for createhome, told nobody; the
// requester's bufferedCreationMsgs entry was never flushed, so only the home
// PE's own revive arrived. The packed kind never hit it because the source
// has the id and forwards the small message to the home. Fixed alongside this
// test: bufferForCreation now also requests the location by index, which the
// home answers once the element exists.
//
// -n, phase 4b and phase 6 were added for the three regressions Aditya found
// in PR 1a (#4017): a message that reaches the home by id did not trigger
// demand creation (parked in bufferedIDMsgs for a location nothing would
// report); the home answered an index-keyed location request for an element
// that did not exist yet with the null entry (pe -1, id 0), which the
// createhere path of 4b provokes on a packed array in a debug build; and the
// hashed kind had no way back from an id to its index at the home.
#include <map>
#include <set>
#include <vector>
#include <utility>
#include <stdio.h>
#include "pup_stl.h"
#include "objid_insert.decl.h"

/* readonly */ CProxy_Main mainProxy;
/* readonly */ CProxy_Elem arrProxy;
/* readonly */ CProxy_Inserter inserterProxy;
/* readonly */ int K;
/* readonly */ int bounded;
/* readonly */ int nonHomeOnly;
/* readonly */ int destroySeed;
/* readonly */ int doFresh;  // phase 7 runs (3 or more PEs)

static unsigned long long mix64(unsigned long long z) {
  z += 0x9e3779b97f4a7c15ULL;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}
static unsigned long long mixIdx(int x, int y, int seed) {
  return mix64(((unsigned long long)(unsigned)x << 32) ^ (unsigned)y ^
               mix64((unsigned long long)(unsigned)seed));
}
// Destination of a migration: never the current PE.
static int migDest(int x, int y, int seed, int cur) {
  int P = CkNumPes();
  return (cur + 1 + (int)(mixIdx(x, y, seed) % (unsigned long long)(P - 1))) % P;
}
static bool diesAt(int x, int y, int seed) { return (mixIdx(x, y, seed) >> 7) & 1; }

// How an index is (re)created in a demand phase, from the index alone, so that
// every PE and the element itself agree without a message.
enum Kind { INSERTED, CREATEHOME, CREATEHERE, FRESH };
static Kind kindOf(int x) {
  if (x >= 700) return FRESH;  // (700+pe,k), inserted in phase 1, re-inserted in phase 7
  if (x >= 500) return CREATEHERE;
  if (x >= 300) return CREATEHOME;
  return INSERTED;  // (pe,k) and (pe+100,k), inserted in phase 1
}
// Whether a demand creation of (x,y) must use createhere. A destroyed element
// (phase 6) always comes back by createhome: see the header on createhere.
static bool createsHere(int x, int y) { return kindOf(x) == CREATEHERE; }
static int homeOf(int x, int y) {
  return arrProxy.ckLocMgr()->homePe(CkArrayIndex2D(x, y));
}
// Phase 7: the PE that never addresses fresh index (x,y) before re-inserting it
// there: not its inserter (x-700) and not its home, which always knows the id.
// Index -> id bindings are per process (the shared index table), so the PE is
// taken from a process holding neither the inserter nor the home when one exists
// (freshIsolated); then no PE of that process addresses (x,y) and the re-insertion
// mints a new id. With fewer processes the fresh PE's process knows the binding
// and the re-insertion reuses the old id, which is checked instead.
static int freshPe(int x, int y) {
  int P = CkNumPes(), ins = x - 700, h = homeOf(x, y);
  for (int f = (ins + 1) % P, n = 0; n < P; f = (f + 1) % P, n++)
    if (CkNodeOf(f) != CkNodeOf(ins) && CkNodeOf(f) != CkNodeOf(h)) return f;
  int f = (ins + 1) % P;
  while (f == ins || f == h) f = (f + 1) % P;
  return f;
}
static bool freshIsolated(int x, int y) {
  int f = freshPe(x, y);
  return CkNodeOf(f) != CkNodeOf(x - 700) && CkNodeOf(f) != CkNodeOf(homeOf(x, y));
}
// The PEs that leave fresh (x,y) alone before its re-insertion: the fresh PE's
// whole process when it is isolated, else the fresh PE only.
static bool skipsFresh(int x, int y, int pe) {
  int f = freshPe(x, y);
  return freshIsolated(x, y) ? CkNodeOf(pe) == CkNodeOf(f) : pe == f;
}
static int freshSkippers(int x, int y) {
  return freshIsolated(x, y) ? CkNodeSize(CkNodeOf(freshPe(x, y))) : 1;
}
// The single PE that sends a createhere message to (x,y): the PE after its home.
static int hereSender(int x, int y) { return (homeOf(x, y) + 1) % CkNumPes(); }
// The PE a demand-created (x,y) must be constructed on.
static int creationPe(int x, int y) {
  return createsHere(x, y) ? hereSender(x, y) : homeOf(x, y);
}
// How many revive/reviveHere messages (x,y) receives in a demand phase.
static int sendersTo(int x, int y) {
  if (createsHere(x, y)) return 1;
  return nonHomeOnly ? CkNumPes() - 1 : CkNumPes();
}

static void watchdogFire(void* arg, double t);
static void abortFire(void* arg, double t) {
  fflush(stdout);
  CkAbort("objid_insert: hung (state dumped above)");
}

enum Action { PING, MIGRATE, REVIVE, REVIVE_HERE, REVIVE_DEAD, REPORT, DESTROY,
              DESTROY_FRESH, REINSERT_FRESH, DONE };
struct Step { Action a; int seed; const char* what; };

class Main : public CBase_Main {
  typedef std::pair<int, int> Key;
  struct Reg { CmiUInt8 id; int pe; int demand; };
  std::map<Key, Reg> regs;
  std::set<CmiUInt8> ids;
  std::set<Key> live;
  std::map<Key, int> expRevives;  // demand-created elements: revives expected
  std::vector<Key> dead;          // destroyed in phase 5, revived in phase 6
  std::map<Key, CmiUInt8> oldFreshIds;  // phase 7: ids before the re-insertion
  bool freshOpen = false;         // phase 7: freshPe(x,y) sends to (x,y) too
  std::vector<Key> pendingDemand; // targets of the demand step in progress
  std::vector<Step> script;
  size_t cur = 0;
  int round = 0;
  int nMigrateRounds = 0;
  bool doDemand = true;
  int wdSecs = 60;
  double lastAdvance = 0;
  bool dumped = false;
  int P;

public:
  Main(CkArgMsg* m) {
    setvbuf(stdout, NULL, _IONBF, 0);
    K = 16;
    bounded = CmiGetArgFlag(m->argv, "-b") ? 1 : 0;
    nonHomeOnly = CmiGetArgFlag(m->argv, "-n") ? 1 : 0;
    destroySeed = 77;
    CmiGetArgInt(m->argv, "-k", &K);
    CmiGetArgInt(m->argv, "-w", &wdSecs);
    doDemand = !CmiGetArgFlag(m->argv, "-D");
    delete m;
    P = CkNumPes();
    CkEnforce(K >= 1 && K <= 1024);
    CkEnforce(P <= 300);  // indices 700+pe must stay inside 1024 bounds
    if (nonHomeOnly) CkEnforce(P >= 2);  // someone other than the home must send
    doFresh = P >= 3 ? 1 : 0;  // freshPe must differ from the inserter and the home
    CkPrintf("objid_insert: %d PEs, %d processes, K=%d, %s kind%s%s\n", P,
             CkNumNodes(), K, bounded ? "packed (setBounds 1024x1024)" : "hashed (unbounded)",
             doDemand ? "" : ", demand creation skipped (-D)",
             nonHomeOnly ? ", createhome senders exclude the home (-n)" : "");
    mainProxy = thisProxy;
    CkArrayOptions opts;
    if (bounded) opts.setBounds(1024, 1024);
    arrProxy = CProxy_Elem::ckNew(opts);
    inserterProxy = CProxy_Inserter::ckNew();

    script.push_back({PING, 0, "cold sends"});
    if (P > 1) {
      for (int c = 0; c < 5; c++) {
        int s = 1000 + 10 * c;
        script.push_back({MIGRATE, s, "migrate"});
        script.push_back({PING, 0, "one-hop-stale sends"});
        script.push_back({MIGRATE, s + 1, "migrate"});
        script.push_back({MIGRATE, s + 2, "migrate"});
        script.push_back({PING, 0, "two-hop-stale sends"});
      }
    } else {
      CkPrintf("objid_insert: 1 PE, migration phases skipped\n");
    }
    if (doDemand) {
      script.push_back({REVIVE, 0, "demand creation (createhome)"});
      script.push_back({REPORT, 0, "demand-creation report"});
      script.push_back({PING, 0, "sends after demand creation"});
      script.push_back({REVIVE_HERE, 0, "demand creation (createhere)"});
      script.push_back({REPORT, 0, "createhere report"});
      script.push_back({PING, 0, "sends after createhere"});
    }
    script.push_back({DESTROY, destroySeed, "destroy half"});
    script.push_back({PING, 0, "sends to survivors"});
    if (doDemand) {
      script.push_back({REVIVE_DEAD, 0, "re-creation of destroyed elements"});
      script.push_back({PING, 0, "sends after re-creation"});
    }
    if (doFresh) {
      script.push_back({DESTROY_FRESH, 0, "destroy the fresh elements"});
      script.push_back({REINSERT_FRESH, 0, "re-insertion on a PE that never saw the index"});
      script.push_back({PING, 0, "sends with old ids"});
      script.push_back({PING, 0, "sends after the old ids were re-bound"});
    } else {
      CkPrintf("objid_insert: fewer than 3 PEs, phase 7 (re-creation under a new id) skipped\n");
    }
    script.push_back({REPORT, 0, "final report"});
    script.push_back({DONE, 0, "done"});

    lastAdvance = CkWallTimer();
    if (wdSecs > 0) CcdCallFnAfter(watchdogFire, NULL, 2000);
    inserterProxy.phase1();
  }

  void progress() { lastAdvance = CkWallTimer(); }

  void registerElem(int x, int y, CmiUInt8 id, int pe, int demand) {
    progress();
    Key key(x, y);
    if (regs.count(key)) {
      CkPrintf("objid_insert FAIL: element (%d,%d) registered twice (ids %llx on PE %d, "
               "%llx on PE %d)\n", x, y, (unsigned long long)regs[key].id, regs[key].pe,
               (unsigned long long)id, pe);
      CkAbort("duplicate element");
    }
    if (!ids.insert(id).second) {
      CkPrintf("objid_insert FAIL: element id %llx of (%d,%d) is not unique\n",
               (unsigned long long)id, x, y);
      CkAbort("duplicate id");
    }
    regs[key] = Reg{id, pe, demand};
    live.insert(key);
  }

  void phase1Done() {
    progress();
    arrProxy.doneInserting();
    CkStartQD(CkCallback(CkIndex_Main::quiescent(), thisProxy));
    cur = (size_t)-1;  // quiescent() advances to script[0]
  }

  // After a demand step is quiescent: every target exists, once, demand-created,
  // on the PE its creation mode prescribes.
  void checkDemand(const char* what) {
    for (const Key& key : pendingDemand) {
      auto it = regs.find(key);
      if (it == regs.end()) {
        CkPrintf("objid_insert FAIL: %s: (%d,%d) was never created\n", what, key.first,
                 key.second);
        CkAbort("demand creation lost");
      }
      int want = creationPe(key.first, key.second);
      if (it->second.demand != 1 || it->second.pe != want) {
        CkPrintf("objid_insert FAIL: %s: (%d,%d) created on PE %d (demand %d), expected "
                 "PE %d (%s)\n", what, key.first, key.second, it->second.pe,
                 it->second.demand, want,
                 createsHere(key.first, key.second) ? "createhere sender" : "home");
        CkAbort("demand creation on the wrong PE");
      }
    }
    pendingDemand.clear();
  }

  void quiescent() {
    progress();
    if (cur == (size_t)-1) {
      CkEnforce(regs.size() == (size_t)((2 + doFresh) * K * P));
      for (int pe = 0; pe < P; pe++)
        for (int k = 0; k < K; k++) {
          auto a = regs.find(Key(pe, k));
          auto b = regs.find(Key(pe + 100, k));
          CkEnforce(a != regs.end() && b != regs.end());
          CkEnforce(a->second.pe == pe);
          CkEnforce(b->second.pe == (pe + 1) % P);
          if (doFresh) {
            auto f = regs.find(Key(pe + 700, k));
            CkEnforce(f != regs.end() && f->second.pe == pe);
          }
        }
      CkPrintf("objid_insert: phase 1 ok, %zu elements, ids distinct\n", regs.size());
      cur = 0;
      runStep();
      return;
    }
    Step& s = script[cur];
    if (s.a == REVIVE || s.a == REVIVE_HERE || s.a == REVIVE_DEAD) checkDemand(s.what);
    if (s.a == REINSERT_FRESH) checkReinsertion();
    next();
  }

  // Phase 7, after the re-insertion is quiescent: every fresh element exists
  // again, on its freshPe, and in the hashed kind under a NEW id (the proof that
  // the re-creating PE had never seen the index); in the packed kind the id is
  // a function of the index and must be the same.
  void checkReinsertion() {
    for (auto& e : oldFreshIds) {
      const Key& key = e.first;
      auto it = regs.find(key);
      if (it == regs.end()) {
        CkPrintf("objid_insert FAIL: phase 7: (%d,%d) was not re-inserted\n", key.first,
                 key.second);
        CkAbort("re-insertion lost");
      }
      int want = freshPe(key.first, key.second);
      CkEnforce(it->second.demand == 0);
      if (it->second.pe != want) {
        CkPrintf("objid_insert FAIL: phase 7: (%d,%d) re-inserted on PE %d, expected %d\n",
                 key.first, key.second, it->second.pe, want);
        CkAbort("re-insertion on the wrong PE");
      }
      bool same = it->second.id == e.second;
      bool expectSame = bounded || !freshIsolated(key.first, key.second);
      if (same != expectSame) {
        CkPrintf("objid_insert FAIL: phase 7: (%d,%d) re-inserted on PE %d with id %llx, "
                 "old id %llx: %s\n", key.first, key.second, it->second.pe,
                 (unsigned long long)it->second.id, (unsigned long long)e.second,
                 bounded ? "a packed id must not change"
                 : expectSame ? "the fresh PE's process knew the index, so the id must be reused"
                              : "the fresh PE's process reused the old id, so it had seen the index");
        CkAbort("re-insertion id");
      }
    }
    CkPrintf("objid_insert: phase 7: %zu fresh elements re-inserted, %s\n",
             oldFreshIds.size(),
             bounded ? "ids unchanged (packed)"
             : freshIsolated(700, 0) ? "all under new ids (isolated fresh processes)"
                                     : "ids reused (the fresh PEs' processes knew the bindings)");
    arrProxy.doneInserting();
    freshOpen = true;
  }

  void next() { cur++; runStep(); }

  // Send a demand phase: every PE decides per target whether it is a sender.
  void demand(const std::vector<Key>& targets, const char* what) {
    std::vector<int> xs, ys;
    std::vector<char> here;
    for (const Key& key : targets) {
      xs.push_back(key.first);
      ys.push_back(key.second);
      here.push_back(createsHere(key.first, key.second) ? 1 : 0);
      expRevives[key] = sendersTo(key.first, key.second);
    }
    pendingDemand = targets;
    CkPrintf("objid_insert: %s: %zu elements\n", what, targets.size());
    inserterProxy.sendDemand(xs, ys, here);
    CkStartQD(CkCallback(CkIndex_Main::quiescent(), thisProxy));
  }

  void runStep() {
    progress();
    Step& s = script[cur];
    switch (s.a) {
      case PING: {
        round++;
        std::vector<int> xs, ys;
        for (auto& e : live) { xs.push_back(e.first); ys.push_back(e.second); }
        CkPrintf("objid_insert: round %d (%s) over %zu elements\n", round, s.what,
                 live.size());
        inserterProxy.sendPings(round, freshOpen ? 1 : 0, xs, ys);
        break;
      }
      case MIGRATE:
        nMigrateRounds++;
        arrProxy.migrateNow(s.seed);
        CkStartQD(CkCallback(CkIndex_Main::quiescent(), thisProxy));
        break;
      case REVIVE: {
        std::vector<Key> t;
        for (int pe = 0; pe < P; pe++)
          for (int k = 0; k < K; k++) t.push_back(Key(300 + pe, k));
        demand(t, s.what);
        break;
      }
      case REVIVE_HERE: {
        std::vector<Key> t;
        for (int pe = 0; pe < P; pe++)
          for (int k = 0; k < K; k++) t.push_back(Key(500 + pe, k));
        demand(t, s.what);
        break;
      }
      case REVIVE_DEAD:
        demand(dead, s.what);
        break;
      case REPORT:
        arrProxy.report();
        break;
      case DESTROY_FRESH: {
        for (auto it = live.begin(); it != live.end();)
          if (kindOf(it->first) == FRESH) {
            oldFreshIds[*it] = regs[*it].id;
            ids.erase(regs[*it].id);
            regs.erase(*it);
            it = live.erase(it);
          } else {
            ++it;
          }
        CkEnforce(oldFreshIds.size() == (size_t)(K * P));
        CkPrintf("objid_insert: destroying the %zu fresh elements\n", oldFreshIds.size());
        arrProxy.dieIfFresh();
        CkStartQD(CkCallback(CkIndex_Main::quiescent(), thisProxy));
        break;
      }
      case REINSERT_FRESH: {
        std::vector<int> xs, ys;
        for (auto& e : oldFreshIds) { xs.push_back(e.first.first); ys.push_back(e.first.second); }
        CkPrintf("objid_insert: re-inserting %zu fresh elements, each on a PE that never "
                 "addressed it\n", xs.size());
        inserterProxy.reinsertFresh(xs, ys);
        CkStartQD(CkCallback(CkIndex_Main::quiescent(), thisProxy));
        break;
      }
      case DESTROY: {
        size_t before = live.size();
        for (auto it = live.begin(); it != live.end();)
          if (kindOf(it->first) != FRESH && diesAt(it->first, it->second, s.seed)) {
            // Forget the element: a re-creation registers afresh, and in the
            // packed kind with the same id.
            dead.push_back(*it);
            ids.erase(regs[*it].id);
            regs.erase(*it);
            expRevives.erase(*it);
            it = live.erase(it);
          } else {
            ++it;
          }
        CkEnforce(!live.empty() && live.size() < before);
        CkPrintf("objid_insert: destroying %zu of %zu elements\n", before - live.size(),
                 before);
        arrProxy.maybeDie(s.seed);
        CkStartQD(CkCallback(CkIndex_Main::quiescent(), thisProxy));
        break;
      }
      case DONE:
        CkPrintf("objid_insert PASS (%s kind, %d PEs, %d processes, %d ping rounds, "
                 "%d migration rounds%s)\n", bounded ? "packed" : "hashed", P,
                 CkNumNodes(), round, nMigrateRounds, nonHomeOnly ? ", -n" : "");
        CkExit();
        break;
    }
  }

  // Senders of a ping round to (x,y): every PE, except that freshPe(x,y) leaves
  // a fresh element alone until it has re-inserted it (phase 7).
  int pingSenders(const Key& key) const {
    return (kindOf(key.first) == FRESH && !freshOpen) ? P - freshSkippers(key.first, key.second)
                                                      : P;
  }

  void pingDone(int nElems, int nPings) {
    progress();
    int want = 0;
    for (auto& e : live) want += pingSenders(e);
    if (nElems != (int)live.size() || nPings != want) {
      CkPrintf("objid_insert FAIL: round %d: %d elements reported %d pings, expected "
               "%zu elements, %d pings\n", round, nElems, nPings, live.size(), want);
      CkAbort("ping count mismatch");
    }
    next();
  }

  void reportDone(int nElems, int nRevives, int nDemand, int nMigrations) {
    progress();
    int wantDemand = 0, wantRevives = 0;
    for (auto& r : regs) wantDemand += r.second.demand;
    for (auto& e : expRevives) wantRevives += e.second;
    CkPrintf("objid_insert: report: %d elements, %d revives, %d demand-created, "
             "%d migrations (expected %zu, %d, %d)\n", nElems, nRevives, nDemand,
             nMigrations, live.size(), wantRevives, wantDemand);
    CkEnforce(nElems == (int)live.size());
    CkEnforce(nDemand == wantDemand);
    CkEnforce(nRevives == wantRevives);
    next();
  }

  void checkProgress() {
    if (!dumped && wdSecs > 0 && CkWallTimer() - lastAdvance > (double)wdSecs) {
      dumped = true;
      CkPrintf("objid_insert WATCHDOG: no progress for %d s; script step %zu (%s), "
               "round %d, %zu registered, %zu live. Dumps:\n", wdSecs, cur,
               cur < script.size() ? script[cur].what : "phase 1", round, regs.size(),
               live.size());
      for (const Key& key : pendingDemand)
        if (!regs.count(key))
          CkPrintf("objid_insert WATCHDOG:   (%d,%d) not created; expected on PE %d\n",
                   key.first, key.second, creationPe(key.first, key.second));
      inserterProxy.dumpState();
      arrProxy.dumpState();
      CcdCallFnAfter(abortFire, NULL, 3000);
      return;
    }
    CcdCallFnAfter(watchdogFire, NULL, 2000);
  }
};

class Inserter : public CBase_Inserter {
  int lastRound = 0;
public:
  Inserter() {}

  void phase1() {
    int me = CkMyPe(), P = CkNumPes();
    for (int k = 0; k < K; k++) {
      arrProxy(me, k).insert(0, me);
      // Local insertion is synchronous (design section 5, goal G5).
      CkEnforce(arrProxy(me, k).ckLocal() != nullptr);
    }
    for (int k = 0; k < K; k++) arrProxy(me + 100, k).insert(0, (me + 1) % P);
    if (doFresh)
      for (int k = 0; k < K; k++) arrProxy(me + 700, k).insert(0, me);
    contribute(CkCallback(CkReductionTarget(Main, phase1Done), mainProxy));
  }

  void sendPings(int round, int freshOpen, std::vector<int> xs, std::vector<int> ys) {
    lastRound = round;
    for (size_t i = 0; i < xs.size(); i++) {
      int x = xs[i], y = ys[i];
      // Phase 7: this PE must not learn a fresh index's id before re-inserting it.
      if (kindOf(x) == FRESH && !freshOpen && skipsFresh(x, y, CkMyPe())) continue;
      arrProxy(x, y).ping(round, CkMyPe());
    }
  }

  // Phase 7: a local insertion on the PE that never addressed the index. Local,
  // so the id is minted here (CkLocMgr::tryNewObjectID) and not on a PE that
  // still holds the old binding.
  void reinsertFresh(std::vector<int> xs, std::vector<int> ys) {
    for (size_t i = 0; i < xs.size(); i++) {
      int x = xs[i], y = ys[i];
      if (CkMyPe() != freshPe(x, y)) continue;
      arrProxy(x, y).insert(1, CkMyPe());
      CkEnforce(arrProxy(x, y).ckLocal() != nullptr);
    }
  }

  void sendDemand(std::vector<int> xs, std::vector<int> ys, std::vector<char> here) {
    for (size_t i = 0; i < xs.size(); i++) {
      int x = xs[i], y = ys[i];
      if (here[i]) {
        if (CkMyPe() == hereSender(x, y)) arrProxy(x, y).reviveHere(CkMyPe());
      } else {
        if (nonHomeOnly && CkMyPe() == homeOf(x, y)) continue;
        arrProxy(x, y).revive(CkMyPe());
      }
    }
  }

  void dumpState() {
    CkPrintf("objid_insert DUMP: Inserter PE %d lastRound %d local elements %u\n",
             CkMyPe(), lastRound, arrProxy.numLocalElements());
  }
};

class Elem : public CBase_Elem {
  std::map<int, std::vector<char>> pending;  // round -> senders seen
  std::vector<char> reviveFrom;
  int revives = 0;
  int demand = 0;
  int migrations = 0;
  int lastRound = 0;
  int reinserted = 0;  // phase 7's second incarnation: every PE pings it

  void registerMe() {
    mainProxy.registerElem(thisIndex.x, thisIndex.y, ckGetID().getElementID(), CkMyPe(),
                           demand);
  }

  void gotRevive(int fromPe, const char* how) {
    CkEnforce(demand == 1);
    CkEnforce(fromPe >= 0 && fromPe < CkNumPes());
    if (reviveFrom[fromPe]) {
      CkPrintf("objid_insert FAIL: (%d,%d) got %s from PE %d twice\n", thisIndex.x,
               thisIndex.y, how, fromPe);
      CkAbort("duplicate revive");
    }
    reviveFrom[fromPe] = 1;
    revives++;
  }

public:
  // origin 0: phase 1 insertion; 1: phase 7 re-insertion on freshPe.
  Elem(int origin) : reviveFrom(CkNumPes(), 0) {
    CkEnforce(thisIndex.x < 300 || kindOf(thisIndex.x) == FRESH);
    if (origin == 1) {
      CkEnforce(kindOf(thisIndex.x) == FRESH && CkMyPe() == freshPe(thisIndex.x, thisIndex.y));
      reinserted = 1;
    }
    registerMe();
  }
  // Demand creation (phases 4, 4b, 6): on the home PE for createhome, on the one
  // sender for createhere. An index below 300 is a re-creation after phase 5.
  Elem() : reviveFrom(CkNumPes(), 0) {
    demand = 1;
    if (kindOf(thisIndex.x) == INSERTED)
      CkEnforce(diesAt(thisIndex.x, thisIndex.y, destroySeed));
    int want = creationPe(thisIndex.x, thisIndex.y);
    if (CkMyPe() != want)
      CkPrintf("objid_insert FAIL: (%d,%d) demand-created on PE %d, expected PE %d (%s)\n",
               thisIndex.x, thisIndex.y, CkMyPe(), want,
               createsHere(thisIndex.x, thisIndex.y) ? "createhere sender" : "home");
    CkEnforce(CkMyPe() == want);
    registerMe();
  }
  Elem(CkMigrateMessage* m) : CBase_Elem(m) {}

  void pup(PUP::er& p) {
    p | pending;
    p | reviveFrom;
    p | revives;
    p | demand;
    p | migrations;
    p | lastRound;
    p | reinserted;
  }

  void ping(int round, int fromPe) {
    CkEnforce(fromPe >= 0 && fromPe < CkNumPes());
    std::vector<char>& seen = pending[round];
    if (seen.empty()) seen.assign(CkNumPes(), 0);
    if (seen[fromPe]) {
      CkPrintf("objid_insert FAIL: (%d,%d) got round %d ping from PE %d twice\n",
               thisIndex.x, thisIndex.y, round, fromPe);
      CkAbort("duplicate ping");
    }
    seen[fromPe] = 1;
    int n = 0;
    for (char c : seen) n += c;
    // A fresh element's first incarnation is never pinged by its freshPe (or its
    // freshPe's whole process when that process is isolated).
    int want = (kindOf(thisIndex.x) == FRESH && !reinserted)
                   ? CkNumPes() - freshSkippers(thisIndex.x, thisIndex.y)
                   : CkNumPes();
    if (n == want) {
      pending.erase(round);
      lastRound = round;
      int v[2] = {1, n};
      contribute(sizeof(v), v, CkReduction::sum_int,
                 CkCallback(CkReductionTarget(Main, pingDone), mainProxy));
    }
  }

  void revive(int fromPe) { gotRevive(fromPe, "revive"); }
  void reviveHere(int fromPe) { gotRevive(fromPe, "reviveHere"); }

  void migrateNow(int seed) {
    if (CkNumPes() < 2) return;
    if (kindOf(thisIndex.x) == FRESH) return;  // must never live on its freshPe
    migrations++;
    migrateMe(migDest(thisIndex.x, thisIndex.y, seed, CkMyPe()));  // last action
  }

  void maybeDie(int seed) {
    // The proxy form (a message to itself, as in the manual and the other tests).
    // A direct ckDestroy() here crashes in broadcast delivery (null CkLocRec in
    // CkLocRec::invokeEntry, called from CkArrayBroadcaster::attemptDelivery),
    // on the pre-redesign runtime as well.
    if (kindOf(thisIndex.x) != FRESH && diesAt(thisIndex.x, thisIndex.y, seed))
      thisProxy[thisIndex].ckDestroy();
  }

  void dieIfFresh() {
    if (kindOf(thisIndex.x) == FRESH) thisProxy[thisIndex].ckDestroy();
  }

  void report() {
    int v[4] = {1, revives, demand, migrations};
    contribute(sizeof(v), v, CkReduction::sum_int,
               CkCallback(CkReductionTarget(Main, reportDone), mainProxy));
  }

  void dumpState() {
    CkPrintf("objid_insert DUMP: (%d,%d) id %llx on PE %d lastRound %d pending rounds "
             "%zu migrations %d demand %d revives %d\n", thisIndex.x, thisIndex.y,
             (unsigned long long)ckGetID().getElementID(), CkMyPe(), lastRound,
             pending.size(), migrations, demand, revives);
    for (auto& r : pending) {
      int n = 0;
      for (char c : r.second) n += c;
      CkPrintf("objid_insert DUMP:   (%d,%d) round %d has %d/%d pings\n", thisIndex.x,
               thisIndex.y, r.first, n, CkNumPes());
    }
  }
};

static void watchdogFire(void* arg, double t) { mainProxy.checkProgress(); }

#include "objid_insert.def.h"

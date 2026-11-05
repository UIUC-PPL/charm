// Broadcasts to array elements that are migrating with zero-copy pup.
//
// Each element owns a buffer that its pup routine moves with
// PUP::er::pup_buffer. When the element migrates to a PE in another process,
// the buffer is fetched with zero-copy reads, and ckJustMigrated runs only
// after those reads complete. Meanwhile the element is already in the new
// PE's element table. A broadcast must not run on the element in that
// interval: hit() aborts if ckJustMigrated has not run yet.
//
// Every round the main chare broadcasts migrate(), which sends each element
// to the PE half the machine away, and then, without waiting, a burst of
// hitsPerRound hit() broadcasts. Each hit() reports to the main chare; when
// all of a round's hits are in, the next round starts. At the end every
// element checks its whole buffer and reports how many hits it saw.
//
// Hits and the final check are counted with point-to-point messages to the
// main chare, not reductions: a reduction contributed right after migrateMe
// can be lost in one process (a separate defect), and this test is about
// broadcasts.
//
// Usage: ./bcast_zcpup [rounds] [hits per round] [buffer bytes]
// The interval exists only between processes (TESTRUN_PROCS=2); in one
// process the unpacked element takes the sender's pointer directly.

#include "bcast_zcpup.decl.h"
#include <cstring>

/*readonly*/ CProxy_Main mainProxy;
/*readonly*/ int numRounds;
/*readonly*/ int hitsPerRound;
/*readonly*/ int bufBytes;

static const int elemsPerPe = 2;

static inline char pattern(int index, int i) { return (char)((index * 31 + i) & 0x7f); }

class Main : public CBase_Main {
  CProxy_Elem arr;
  int numElems;
  int round = 0;
  int hitsThisRound = 0;
  long hitsCounted = 0;
  int checksIn = 0;
  long hitsSeen = 0;
  int badElems = 0;

public:
  Main(CkArgMsg *m) {
    numRounds = m->argc > 1 ? atoi(m->argv[1]) : 50;
    hitsPerRound = m->argc > 2 ? atoi(m->argv[2]) : 4;
    bufBytes = m->argc > 3 ? atoi(m->argv[3]) : (256 * 1024);
    delete m;
    mainProxy = thisProxy;
    numElems = elemsPerPe * CkNumPes();
    CkPrintf("bcast_zcpup: %d elements, %d rounds, %d hits per round, %d-byte buffers\n",
             numElems, numRounds, hitsPerRound, bufBytes);
    arr = CProxy_Elem::ckNew(numElems);
    startRound();
  }

  void startRound() {
    hitsThisRound = 0;
    arr.migrate(round);
    for (int h = 0; h < hitsPerRound; h++) arr.hit(round);
  }

  void hitDone(int r) {
    if (r != round)
      CkAbort("bcast_zcpup: a hit of round %d arrived during round %d", r, round);
    hitsCounted++;
    if (++hitsThisRound < hitsPerRound * numElems) return;
    if (++round < numRounds)
      startRound();
    else
      arr.check();
  }

  void checkDone(int hits, int bad) {
    hitsSeen += hits;
    badElems += bad;
    if (++checksIn < numElems) return;
    long expected = (long)numRounds * hitsPerRound * numElems;
    if (badElems != 0 || hitsSeen != expected || hitsCounted != expected)
      CkAbort("bcast_zcpup: %d elements with a damaged buffer; hits seen %ld, reported %ld, expected %ld",
              badElems, hitsSeen, hitsCounted, expected);
    CkPrintf("bcast_zcpup: %ld hits delivered, every buffer intact\n", expected);
    CkPrintf("All tests passed\n");
    CkExit();
  }
};

class Elem : public CBase_Elem {
  char *buf = nullptr;
  int restored;   // 1 once ckJustMigrated has run after the latest migration
  int hits = 0;
  int migrations = 0;

public:
  Elem() : restored(1) {
    buf = (char *)malloc(bufBytes);
    for (int i = 0; i < bufBytes; i++) buf[i] = pattern(thisIndex, i);
  }

  Elem(CkMigrateMessage *m) : CBase_Elem(m), restored(0) {}

  ~Elem() { free(buf); }

  void pup(PUP::er &p) {
    p | hits;
    p | migrations;
    p.pup_buffer(buf, bufBytes);
    // After packing, the runtime frees the buffer once the receiver has it
    // (in one process the receiver takes this very pointer), so the
    // departing element must not free it in its destructor.
    if (p.isPacking()) buf = nullptr;
  }

  // The base ckJustMigrated delivers the broadcasts that arrived while the
  // element was away, so restored is set first.
  void ckJustMigrated() {
    restored = 1;
    CBase_Elem::ckJustMigrated();
  }

  void migrate(int round) {
    if (CkNumPes() == 1) return;
    migrations++;
    migrateMe((CkMyPe() + CkNumPes() / 2) % CkNumPes());
  }

  // Checks every 4096th byte; check() reads the whole buffer at the end.
  void hit(int round) {
    if (!restored)
      CkAbort("bcast_zcpup: element %d on PE %d got hit(%d) before ckJustMigrated ran",
              thisIndex, CkMyPe(), round);
    for (int i = 0; i < bufBytes; i += 4096)
      if (buf[i] != pattern(thisIndex, i))
        CkAbort("bcast_zcpup: element %d byte %d wrong in hit(%d)", thisIndex, i, round);
    hits++;
    mainProxy.hitDone(round);
  }

  void check() {
    int bad = 0;
    for (int i = 0; i < bufBytes; i++)
      if (buf[i] != pattern(thisIndex, i)) { bad = 1; break; }
    mainProxy.checkDone(hits, bad);
  }
};

#include "bcast_zcpup.def.h"

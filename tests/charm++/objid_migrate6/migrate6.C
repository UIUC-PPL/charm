#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "migrate6.decl.h"

// A chare array with a 6-component index that migrates. Without -b the array has
// no bounds, so its index is not packed into element ids (the hashed kind: ids come
// from the process's tranche and every PE learns index -> id -> location through
// the location tables); with -b the bounds 4^6 pack into 12 bits (the packed kind).
// Elements are inserted dynamically, from every PE. Three modes:
//
//   default   ring traffic as in unbounded_migration, and every migPeriod steps an
//             element calls migrateMe to the next PE (crosses processes under lcrun)
//   -lb       the same traffic, but the element calls AtSync instead of migrateMe;
//             run with +balancer (user code must not call migrateMe under a balancer)
//   -stream   one element and a sender on another PE of the same process: the sender
//             streams nCount messages while the element migrates between the PEs of
//             its process; checks that every message is delivered exactly once
//
// Every mode ends by quiescence and then counts what the elements received; a
// count that is off aborts. Run with +objid_diag to see the location table's
// counters per process.
// doc/internals/process-location-cache.md, section 7.

/*readonly*/ CProxy_Main mainProxy;
/*readonly*/ CProxy_Elem arrProxy;
/*readonly*/ int nElems;
/*readonly*/ int nCount;
/*readonly*/ int migPeriod;
/*readonly*/ int mode;  // 0 migrateMe, 1 AtSync, 2 stream

enum { MODE_MIGRATEME = 0, MODE_ATSYNC = 1, MODE_STREAM = 2 };

static CkArrayIndex6D indexOf(int i)
{
  return CkArrayIndex6D(i % 4, (i / 4) % 4, (i / 16) % 4, i % 3, i % 2, 1);
}

class Main : public CBase_Main
{
  double t0;

public:
  Main(CkArgMsg* m)
  {
    nElems = 24;
    nCount = 40;
    migPeriod = 5;
    mode = MODE_MIGRATEME;
    bool bounded = false;
    for (int i = 1; i < m->argc; ++i)
    {
      if (!strcmp(m->argv[i], "-b")) bounded = true;
      else if (!strcmp(m->argv[i], "-lb")) mode = MODE_ATSYNC;
      else if (!strcmp(m->argv[i], "-stream")) mode = MODE_STREAM;
      else if (!strcmp(m->argv[i], "-n") && i + 1 < m->argc) nElems = atoi(m->argv[++i]);
      else if (!strcmp(m->argv[i], "-c") && i + 1 < m->argc) nCount = atoi(m->argv[++i]);
      else if (!strcmp(m->argv[i], "-p") && i + 1 < m->argc) migPeriod = atoi(m->argv[++i]);
    }
    delete m;
    if (mode == MODE_STREAM) nElems = 1;
    if (nElems > 64) nElems = 64;  // indexOf is injective below 64

    CkPrintf("objid_migrate6: %d PEs in %d process(es), %d elements, count %d, period %d,"
             " %s index, mode %s\n",
             CkNumPes(), CkNumNodes(), nElems, nCount, migPeriod,
             bounded ? "packed (bounded)" : "hashed (unbounded)",
             mode == MODE_MIGRATEME ? "migrateMe" : mode == MODE_ATSYNC ? "AtSync" : "stream");
    mainProxy = thisProxy;

    CkArrayOptions opts;  // no initial elements
    if (bounded) opts.setBounds(4, 4, 4, 4, 4, 4);
    arrProxy = CProxy_Elem::ckNew(opts);
    t0 = CkWallTimer();
    CProxy_Inserter::ckNew();
  }

  void inserted()
  {
    arrProxy.doneInserting();
    CkStartQD(CkCallback(CkIndex_Main::quiescent(), thisProxy));
    if (mode == MODE_STREAM)
    {
      // The sender is the next rank of the element's process (or PE 0 itself when
      // the process has one PE: then the stream is local sends only).
      const int me = 0;
      const int sender = CkNodeSize(CkNodeOf(me)) > 1 ? me + 1 : me;
      CProxy_Streamer streamers = CProxy_Streamer::ckNew();
      streamers[sender].stream(nCount);
    }
    else
    {
      arrProxy.SayHi(nCount);
    }
  }

  void quiescent()
  {
    CkPrintf("quiescent after %.3f s; counting\n", CkWallTimer() - t0);
    arrProxy.report();
  }

  void tally(CkReductionMsg* msg)
  {
    const long* got = (const long*)msg->getData();
    // got[0] SayHi, got[1] ackPlease, got[2] ticks, got[3] migrations
    long expectHi = 0, expectAck = 0, expectTick = 0;
    if (mode == MODE_STREAM)
      expectTick = nCount;
    else
    {
      expectHi = (long)nElems * (nCount + 1);  // SayHi(count) ... SayHi(0)
      expectAck = (long)nElems * nCount;
    }
    CkPrintf("received: SayHi %ld (expect %ld), ackPlease %ld (expect %ld), ticks %ld"
             " (expect %ld); migrations %ld\n",
             got[0], expectHi, got[1], expectAck, got[2], expectTick, got[3]);
    if (got[0] != expectHi || got[1] != expectAck || got[2] != expectTick)
      CkAbort("objid_migrate6: delivery count mismatch\n");
    // Stream mode moves only between the PEs of one process; nothing can move when
    // each process has one PE. A balancer is free to move nothing.
    const bool canMove = mode == MODE_STREAM ? CkNodeSize(0) > 1 : CkNumPes() > 1;
    if (got[3] == 0 && canMove && mode != MODE_ATSYNC)
      CkAbort("objid_migrate6: nothing migrated\n");
    CkPrintf("All done\n");
    delete msg;
    CkExit();
  }
};

// Every PE inserts its share of the elements, so hashed-kind ids are minted on
// every process and the index bindings reach the homes from everywhere.
class Inserter : public CBase_Inserter
{
public:
  Inserter()
  {
    for (int i = 0; i < nElems; ++i)
      if (i % CkNumPes() == CkMyPe()) arrProxy(indexOf(i)).insert(i);
    contribute(CkCallback(CkReductionTarget(Main, inserted), mainProxy));
  }
};

class Streamer : public CBase_Streamer
{
public:
  Streamer() {}
  void stream(int n)
  {
    CkArrayIndex6D target = indexOf(0);
    for (int i = 0; i < n; ++i) arrProxy(target).tick();
  }
};

class Elem : public CBase_Elem
{
  int ordinal;
  long hi, ack, ticks, moves;
  int pendingCount;  // AtSync mode: the step to continue in ResumeFromSync

  int nextPeInProcess() const
  {
    const int node = CkMyNode();
    const int first = CkNodeFirst(node), size = CkNodeSize(node);
    return first + (CkMyPe() - first + 1) % size;
  }

public:
  Elem(int ord) : ordinal(ord), hi(0), ack(0), ticks(0), moves(0), pendingCount(-1)
  {
    usesAtSync = (mode == MODE_ATSYNC);
  }
  Elem(CkMigrateMessage* m) : CBase_Elem(m) {}

  void pup(PUP::er& p)
  {
    CBase_Elem::pup(p);
    p | ordinal;
    p | hi;
    p | ack;
    p | ticks;
    p | moves;
    p | pendingCount;
  }

  void ckJustMigrated()
  {
    CBase_Elem::ckJustMigrated();
    ++moves;
  }

  void SayHi(int count)
  {
    ++hi;
    if (count > 0)
      arrProxy(indexOf((ordinal + 1) % nElems)).ackPlease(ordinal, count - 1);
    if (count % migPeriod == 1 && CkNumPes() > 1)
    {
      if (mode == MODE_ATSYNC)
        AtSync();  // last action; the balancer may move us before the ack returns
      else if (mode == MODE_STREAM)
        migrateMe(nextPeInProcess());
      else
        migrateMe((CkMyPe() + 1) % CkNumPes());
    }
  }

  void ResumeFromSync() {}

  void ackPlease(int fromOrdinal, int c)
  {
    ++ack;
    arrProxy(indexOf(fromOrdinal)).SayHi(c);
  }

  void tick()
  {
    ++ticks;
    // Move between the PEs of this process while the stream is in flight.
    if (ticks % migPeriod == 0 && CkNodeSize(CkMyNode()) > 1) migrateMe(nextPeInProcess());
  }

  void report()
  {
    long v[4] = {hi, ack, ticks, moves};
    contribute(sizeof(v), v, CkReduction::sum_long,
               CkCallback(CkIndex_Main::tally(NULL), mainProxy));
  }
};

#include "migrate6.def.h"

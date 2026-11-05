// iosessions: more CkIO sessions than the standard id layout has collection
// numbers (charm #4149), with two standard arrays alive and busy the whole time.
//
// CkIO creates one chare array per write session (WriteSession) and one per read
// session (BufferChares) and destroys it when the session ends; collection numbers
// are not reused, so the standard layout (12 collection bits, 4095 arrays) ran out
// after 4095 sessions. The session arrays use the wide layout now
// (doc/internals/object-ids.md, section 7). This test runs S sessions (default
// 4200), alternating a write session and a read session over the same bytes:
//
//   App   standard layout, packed kind (bounded, K elements): in each write
//         session element i writes its 16-byte record for that iteration at
//         offset 16*i; in each read session it reads the 16 bytes back and
//         checks they are the record of the previous iteration.
//   Hash  standard layout, hashed kind (no bounds, K elements inserted): at
//         each write session element i sends one message to element i+1, so
//         standard ids are addressed while wide arrays come and go. The total
//         is checked at the end.
//
// At the end the file is read with stdio and must hold the K records of the
// last write session. The memory in use by PE 0's process (CmiMemoryUsage) is
// sampled at session 1000 and at the end: a session must leave less than M KB
// behind (-m M, default
// 4), since each one creates and destroys an array. About 1 KB per session stays
// by design: broadcast copies kept until the periodic spring cleaning, and the
// per-id slots of the group table, the load balancer and the array map, which
// are not reused. Options: -s S sessions
// (default 4200; must exceed 4095 to cover the defect), -k K elements per array
// (default 8).
#include <stdio.h>
#include <string.h>
#include <string>
#include "ckio.h"
#include "iosessions.decl.h"

/* readonly */ CProxy_Main mainProxy;
/* readonly */ int K;

static const int REC = 16;  // bytes per record
static const char* FILENAME = "sessions.dat";

static std::string record(int iter, int idx) {
  char buf[REC + 1];
  snprintf(buf, sizeof(buf), "%7d:%7d\n", iter, idx);
  return std::string(buf, REC);
}

static void watchdogFire(void* arg, double t);

class Main : public CBase_Main {
  int S;
  int iter = 0;   // even: write session, odd: read session
  int writes = 0;
  Ck::IO::File file;
  Ck::IO::Session readSession;
  CProxy_App app;
  CProxy_Hash hash;
  double lastAdvance;
  int wdSecs = 60;
  double maxKB = 4.0;             // -m: allowed growth per session
  static const int MEM_ITER = 1000;
  CmiUInt8 memAtStart = 0;

public:
  Main(CkArgMsg* m) {
    setvbuf(stdout, NULL, _IONBF, 0);
    S = 4200;
    K = 8;
    CmiGetArgInt(m->argv, "-s", &S);
    CmiGetArgInt(m->argv, "-k", &K);
    CmiGetArgInt(m->argv, "-w", &wdSecs);
    CmiGetArgDouble(m->argv, "-m", &maxKB);
    delete m;
    CkEnforce(S >= 2 && K >= 1);
    mainProxy = thisProxy;
    CkPrintf("iosessions: %d sessions, %d PEs, %d processes, %d elements per array\n", S,
             CkNumPes(), CkNumNodes(), K);
    remove(FILENAME);

    app = CProxy_App::ckNew(K);  // bounded: standard layout, packed kind
    CkArrayOptions hopts;        // no bounds: standard layout, hashed kind
    hash = CProxy_Hash::ckNew(hopts);
    for (int i = 0; i < K; i++) hash[i].insert();
    hash.doneInserting();

    Ck::IO::Options opts;
    Ck::IO::open(FILENAME, CkCallback(CkIndex_Main::opened(NULL), thisProxy), opts);
    lastAdvance = CkWallTimer();
    if (wdSecs > 0) CcdCallFnAfter(watchdogFire, NULL, 2000);
  }

  void progress() { lastAdvance = CkWallTimer(); }

  void opened(Ck::IO::FileReadyMsg* m) {
    file = m->file;
    delete m;
    next();
  }

  void next() {
    progress();
    if (iter == MEM_ITER) memAtStart = CmiMemoryUsage();
    if (iter == S) {
      hash.report(K * writes);
      return;
    }
    if (iter % 500 == 0) CkPrintf("iosessions: session %d\n", iter);
    if (iter % 2 == 0)
      Ck::IO::startSession(file, REC * K, 0,
                           CkCallback(CkIndex_Main::writeReady(NULL), thisProxy),
                           CkCallback(CkIndex_Main::written(NULL), thisProxy));
    else
      Ck::IO::startReadSession(file, REC * K, 0,
                               CkCallback(CkIndex_Main::readReady(NULL), thisProxy));
  }

  void writeReady(Ck::IO::SessionReadyMsg* m) {
    progress();
    app.writeRecord(m->session, iter);
    hash.ping(iter);
    delete m;
  }

  void written(CkReductionMsg* m) {
    delete m;
    writes++;
    iter++;
    next();
  }

  void readReady(Ck::IO::SessionReadyMsg* m) {
    progress();
    readSession = m->session;
    delete m;
    app.readRecord(readSession, iter);
  }

  void readsDone() {
    progress();
    Ck::IO::closeReadSession(readSession,
                             CkCallback(CkIndex_Main::readClosed(NULL), thisProxy));
  }

  void readClosed(CkReductionMsg* m) {
    delete m;
    iter++;
    next();
  }

  void pingsDone(int n) {
    progress();
    if (n != K * writes) {
      CkPrintf("iosessions FAIL: Hash elements received %d messages, expected %d\n", n,
               K * writes);
      CkAbort("message count");
    }
    Ck::IO::close(file, CkCallback(CkIndex_Main::closed(NULL), thisProxy));
  }

  void closed(CkReductionMsg* m) {
    delete m;
    if (S > MEM_ITER && maxKB > 0) {
      double perSession = (double)((CmiInt8)CmiMemoryUsage() - (CmiInt8)memAtStart) / 1024.0 /
                          (S - MEM_ITER);
      CkPrintf("iosessions: memory in use grew %.2f KB per session between session %d and "
               "%d (limit %.1f)\n", perSession, MEM_ITER, S, maxKB);
      if (perSession > maxKB) {
        CkPrintf("iosessions FAIL: memory left behind by sessions\n");
        CkAbort("memory growth");
      }
    }
    // The file holds the records of the last write session.
    const int last = (S - 1) / 2 * 2;
    FILE* fp = fopen(FILENAME, "rb");
    if (!fp) CkAbort("iosessions: cannot open %s", FILENAME);
    for (int i = 0; i < K; i++) {
      char buf[REC + 1] = {0};
      if (fread(buf, 1, REC, fp) != (size_t)REC) {
        CkPrintf("iosessions FAIL: %s is shorter than %d bytes\n", FILENAME, REC * K);
        CkAbort("file length");
      }
      if (std::string(buf, REC) != record(last, i)) {
        CkPrintf("iosessions FAIL: record %d is \"%.*s\", expected \"%.*s\"\n", i, REC, buf,
                 REC, record(last, i).c_str());
        CkAbort("file contents");
      }
    }
    fclose(fp);
    CkPrintf("iosessions PASS: %d sessions (%d write, %d read), %d PEs, %d processes, "
             "file checked\n", S, writes, S - writes, CkNumPes(), CkNumNodes());
    CkExit();
  }

  void checkProgress() {
    if (wdSecs > 0 && CkWallTimer() - lastAdvance > (double)wdSecs) {
      CkPrintf("iosessions WATCHDOG: no progress for %d s at session %d\n", wdSecs, iter);
      CkAbort("iosessions: hung");
    }
    CcdCallFnAfter(watchdogFire, NULL, 2000);
  }
};

static void watchdogFire(void* arg, double t) { mainProxy.checkProgress(); }

class App : public CBase_App {
  char buf[REC];
  int expectIter = -1;

public:
  App() {}
  App(CkMigrateMessage* m) {}

  void writeRecord(Ck::IO::Session s, int iter) {
    std::string r = record(iter, thisIndex);
    Ck::IO::write(s, r.data(), REC, REC * thisIndex);
  }

  void readRecord(Ck::IO::Session s, int iter) {
    expectIter = iter - 1;  // the write session before this read session
    Ck::IO::read(s, REC, REC * thisIndex, buf,
                 CkCallback(CkIndex_App::readDone(NULL), thisProxy[thisIndex]));
  }

  void readDone(Ck::IO::ReadCompleteMsg* m) {
    if (m->bytes != (size_t)REC || m->offset != (size_t)(REC * thisIndex) ||
        std::string(buf, REC) != record(expectIter, thisIndex)) {
      CkPrintf("iosessions FAIL: element %d read %zu bytes at %zu: \"%.*s\", expected "
               "\"%.*s\"\n", thisIndex, m->bytes, m->offset, REC, buf, REC,
               record(expectIter, thisIndex).c_str());
      CkAbort("read contents");
    }
    delete m;
    contribute(CkCallback(CkReductionTarget(Main, readsDone), mainProxy));
  }
};

class Hash : public CBase_Hash {
  int received = 0;
  int expected = -1;  // set by report(); the element contributes when it has them all

  void maybeReport() {
    if (expected < 0 || received < expected) return;
    contribute(sizeof(int), &received, CkReduction::sum_int,
               CkCallback(CkReductionTarget(Main, pingsDone), mainProxy));
    expected = -1;
  }

public:
  Hash() {}
  Hash(CkMigrateMessage* m) {}
  void ping(int iter) { thisProxy[(thisIndex + 1) % K].pass(iter); }
  void pass(int iter) {
    received++;
    maybeReport();
  }
  // total: every element's share, so the last pass messages are waited for
  void report(int total) {
    expected = total / K;
    maybeReport();
  }
};

#include "iosessions.def.h"

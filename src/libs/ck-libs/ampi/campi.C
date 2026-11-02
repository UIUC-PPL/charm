/*
 * campi (coroutine AMPI): AMPI ranks as C++20 coroutines instead of threads.
 * Compiled with -std=c++20 into libmoduleampi. See campi_core.h.
 *
 * Structure:
 *  - Rank record and context switching (campi_start, campi_resume): every
 *    rank on a PE shares the scheduler thread's Ctv slots, so the rank's
 *    AMPI context (_curTCharm, ampiPtr, ampiInitDone, ampiFinalized) is
 *    installed at each resume and saved back at each suspension.
 *  - Wake-ups: TCharm::start()/resume() call campi_wake (via a tcharm hook),
 *    which enqueues a small Converse message whose handler resumes the rank,
 *    as CthAwaken enqueues a thread. Resuming inline instead is unsafe: AMPI
 *    wakes ranks from inside constructors (registerAmpi's semaPut runs in the
 *    ampi element constructor before myComm is set) and from the middle of
 *    delivery code, which a thread-based rank never observes.
 *  - Awaitables: blocking MPI calls as "post the nonblocking variant, then
 *    suspend until the request completes", mirroring what the request's
 *    wait() does before it blocks a thread. The simple calls return a
 *    request_awaiter (no coroutine frame); a rank blocked in one is polled
 *    by campi_resume and resumed only when its requests are complete.
 *  - Init/Finalize and communicator creation.
 */
#include "ampiimpl.h"
#include "tcharm_impl.h"

#if !(__cplusplus >= 202002L && defined(__cpp_impl_coroutine))
// The compiler cannot build coroutines (e.g. GCC 10 without -fcoroutines):
// libampi is built without campi, and no program can select campi mode.
void campi_parent_attached(TCharm *, ampiParent *) noexcept {}
void campi_proc_init() noexcept {}
#else

#include "campi_core.h"

#include <cstdlib>
#include <cstring>

CtvExtern(ampiParent*, ampiPtr);
CtvExtern(bool, ampiInitDone);
CtvExtern(bool, ampiFinalized);
CtvExtern(void*, stackBottom);
CkpvExtern(int, ampiThreadLevel);

// Set CAMPI_DEBUG=1 in the environment for a trace of rank scheduling.
static int campiDebug = -1;
#define CAMPI_DBG(...)                                                     \
  do {                                                                     \
    if (campiDebug > 0) { CmiPrintf("[campi pe %d] ", CkMyPe());          \
                          CmiPrintf(__VA_ARGS__); fflush(stdout); }        \
  } while (0)

namespace campi {

/* ====================== rank record ====================== */

struct Rank {
  TCharm *tc = nullptr;
  ampiParent *parent = nullptr;
  std::coroutine_handle<> h;                          // frame to resume next
  request_awaiter *waiter = nullptr; // h is blocked in this awaiter: poll it first
  std::coroutine_handle<detail::promise<int>> root;   // the rank's main
  bool running = false;        // inside campi_resume for this rank
  bool resumePending = false;  // woken while running
  bool wakeQueued = false;     // a wake message is in the scheduler queue
  bool initDone = false;       // saved Ctv ampiInitDone
  bool finalized = false;      // saved Ctv ampiFinalized
  bool finished = false;       // main returned and AMPI_Exit was called
  int exitCode = 0;
  char **argv = nullptr;
  // Init on element 0: completion of an array creation (CkCallback target)
  bool arrayCreated = false;
  CkArrayID createdAid;
};

main_fn registered_main = nullptr;

static void campi_start(TCharm *tc);
static void campi_wake(void *rank);

void register_main(main_fn f) noexcept {
  registered_main = f;
  // Selects campi mode in TCharm::procInit (tcharm cannot refer to campi).
  tcharm_campi_start_fn = campi_start;
  tcharm_campi_resume_fn = campi_wake;
}

[[noreturn]] void abort_unhandled_exception() noexcept {
  CkAbort("campi: unhandled C++ exception in an AMPI rank coroutine");
  std::abort(); // CkAbort is not declared noreturn
}

static inline Rank *current() noexcept {
  TCharm *tc = TCharm::getNULL();
  return tc ? (Rank *)tc->getCampiRank() : nullptr;
}

void set_resume_point(std::coroutine_handle<> h) noexcept {
  Rank *r = current();
  if (r == nullptr)
    CkAbort("campi: co_await on a campi awaitable outside a campi rank");
  r->h = h;
  r->waiter = nullptr;
}

void set_resume_point(std::coroutine_handle<> h, request_awaiter *w) noexcept {
  Rank *r = current();
  if (r == nullptr)
    CkAbort("campi: co_await on a campi awaitable outside a campi rank");
  r->h = h;
  r->waiter = w;
}

/* ====================== resume / wake ====================== */

static void campi_resume(Rank *r);

struct WakeMsg {
  char header[CmiMsgHeaderSizeBytes];
  Rank *rank;
};

// Per PE (the handler table is per PE; wake messages never leave the PE).
// Registered by campi_proc_init on every PE at startup.
static thread_local int wakeHandlerIdx = -1;

static void wakeHandler(void *msg) {
  Rank *r = ((WakeMsg *)msg)->rank;
  CmiFree(msg);
  r->wakeQueued = false;
  campi_resume(r);
}

static void enqueue_wake(Rank *r) {
  if (r->wakeQueued || r->finished) return;
  r->wakeQueued = true;
  WakeMsg *m = (WakeMsg *)CmiAlloc(sizeof(WakeMsg));
  m->rank = r;
  CmiSetHandler(m, wakeHandlerIdx);
  CsdEnqueue(m);
}

// tcharm hook: TCharm::start() (and so resume(), semaPut, setInitDoneFlag,
// AMPI's delivery code) lands here instead of CthAwaken.
static void campi_wake(void *rp) {
  Rank *r = (Rank *)rp;
  CAMPI_DBG("wake rank %d (running %d, queued %d)\n", r ? r->tc->getElement() : -1,
            r ? (int)r->running : -1, r ? (int)r->wakeQueued : -1);
  if (r == nullptr || r->finished) return;
  enqueue_wake(r);
}

static void finish_rank(Rank *r) {
  // The rank's main returned: exit on its behalf, with its context installed.
  r->exitCode = r->root.promise().value;
  r->root.destroy();
  r->root = {};
  r->h = {};
  r->waiter = nullptr;
  r->finished = true;
  AMPI_Exit(r->exitCode); // contributes to TCharm's exit reduction; returns in campi mode
}

static void campi_resume(Rank *r) {
  if (r->finished) return;
  if (r->running) { // re-entrancy guard (cannot happen with queued wake-ups)
    r->resumePending = true;
    return;
  }
  do {
    r->resumePending = false;
    if (!r->h) CkAbort("campi: rank woken with no resume point");

    // Save the current context and install the rank's.
    TCharm *oldTc = CtvAccess(_curTCharm);
    ampiParent *oldParent = CtvAccess(ampiPtr);
    bool oldInitDone = CtvAccess(ampiInitDone);
    bool oldFinalized = CtvAccess(ampiFinalized);
    CtvAccess(_curTCharm) = r->tc;
    CtvAccess(ampiPtr) = r->parent;
    CtvAccess(ampiInitDone) = r->initDone;
    CtvAccess(ampiFinalized) = r->finalized;
    r->running = true;
    r->tc->setStopped(false);
#if CMK_LBDB_ON
    // as TCharm::stop_static does around a thread's run
    r->tc->getCkLocRec()->startTiming();
#endif

    // A rank blocked in a request_awaiter stays suspended (r->h kept) until
    // its requests complete; poll() redoes the blocked bookkeeping.
    if (r->waiter == nullptr || r->waiter->poll()) {
      r->waiter = nullptr;
      std::coroutine_handle<> h = std::exchange(r->h, {});
      CAMPI_DBG("resume rank %d\n", r->tc->getElement());
      h.resume(); // runs until the rank suspends at an awaitable or main returns
      CAMPI_DBG("rank %d suspended (done %d)\n", r->tc->getElement(), (int)r->root.done());
    }

    r->initDone = CtvAccess(ampiInitDone);
    r->finalized = CtvAccess(ampiFinalized);
    if (CtvAccess(ampiPtr) != nullptr) r->parent = CtvAccess(ampiPtr);
    if (r->root.done())
      finish_rank(r);
    else if (!r->h)
      CkAbort("campi: rank suspended at an awaitable that is not campi's");

#if CMK_LBDB_ON
    r->tc->getCkLocRec()->stopTiming();
#endif
    r->tc->setStopped(true);
    r->running = false;
    CtvAccess(_curTCharm) = oldTc;
    CtvAccess(ampiPtr) = oldParent;
    CtvAccess(ampiInitDone) = oldInitDone;
    CtvAccess(ampiFinalized) = oldFinalized;
  } while (r->resumePending && !r->finished);
}

// tcharm hook, called from TCharm::run() (the element constructor) in place
// of creating and awakening a thread: what AMPI_threadstart does, as a coroutine.
static void campi_start(TCharm *tc) {
  if (campiDebug < 0) {
    const char *dbg = getenv("CAMPI_DEBUG");
    campiDebug = (dbg != nullptr && atoi(dbg) != 0) ? 1 : 0;
  }
  CkAssert(wakeHandlerIdx >= 0); // registered by campi_proc_init
  Rank *r = new Rank;
  r->tc = tc;
  tc->setCampiRank(r);

  r->argv = CmiCopyArgs(CkGetArgv());
  int argc = CkGetArgc();
  // A pointer near the "bottom of the stack", for ampiCurrentStackUsage:
  // meaningless without a thread, but keep it non-null.
  CtvAccess(stackBottom) = &r->argv;

  // initial_suspend is suspend_always: no user code runs here.
  r->root = registered_main(r->argv ? argc : 0, r->argv).release();
  r->h = r->root;
  campi_wake(r);
}

} // namespace campi

// ampi.C hook: ampiProcInit (an initproc, run on every PE before any rank
// starts). Registering here rather than when the first rank starts on a PE
// keeps handler indices the same on every PE, including PEs with no rank.
void campi_proc_init() noexcept {
  if (campi::registered_main == nullptr) return; // not a campi program
  campi::wakeHandlerIdx = CmiRegisterHandler((CmiHandler)campi::wakeHandler);
}

// ampi.C hook: ampiParent::prepareCtv in campi mode.
void campi_parent_attached(TCharm *tc, ampiParent *parent) noexcept {
  campi::Rank *r = (campi::Rank *)tc->getCampiRank();
  if (r == nullptr) return;
  r->parent = parent;
  if (campi::current() == r) CtvAccess(ampiPtr) = parent;
}

namespace campi {

task<void> yield() {
  Rank *r = current();
  enqueue_wake(r);
  co_await suspend();
}

/* ====================== request completion ====================== */

static inline AmpiRequest *lookup(ampiParent *p, MPI_Request idx) {
  return p->getReqs()[idx];
}

// Completion check without MPI_Test's side effects (status, free).
static inline bool req_done(ampiParent *p, MPI_Request idx) {
  if (idx == MPI_REQUEST_NULL) return true;
  return lookup(p, idx)->test(MPI_STATUS_IGNORE);
}

/* ---- request_awaiter: the frame-free awaitable of the simple calls ---- */

bool request_awaiter::done() noexcept {
  ampiParent *p = getAmpiParent();
  MPI_Request *r = reqs();
  for (int i = 0; i < n_; i++)
    if (!req_done(p, r[i])) return false;
  return true;
}

// As the request's wait() (WAIT_ONE, through AmpiRequest::beginBlocking) or
// ampiParent::waitall (WAIT_ALL, SENDRECV) does before it suspends a thread.
// A request that completes by polling (beginBlocking returns 0) is polled
// again through a queued wake-up.
void request_awaiter::block() noexcept {
  ampiParent *p = getAmpiParent();
  MPI_Request *r = reqs();
  CkAssert(p->numBlockedReqs == 0);
  if (kind_ == WAIT_ONE) {
    bool viaColl = false;
    int n = lookup(p, r[0])->beginBlocking(p, viaColl);
    p->numBlockedReqs = n;
    CAMPI_DBG("rank %d waits on request %d (type %d)\n", TCHARM_Element(), r[0],
              (int)lookup(p, r[0])->getType());
    if (n == 0) enqueue_wake(current());
    // IReq/SendReq/SsendReq/ScanReq::wait block via blockOnRecv,
    // RednReq/GatherReq/GathervReq::wait via blockOnColl.
    if (viaColl) p->resumeOnColl = true;
    else p->resumeOnRecv = true;
  } else {
    int blocked = 0;
    bool needPoll = false;
    for (int i = 0; i < n_; i++) {
      if (req_done(p, r[i])) continue;
      bool viaColl = false;
      int n = lookup(p, r[i])->beginBlocking(p, viaColl);
      if (n == 0) needPoll = true;
      blocked += n;
    }
    p->numBlockedReqs = blocked;
    CAMPI_DBG("rank %d waits on %d of %d requests\n", TCHARM_Element(), blocked, n_);
    if (needPoll) enqueue_wake(current());
    p->resumeOnRecv = true;
    p->resumeOnColl = true; // a collective request's completion (irednResult) checks this flag
  }
}

void request_awaiter::unblock() noexcept {
  ampiParent *p = getAmpiParent();
  MPI_Request *r = reqs();
  for (int i = 0; i < n_; i++)
    if (r[i] != MPI_REQUEST_NULL) lookup(p, r[i])->endBlocking(p);
  p->resumeOnRecv = false;
  p->resumeOnColl = false;
  p->numBlockedReqs = 0;
}

bool request_awaiter::await_ready() noexcept {
  return err_ != MPI_SUCCESS || done();
}

void request_awaiter::await_suspend(std::coroutine_handle<> h) noexcept {
  block();
  set_resume_point(h, this);
}

bool request_awaiter::poll() noexcept {
  unblock();
  if (done()) return true;
  block(); // a spurious wake-up: stay suspended
  return false;
}

int request_awaiter::await_resume() noexcept {
  if (err_ != MPI_SUCCESS) return err_;
  switch (kind_) { // the requests are complete: these do not block
    case WAIT_ONE: return MPI_Wait(reqs(), sts_);
    case WAIT_ALL: return MPI_Waitall(n_, reqs(), sts_);
    case SENDRECV: default: {
      MPI_Status st[2];
      int e = MPI_Waitall(2, own_, st);
      if (sts_ != MPI_STATUS_IGNORE) *sts_ = st[0];
      return e;
    }
  }
}

request_awaiter Wait(MPI_Request *req, MPI_Status *sts) {
  return request_awaiter(request_awaiter::WAIT_ONE, 1, req, sts);
}

request_awaiter Waitall(int n, MPI_Request *reqs, MPI_Status *sts) {
  return request_awaiter(request_awaiter::WAIT_ALL, n, reqs, sts);
}

/* ====================== point-to-point ====================== */

// Post one request with call (which names it req), then wait on it.
#define CAMPI_POST_AND_WAIT(call, sts)                                 \
  request_awaiter w(request_awaiter::WAIT_ONE, 1, nullptr, (sts));     \
  MPI_Request *req = w.own();                                          \
  int e = (call);                                                      \
  if (e != MPI_SUCCESS) return request_awaiter::failed(e);             \
  return w;

request_awaiter Send(const void *buf, int count, MPI_Datatype type, int dest, int tag,
                     MPI_Comm comm) {
  CAMPI_POST_AND_WAIT(MPI_Isend(buf, count, type, dest, tag, comm, req), MPI_STATUS_IGNORE)
}

request_awaiter Ssend(const void *buf, int count, MPI_Datatype type, int dest, int tag,
                      MPI_Comm comm) {
  CAMPI_POST_AND_WAIT(MPI_Issend(buf, count, type, dest, tag, comm, req), MPI_STATUS_IGNORE)
}

request_awaiter Recv(void *buf, int count, MPI_Datatype type, int src, int tag, MPI_Comm comm,
                     MPI_Status *sts) {
  CAMPI_POST_AND_WAIT(MPI_Irecv(buf, count, type, src, tag, comm, req), sts)
}

request_awaiter Sendrecv(const void *sbuf, int scount, MPI_Datatype stype, int dest, int stag,
                         void *rbuf, int rcount, MPI_Datatype rtype, int src, int rtag,
                         MPI_Comm comm, MPI_Status *sts) {
  request_awaiter w(request_awaiter::SENDRECV, 2, nullptr, sts);
  MPI_Request *reqs = w.own();
  int e = MPI_Irecv(rbuf, rcount, rtype, src, rtag, comm, &reqs[0]);
  if (e != MPI_SUCCESS) return request_awaiter::failed(e);
  e = MPI_Isend(sbuf, scount, stype, dest, stag, comm, &reqs[1]);
  if (e != MPI_SUCCESS) return request_awaiter::failed(e);
  return w;
}

/* ====================== collectives ====================== */

request_awaiter Barrier(MPI_Comm comm) {
  CAMPI_POST_AND_WAIT(MPI_Ibarrier(comm, req), MPI_STATUS_IGNORE)
}
request_awaiter Bcast(void *buf, int count, MPI_Datatype type, int root, MPI_Comm comm) {
  CAMPI_POST_AND_WAIT(MPI_Ibcast(buf, count, type, root, comm, req), MPI_STATUS_IGNORE)
}
request_awaiter Reduce(const void *sbuf, void *rbuf, int count, MPI_Datatype type, MPI_Op op,
                       int root, MPI_Comm comm) {
  CAMPI_POST_AND_WAIT(MPI_Ireduce(sbuf, rbuf, count, type, op, root, comm, req),
                      MPI_STATUS_IGNORE)
}
request_awaiter Allreduce(const void *sbuf, void *rbuf, int count, MPI_Datatype type, MPI_Op op,
                          MPI_Comm comm) {
  CAMPI_POST_AND_WAIT(MPI_Iallreduce(sbuf, rbuf, count, type, op, comm, req),
                      MPI_STATUS_IGNORE)
}
request_awaiter Gather(const void *sbuf, int scount, MPI_Datatype stype, void *rbuf, int rcount,
                       MPI_Datatype rtype, int root, MPI_Comm comm) {
  CAMPI_POST_AND_WAIT(MPI_Igather(sbuf, scount, stype, rbuf, rcount, rtype, root, comm, req),
                      MPI_STATUS_IGNORE)
}
request_awaiter Gatherv(const void *sbuf, int scount, MPI_Datatype stype, void *rbuf,
                        const int *rcounts, const int *displs, MPI_Datatype rtype, int root,
                        MPI_Comm comm) {
  CAMPI_POST_AND_WAIT(MPI_Igatherv(sbuf, scount, stype, rbuf, rcounts, displs, rtype, root,
                                   comm, req),
                      MPI_STATUS_IGNORE)
}
request_awaiter Allgather(const void *sbuf, int scount, MPI_Datatype stype, void *rbuf,
                          int rcount, MPI_Datatype rtype, MPI_Comm comm) {
  CAMPI_POST_AND_WAIT(MPI_Iallgather(sbuf, scount, stype, rbuf, rcount, rtype, comm, req),
                      MPI_STATUS_IGNORE)
}
request_awaiter Allgatherv(const void *sbuf, int scount, MPI_Datatype stype, void *rbuf,
                           const int *rcounts, const int *displs, MPI_Datatype rtype,
                           MPI_Comm comm) {
  CAMPI_POST_AND_WAIT(MPI_Iallgatherv(sbuf, scount, stype, rbuf, rcounts, displs, rtype, comm,
                                      req),
                      MPI_STATUS_IGNORE)
}
request_awaiter Scatter(const void *sbuf, int scount, MPI_Datatype stype, void *rbuf,
                        int rcount, MPI_Datatype rtype, int root, MPI_Comm comm) {
  CAMPI_POST_AND_WAIT(MPI_Iscatter(sbuf, scount, stype, rbuf, rcount, rtype, root, comm, req),
                      MPI_STATUS_IGNORE)
}
request_awaiter Scatterv(const void *sbuf, const int *scounts, const int *displs,
                         MPI_Datatype stype, void *rbuf, int rcount, MPI_Datatype rtype,
                         int root, MPI_Comm comm) {
  CAMPI_POST_AND_WAIT(MPI_Iscatterv(sbuf, scounts, displs, stype, rbuf, rcount, rtype, root,
                                    comm, req),
                      MPI_STATUS_IGNORE)
}
request_awaiter Alltoall(const void *sbuf, int scount, MPI_Datatype stype, void *rbuf,
                         int rcount, MPI_Datatype rtype, MPI_Comm comm) {
  CAMPI_POST_AND_WAIT(MPI_Ialltoall(sbuf, scount, stype, rbuf, rcount, rtype, comm, req),
                      MPI_STATUS_IGNORE)
}
request_awaiter Alltoallv(const void *sbuf, const int *scounts, const int *sdispls,
                          MPI_Datatype stype, void *rbuf, const int *rcounts,
                          const int *rdispls, MPI_Datatype rtype, MPI_Comm comm) {
  CAMPI_POST_AND_WAIT(MPI_Ialltoallv(const_cast<void *>(sbuf), const_cast<int *>(scounts),
                                     const_cast<int *>(sdispls), stype, rbuf,
                                     const_cast<int *>(rcounts), const_cast<int *>(rdispls),
                                     rtype, comm, req),
                      MPI_STATUS_IGNORE)
}

request_awaiter Scan(const void *sbuf, void *rbuf, int count, MPI_Datatype type, MPI_Op op,
                     MPI_Comm comm) {
  CAMPI_POST_AND_WAIT(MPI_Iscan(sbuf, rbuf, count, type, op, comm, req), MPI_STATUS_IGNORE)
}
request_awaiter Exscan(const void *sbuf, void *rbuf, int count, MPI_Datatype type, MPI_Op op,
                       MPI_Comm comm) {
  CAMPI_POST_AND_WAIT(MPI_Iexscan(sbuf, rbuf, count, type, op, comm, req), MPI_STATUS_IGNORE)
}

#undef CAMPI_POST_AND_WAIT

/* ====================== tests and probes ====================== */

// As in ampi.C: test first, yield after a miss (ampiParent::yield returns at
// once in campi mode; the yield happens here, as a coroutine suspension).
task<int> Test(MPI_Request *req, int *flag, MPI_Status *sts) {
  int e = MPI_Test(req, flag, sts);
  if (!*flag) co_await yield();
  co_return e;
}
task<int> Testany(int n, MPI_Request *reqs, int *index, int *flag, MPI_Status *sts) {
  int e = MPI_Testany(n, reqs, index, flag, sts);
  if (!*flag) co_await yield();
  co_return e;
}
task<int> Testall(int n, MPI_Request *reqs, int *flag, MPI_Status *sts) {
  int e = MPI_Testall(n, reqs, flag, sts);
  if (!*flag) co_await yield();
  co_return e;
}
task<int> Testsome(int n, MPI_Request *reqs, int *outcount, int *indices, MPI_Status *sts) {
  int e = MPI_Testsome(n, reqs, outcount, indices, sts);
  if (*outcount == 0) co_await yield();
  co_return e;
}
task<int> Request_get_status(MPI_Request req, int *flag, MPI_Status *sts) {
  int e = MPI_Request_get_status(req, flag, sts);
  if (!*flag) co_await yield();
  co_return e;
}
task<int> Iprobe(int src, int tag, MPI_Comm comm, int *flag, MPI_Status *sts) {
  int e = MPI_Iprobe(src, tag, comm, flag, sts);
  if (e == MPI_SUCCESS && !*flag) co_await yield();
  co_return e;
}
task<int> Improbe(int src, int tag, MPI_Comm comm, int *flag, MPI_Message *msg,
                  MPI_Status *sts) {
  int e = MPI_Improbe(src, tag, comm, flag, msg, sts);
  if (e == MPI_SUCCESS && !*flag) co_await yield();
  co_return e;
}

/* ====================== communicator creation ====================== */

// ampi::split, with the suspension replaced by an await on this comm's
// unblock() (sent by ampi::registrationFinish when the new arrays exist).
task<int> Comm_split(MPI_Comm src, int color, int key, MPI_Comm *dest) {
  ampi *ptr = getAmpiParent()->comm2ampi(src);
  const int before = ptr->getNumUnblocks();
  ptr->splitPost(color, key, ptr->getCommStruct().getType());
  while (ptr->getNumUnblocks() == before) co_await suspend();
  ptr->splitFinish(dest);
  if (color == MPI_UNDEFINED) *dest = MPI_COMM_NULL;
  co_return MPI_SUCCESS;
}

// MPI_Comm_dup = ampi::topoDup (split + topology copy), attributes, barrier
task<int> Comm_dup(MPI_Comm comm, MPI_Comm *newcomm) {
  {
    ampi *ptr = getAmpiInstance(comm);
    int topoType, rank = ptr->getRank();
    MPI_Topo_test(comm, &topoType);
    const int before = ptr->getNumUnblocks();
    ptr->splitPost(0, rank, getAmpiParent()->getCommStruct(comm).getType());
    while (ptr->getNumUnblocks() == before) co_await suspend();
    ptr->splitFinish(newcomm);
    ptr->topoDupFinish(topoType, comm, newcomm);
  }
  ampiParent *parent = getAmpiParent();
  auto &old_attr = parent->getAttributes(comm);
  auto &new_attr = parent->getAttributes(*newcomm);
  int ret = parent->dupUserAttributes(comm, old_attr, new_attr);
  co_await Barrier(comm);
  co_return ret;
}

// MPI_Comm_dup_with_info: MPI_Comm_dup, then MPI_Comm_set_info (a no-op in AMPI)
task<int> Comm_dup_with_info(MPI_Comm comm, MPI_Info info, MPI_Comm *newcomm) {
  int ret = co_await Comm_dup(comm, newcomm);
  if (ret != MPI_SUCCESS) co_return ret;
  MPI_Comm_set_info(*newcomm, info);
  co_return MPI_SUCCESS;
}

// MPI_Comm_create for intracommunicators (a split, as in ampi.C)
task<int> Comm_create(MPI_Comm comm, MPI_Group group, MPI_Comm *newcomm) {
  std::vector<int> vec = getAmpiParent()->group2vec(group);
  if (vec.size() == 0) {
    *newcomm = MPI_COMM_NULL;
    co_return MPI_SUCCESS;
  }
  if (getAmpiParent()->isInter(comm))
    CkAbort("campi: MPI_Comm_create on an intercommunicator is not supported yet");
  int rank_in_group, color, key;
  MPI_Group_rank(group, &rank_in_group);
  if (rank_in_group == MPI_UNDEFINED) {
    color = MPI_UNDEFINED;
    key = 0;
  } else {
    MPI_Group group_of_comm;
    MPI_Comm_group(comm, &group_of_comm);
    int zero = 0;
    MPI_Group_translate_ranks(group, 1, &zero, group_of_comm, &color);
    key = rank_in_group;
  }
  co_return co_await Comm_split(comm, color, key, newcomm);
}

/* ====================== Init / Finalize ====================== */

static void array_created(void *param, void *msg) {
  Rank *r = (Rank *)param;
  r->createdAid = ((CkArrayCreatedMsg *)msg)->aid;
  r->arrayCreated = true;
  delete (CkArrayCreatedMsg *)msg;
  r->tc->resume();
}

// TCharm::semaGet, awaiting instead of suspending the thread
static task<void *> sema_get(int id) {
  TCharm *tc = TCharm::get();
  while (tc->semaPeek(id) == nullptr) {
    tc->findSema(id)->thread = CthSelf(); // non-null: semaPut then calls resume()
    co_await suspend();
    tc = TCharm::get();
  }
  co_return tc->semaGet(id);
}

// createCommSelf (ampi.C) with the awaitable Comm_create
static task<void> create_comm_self() {
  MPI_Comm selfComm;
  MPI_Group worldGroup, selfGroup;
  int ranks[1] = { getAmpiInstance(MPI_COMM_WORLD)->getRank() };
  MPI_Comm_group(MPI_COMM_WORLD, &worldGroup);
  MPI_Group_incl(worldGroup, 1, ranks, &selfGroup);
  co_await Comm_create(MPI_COMM_WORLD, selfGroup, &selfComm);
  MPI_Comm_set_name(selfComm, "MPI_COMM_SELF");
  CkAssert(selfComm == MPI_COMM_SELF);
}

// ampiInit (ampi.C) with awaits in place of its blocking waits
static task<void> ampi_init() {
  if (CtvAccess(ampiInitDone)) co_return; /* Already called ampiInit */
  Rank *r = current();

  int nchunks = TCHARM_Num_elements();
  CkArrayOptions opts;
  CkArrayID parentAid;
  if (TCHARM_Element() == 0) { /* I'm responsible for building the arrays: */
    CkArrayID threads;
    opts = ampiInitRootArrayOptions(&threads, &nchunks);
    r->arrayCreated = false;
    CProxy_ampiParent::ckNew(threads, nchunks, opts, CkCallback(array_created, r));
    while (!r->arrayCreated) co_await suspend();
    parentAid = r->createdAid;
    CAMPI_DBG("init: ampiParent array created\n");
  }
  (void)co_await sema_get(AMPI_BARRIER_SEMAID);
  r = current();
  CAMPI_DBG("init: rank %d has its ampiParent %p\n", TCHARM_Element(), (void *)r->parent);
  CtvAccess(ampiPtr) = r->parent; // set by ampiParent::prepareCtv (campi_parent_attached)

  if (TCHARM_Element() == 0) {
    // Make a new ampi array
    CkArrayID empty;
    ampiCommStruct worldComm(MPI_COMM_WORLD, empty, nchunks);
    r->arrayCreated = false;
    CProxy_ampi::ckNew(parentAid, worldComm, opts, CkCallback(array_created, r));
    while (!r->arrayCreated) co_await suspend();
  }

  // Find our ampi object:
  ampi *ptr = (ampi *)co_await sema_get(AMPI_TCHARM_SEMAID);
  CAMPI_DBG("init: rank %d has its MPI_COMM_WORLD ampi\n", TCHARM_Element());
  ampiParent *pptr = ampiInitWorldLocal(ptr, nchunks);
  current()->initDone = true;
  while (pptr->ampiInitCallDone != 1) {
    co_await suspend(); // woken by ampi::setInitDoneFlag
    pptr = getAmpiParent();
  }

  CAMPI_DBG("init: rank %d past allInitDone\n", TCHARM_Element());
  co_await create_comm_self();
  CAMPI_DBG("init: rank %d has MPI_COMM_SELF\n", TCHARM_Element());
  removeUnimportantArrayObjsfromPeCache();
}

task<int> Init(int *p_argc, char ***p_argv) {
  // MPI_Init around ampiInit
  char **argv;
  if (p_argv) argv = *p_argv;
  else argv = CkGetArgv();
  co_await ampi_init();
  if (p_argc) *p_argc = CmiGetArgc(argv);
  co_return MPI_SUCCESS;
}

task<int> Init_thread(int *p_argc, char ***p_argv, int required, int *provided) {
  // MPI_Init_thread: record the thread level, then MPI_Init
  if (required < MPI_THREAD_SINGLE || required > MPI_THREAD_MULTIPLE)
    co_return MPI_ERR_ARG;
  // AMPI does not support MPI_THREAD_SERIALIZED or MPI_THREAD_MULTIPLE
  CkpvAccess(ampiThreadLevel) =
      (required == MPI_THREAD_SINGLE) ? MPI_THREAD_SINGLE : MPI_THREAD_FUNNELED;
  *provided = CkpvAccess(ampiThreadLevel);
  co_return co_await Init(p_argc, p_argv);
}

task<int> Finalize() {
  // MPI_Finalize except AMPI_Exit: the rank exits when its main returns.
  int ret = ampiFinalizeBody();
  current()->finalized = true;
  co_return ret;
}

} // namespace campi

#endif // coroutine support

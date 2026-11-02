/*
 * campi (coroutine AMPI), core header: the rank task type and the awaitable
 * replacements for AMPI's blocking calls. Requires C++20 (<coroutine>).
 *
 * An AMPI rank in campi mode is a C++20 coroutine instead of a user-level
 * thread. Every call that would block a thread-based rank is awaited with
 * co_await here: it posts AMPI's nonblocking variant and suspends until
 * AMPI's delivery code wakes the rank (TCharm::resume, which campi redirects
 * from CthAwaken to a coroutine resume). The simple calls return a
 * request_awaiter (no coroutine frame); calls with internal awaited loops
 * (Init, Finalize, tests and probes, communicator creation) are tasks.
 *
 * Applications normally include campi.h (this header plus macros that
 * redirect the blocking MPI names to co_await expressions) and define their
 * main with CAMPI_MAIN. campi.C includes only this core header.
 */
#ifndef CAMPI_CORE_H
#define CAMPI_CORE_H

#if !defined(__cplusplus) || __cplusplus < 202002L
#error "campi requires C++20: compile with -std=c++20"
#endif

#include <coroutine>
#include <type_traits>
#include <utility>
#include "mpi.h"

namespace campi {

struct Rank; // per-rank record, defined in campi.C

// Called by campi's coroutine machinery; defined in campi.C.
[[noreturn]] void abort_unhandled_exception() noexcept;
// Record h as the frame to resume when the running rank is next woken.
void set_resume_point(std::coroutine_handle<> h) noexcept;
class request_awaiter;
// Same, for a rank blocked in a request_awaiter: on each wake-up campi first
// calls w->poll() and resumes h only once w's requests are complete.
void set_resume_point(std::coroutine_handle<> h, request_awaiter *w) noexcept;

/* ---------------- task<T>: the rank's coroutine type ---------------- */

template <class T> class task;

namespace detail {

struct promise_base {
  std::coroutine_handle<> continuation; // the awaiting coroutine, if nested

  std::suspend_always initial_suspend() const noexcept { return {}; }

  struct final_awaiter {
    bool await_ready() const noexcept { return false; }
    template <class P>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<P> h) const noexcept {
      // A nested task resumes the task that awaited it (symmetric transfer);
      // the outermost task returns control to campi's resume loop.
      std::coroutine_handle<> c = h.promise().continuation;
      return c ? c : std::noop_coroutine();
    }
    void await_resume() const noexcept {}
  };
  final_awaiter final_suspend() const noexcept { return {}; }

  void unhandled_exception() const noexcept { abort_unhandled_exception(); }
};

template <class T> struct promise : promise_base {
  T value{};
  task<T> get_return_object() noexcept;
  template <class U> void return_value(U &&v) { value = std::forward<U>(v); }
};

template <> struct promise<void> : promise_base {
  task<void> get_return_object() noexcept;
  void return_void() const noexcept {}
};

} // namespace detail

template <class T = void> class [[nodiscard]] task {
 public:
  using promise_type = detail::promise<T>;
  using handle_type = std::coroutine_handle<promise_type>;

  task() noexcept = default;
  explicit task(handle_type h) noexcept : h_(h) {}
  task(task &&o) noexcept : h_(std::exchange(o.h_, {})) {}
  task &operator=(task &&o) noexcept {
    if (this != &o) { if (h_) h_.destroy(); h_ = std::exchange(o.h_, {}); }
    return *this;
  }
  task(const task &) = delete;
  task &operator=(const task &) = delete;
  ~task() { if (h_) h_.destroy(); }

  // Awaiting a task starts it; it resumes the awaiter when it finishes.
  bool await_ready() const noexcept { return !h_ || h_.done(); }
  std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting) noexcept {
    h_.promise().continuation = awaiting;
    return h_;
  }
  T await_resume() {
    if constexpr (!std::is_void_v<T>) return std::move(h_.promise().value);
  }

  // For campi's rank start: take ownership of the coroutine frame.
  handle_type release() noexcept { return std::exchange(h_, {}); }

 private:
  handle_type h_{};
};

namespace detail {
template <class T> task<T> promise<T>::get_return_object() noexcept {
  return task<T>(std::coroutine_handle<promise<T>>::from_promise(*this));
}
inline task<void> promise<void>::get_return_object() noexcept {
  return task<void>(std::coroutine_handle<promise<void>>::from_promise(*this));
}
} // namespace detail

/* ---------------- rank main registration ---------------- */

using main_fn = task<int> (*)(int argc, char **argv);
extern main_fn registered_main; // non-null selects campi mode
void register_main(main_fn f) noexcept;
struct main_registrar {
  explicit main_registrar(main_fn f) noexcept { register_main(f); }
};

/* ---------------- suspension primitive ---------------- */

// Return to the scheduler until something wakes this rank. Wake-ups may be
// spurious, so always use it in a loop that re-checks the condition:
//   while (!pred()) co_await campi::suspend();
struct suspend_awaiter {
  bool await_ready() const noexcept { return false; }
  void await_suspend(std::coroutine_handle<> h) const noexcept { set_resume_point(h); }
  void await_resume() const noexcept {}
};
inline suspend_awaiter suspend() noexcept { return {}; }

// Like suspend(), but also schedules a wake-up of this rank, so the rank runs
// again after the messages already queued on this PE (cf. AMPI_Yield).
task<void> yield();

// An awaitable that is already complete with an int result.
struct ready_int {
  int v;
  bool await_ready() const noexcept { return true; }
  void await_suspend(std::coroutine_handle<>) const noexcept {}
  int await_resume() const noexcept { return v; }
};

/* ---------------- request completion ---------------- */

// The awaitable for every blocking call that is "post the nonblocking
// variant, wait for its request(s), finish": Send, Recv, Wait, Waitall,
// Sendrecv and the plain collectives. It is a plain object, not a coroutine,
// so a call allocates no coroutine frame; it lives in the awaiting
// coroutine's frame while the rank is suspended.
//  - The factory function (Send, Recv, ...) posts the request(s).
//  - await_ready: true if posting failed or the requests are complete.
//  - await_suspend: marks the requests blocked (as their wait() does before
//    it suspends a thread) and records the resume point with this object.
//  - On each wake-up campi_resume calls poll(), which clears the blocked
//    bookkeeping and, if the requests are still incomplete, marks them
//    blocked again and leaves the rank suspended (wake-ups may be spurious).
//  - await_resume: MPI_Wait/MPI_Waitall on the completed request(s).
// The object holds no pointers into itself, so it may be copied freely
// before it is awaited.
class [[nodiscard]] request_awaiter {
 public:
  enum kind : unsigned char { WAIT_ONE, WAIT_ALL, SENDRECV };

  // Wait on n requests: own_ (ext == nullptr) or the caller's array ext.
  request_awaiter(kind k, int n, MPI_Request *ext, MPI_Status *sts) noexcept
      : ext_(ext), sts_(sts), n_(n), kind_(k) {
    own_[0] = own_[1] = MPI_REQUEST_NULL;
  }
  // Posting failed with error code e: complete at once with result e.
  static request_awaiter failed(int e) noexcept {
    request_awaiter w(WAIT_ONE, 0, nullptr, nullptr);
    w.err_ = e;
    return w;
  }

  MPI_Request *own() noexcept { return own_; }
  void set_error(int e) noexcept { err_ = e; }

  bool await_ready() noexcept;
  void await_suspend(std::coroutine_handle<> h) noexcept;
  int await_resume() noexcept;
  bool poll() noexcept; // called by campi_resume on a wake-up; true: complete

 private:
  MPI_Request *reqs() noexcept { return ext_ ? ext_ : own_; }
  bool done() noexcept;
  void block() noexcept;
  void unblock() noexcept;

  MPI_Request own_[2];
  MPI_Request *ext_;
  MPI_Status *sts_;
  int n_;
  int err_ = MPI_SUCCESS;
  kind kind_;
};

request_awaiter Wait(MPI_Request *req, MPI_Status *sts);
request_awaiter Waitall(int n, MPI_Request *reqs, MPI_Status *sts);
inline request_awaiter wait(MPI_Request *req, MPI_Status *sts) { return Wait(req, sts); }
inline request_awaiter waitall(int n, MPI_Request *reqs, MPI_Status *sts) {
  return Waitall(n, reqs, sts);
}

/* ---------------- blocking point-to-point ---------------- */

request_awaiter Send(const void *buf, int count, MPI_Datatype type, int dest, int tag,
                     MPI_Comm comm);
request_awaiter Ssend(const void *buf, int count, MPI_Datatype type, int dest, int tag,
                      MPI_Comm comm);
request_awaiter Recv(void *buf, int count, MPI_Datatype type, int src, int tag, MPI_Comm comm,
                     MPI_Status *sts);
request_awaiter Sendrecv(const void *sbuf, int scount, MPI_Datatype stype, int dest, int stag,
                         void *rbuf, int rcount, MPI_Datatype rtype, int src, int rtag,
                         MPI_Comm comm, MPI_Status *sts);

/* ---------------- blocking collectives ---------------- */

request_awaiter Barrier(MPI_Comm comm);
request_awaiter Bcast(void *buf, int count, MPI_Datatype type, int root, MPI_Comm comm);
request_awaiter Reduce(const void *sbuf, void *rbuf, int count, MPI_Datatype type, MPI_Op op,
                       int root, MPI_Comm comm);
request_awaiter Allreduce(const void *sbuf, void *rbuf, int count, MPI_Datatype type, MPI_Op op,
                          MPI_Comm comm);
request_awaiter Gather(const void *sbuf, int scount, MPI_Datatype stype, void *rbuf, int rcount,
                       MPI_Datatype rtype, int root, MPI_Comm comm);
request_awaiter Gatherv(const void *sbuf, int scount, MPI_Datatype stype, void *rbuf,
                        const int *rcounts, const int *displs, MPI_Datatype rtype, int root,
                        MPI_Comm comm);
request_awaiter Allgather(const void *sbuf, int scount, MPI_Datatype stype, void *rbuf,
                          int rcount, MPI_Datatype rtype, MPI_Comm comm);
request_awaiter Allgatherv(const void *sbuf, int scount, MPI_Datatype stype, void *rbuf,
                           const int *rcounts, const int *displs, MPI_Datatype rtype,
                           MPI_Comm comm);
request_awaiter Scatter(const void *sbuf, int scount, MPI_Datatype stype, void *rbuf,
                        int rcount, MPI_Datatype rtype, int root, MPI_Comm comm);
request_awaiter Scatterv(const void *sbuf, const int *scounts, const int *displs,
                         MPI_Datatype stype, void *rbuf, int rcount, MPI_Datatype rtype,
                         int root, MPI_Comm comm);
request_awaiter Alltoall(const void *sbuf, int scount, MPI_Datatype stype, void *rbuf,
                         int rcount, MPI_Datatype rtype, MPI_Comm comm);
request_awaiter Alltoallv(const void *sbuf, const int *scounts, const int *sdispls,
                          MPI_Datatype stype, void *rbuf, const int *rcounts,
                          const int *rdispls, MPI_Datatype rtype, MPI_Comm comm);
request_awaiter Scan(const void *sbuf, void *rbuf, int count, MPI_Datatype type, MPI_Op op,
                     MPI_Comm comm);
request_awaiter Exscan(const void *sbuf, void *rbuf, int count, MPI_Datatype type, MPI_Op op,
                       MPI_Comm comm);

/* ---------------- tests and probes ---------------- */

// A failed test or probe is a scheduling point in thread AMPI (it yields the
// thread so that messages can arrive); a coroutine must suspend instead, so
// these are awaitables: the AMPI routine, then a yield after a miss.
task<int> Test(MPI_Request *req, int *flag, MPI_Status *sts);
task<int> Testany(int n, MPI_Request *reqs, int *index, int *flag, MPI_Status *sts);
task<int> Testall(int n, MPI_Request *reqs, int *flag, MPI_Status *sts);
task<int> Testsome(int n, MPI_Request *reqs, int *outcount, int *indices, MPI_Status *sts);
task<int> Request_get_status(MPI_Request req, int *flag, MPI_Status *sts);
task<int> Iprobe(int src, int tag, MPI_Comm comm, int *flag, MPI_Status *sts);
task<int> Improbe(int src, int tag, MPI_Comm comm, int *flag, MPI_Message *msg,
                  MPI_Status *sts);

/* ---------------- communicator creation ---------------- */

task<int> Comm_split(MPI_Comm comm, int color, int key, MPI_Comm *newcomm);
task<int> Comm_dup(MPI_Comm comm, MPI_Comm *newcomm);
task<int> Comm_dup_with_info(MPI_Comm comm, MPI_Info info, MPI_Comm *newcomm);
task<int> Comm_create(MPI_Comm comm, MPI_Group group, MPI_Comm *newcomm);

/* ---------------- startup, shutdown, migration ---------------- */

task<int> Init(int *argc, char ***argv);
task<int> Init_thread(int *argc, char ***argv, int required, int *provided);
task<int> Finalize();
// Increment 1: migration is not supported; this keeps converted programs
// (AMPI_Migrate calls) compiling and returns MPI_SUCCESS at once.
inline ready_int Migrate(MPI_Info) noexcept { return ready_int{MPI_SUCCESS}; }

} // namespace campi

#endif // CAMPI_CORE_H

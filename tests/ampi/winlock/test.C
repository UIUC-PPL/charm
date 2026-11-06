// Passive-target synchronization: MPI_Win_lock in shared and exclusive mode,
// MPI_Win_lock_all, and the MPI_Win_flush family (MPI-3.1 sections 11.5.3
// and 11.5.4).
//
// Usage: winlock [badunlock]
//   (no argument): the tests below; prints "All tests passed" on success.
//   badunlock: rank 0 calls MPI_Win_unlock on a window it has not locked;
//              the call must fail with MPI_ERR_RMA_SYNC.
//
// Every rank creates a window of ints; the lock tests target rank 0's window.
// The ranks yield (AMPI_Yield) while they hold a lock, so other ranks on the
// same PE run and send their lock requests during the hold.
#include <stdio.h>
#include <string.h>
#include <vector>
#include "mpi.h"

// Per-rank state lives on the stack: AMPI ranks on one PE share globals.
struct Ctx {
  int rank, size;
  int errors;
};

static void check(Ctx& c, bool ok, const char* test, const char* what, long got, long want)
{
  if (!ok) {
    printf("[%d] FAIL %s %s: got %ld, expected %ld\n", c.rank, test, what, got, want);
    c.errors++;
  }
}

static void holdFor(double seconds)
{
  double end = MPI_Wtime() + seconds;
  while (MPI_Wtime() < end) AMPI_Yield();
}

// Slots of each window.
enum { COUNTER = 0, EXCL_IN = 1, FLUSHED = 2, DONE_BASE = 3 };

static int getInt(int target, int slot, MPI_Win win)
{
  int v = -1;
  MPI_Get(&v, 1, MPI_INT, target, slot, 1, MPI_INT, win);
  return v;
}

static void putInt(int v, int target, int slot, MPI_Win win)
{
  MPI_Put(&v, 1, MPI_INT, target, slot, 1, MPI_INT, win);
}

// (a) Every rank holds a shared lock on rank 0's window at the same time.
static void testShared(Ctx& c, MPI_Win win)
{
  int rc = MPI_Win_lock(MPI_LOCK_SHARED, 0, 0, win);
  check(c, rc == MPI_SUCCESS, "shared", "lock return", rc, MPI_SUCCESS);
  int v = getInt(0, COUNTER, win);
  check(c, v == 0, "shared", "counter before the exclusive test", v, 0);
  holdFor(0.002);
  rc = MPI_Win_unlock(0, win);
  check(c, rc == MPI_SUCCESS, "shared", "unlock return", rc, MPI_SUCCESS);
  MPI_Barrier(MPI_COMM_WORLD);
}

// (b) Read-modify-write of a counter under an exclusive lock. If another rank
// could enter while one holds the lock, increments would be lost.
static void testExclusive(Ctx& c, MPI_Win win)
{
  const int iters = 5;
  for (int i = 0; i < iters; i++) {
    MPI_Win_lock(MPI_LOCK_EXCLUSIVE, 0, 0, win);
    int v = getInt(0, COUNTER, win);
    holdFor(0.001);
    putInt(v + 1, 0, COUNTER, win);
    MPI_Win_unlock(0, win);
  }
  MPI_Barrier(MPI_COMM_WORLD);
  if (c.rank == 0) {
    MPI_Win_lock(MPI_LOCK_SHARED, 0, 0, win);
    int v = getInt(0, COUNTER, win);
    MPI_Win_unlock(0, win);
    check(c, v == c.size * iters, "exclusive", "final counter", v, c.size * iters);
  }
  MPI_Barrier(MPI_COMM_WORLD);
}

// (c) Rank 1 requests an exclusive lock while every other rank holds a shared
// one. Each shared holder tells rank 1 that it holds the lock, keeps it 30 ms,
// checks that the exclusive holder has not entered, and marks its DONE slot
// before unlocking. Rank 1, once granted, must find every DONE slot set.
static void testExclusiveAfterShared(Ctx& c, MPI_Win win)
{
  if (c.size < 2) return;
  const int excl = 1;
  if (c.rank != excl) {
    MPI_Win_lock(MPI_LOCK_SHARED, 0, 0, win);
    MPI_Send(&c.rank, 1, MPI_INT, excl, 7, MPI_COMM_WORLD);
    holdFor(0.03);
    int in = getInt(0, EXCL_IN, win);
    check(c, in == 0, "excl-after-shared", "exclusive holder entered during a shared hold", in, 0);
    putInt(1, 0, DONE_BASE + c.rank, win);
    MPI_Win_unlock(0, win);
  } else {
    for (int i = 0; i < c.size - 1; i++) {
      int who;
      MPI_Recv(&who, 1, MPI_INT, MPI_ANY_SOURCE, 7, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    }
    MPI_Win_lock(MPI_LOCK_EXCLUSIVE, 0, 0, win);
    putInt(1, 0, EXCL_IN, win);
    for (int r = 0; r < c.size; r++) {
      if (r == excl) continue;
      int done = getInt(0, DONE_BASE + r, win);
      if (done != 1) {
        printf("[%d] FAIL excl-after-shared: granted before rank %d released its shared lock\n",
               c.rank, r);
        c.errors++;
      }
    }
    MPI_Win_unlock(0, win);
  }
  MPI_Barrier(MPI_COMM_WORLD);
}

// (d) A Put is visible at the target after MPI_Win_flush, read back within the
// same epoch, and in the target's window after the unlock.
static void testFlush(Ctx& c, MPI_Win win, const int* wbuf)
{
  int target = (c.rank + 1) % c.size;
  int source = (c.rank + c.size - 1) % c.size;
  MPI_Win_lock(MPI_LOCK_EXCLUSIVE, target, 0, win);
  putInt(5000 + c.rank, target, FLUSHED, win);
  int rc = MPI_Win_flush(target, win);
  check(c, rc == MPI_SUCCESS, "flush", "MPI_Win_flush return", rc, MPI_SUCCESS);
  int v = getInt(target, FLUSHED, win);
  check(c, v == 5000 + c.rank, "flush", "value read back after flush", v, 5000 + c.rank);
  rc = MPI_Win_flush_local(target, win);
  check(c, rc == MPI_SUCCESS, "flush", "MPI_Win_flush_local return", rc, MPI_SUCCESS);
  rc = MPI_Win_flush_all(win);
  check(c, rc == MPI_SUCCESS, "flush", "MPI_Win_flush_all return", rc, MPI_SUCCESS);
  rc = MPI_Win_flush_local_all(win);
  check(c, rc == MPI_SUCCESS, "flush", "MPI_Win_flush_local_all return", rc, MPI_SUCCESS);
  MPI_Win_unlock(target, win);
  MPI_Barrier(MPI_COMM_WORLD);
  MPI_Win_lock(MPI_LOCK_SHARED, c.rank, 0, win);
  check(c, wbuf[FLUSHED] == 5000 + source, "flush", "own window after flush+unlock",
        wbuf[FLUSHED], 5000 + source);
  MPI_Win_unlock(c.rank, win);
  MPI_Barrier(MPI_COMM_WORLD);
}

// (e) MPI_Win_lock_all takes a shared lock on every rank's window.
static void testLockAll(Ctx& c, MPI_Win win)
{
  int rc = MPI_Win_lock_all(0, win);
  check(c, rc == MPI_SUCCESS, "lock_all", "lock_all return", rc, MPI_SUCCESS);
  for (int r = 0; r < c.size; r++) {
    int src = (r + c.size - 1) % c.size;
    int v = getInt(r, FLUSHED, win);
    check(c, v == 5000 + src, "lock_all", "FLUSHED slot", v, 5000 + src);
  }
  holdFor(0.001);
  rc = MPI_Win_unlock_all(win);
  check(c, rc == MPI_SUCCESS, "lock_all", "unlock_all return", rc, MPI_SUCCESS);
  MPI_Barrier(MPI_COMM_WORLD);
}

int main(int argc, char** argv)
{
  MPI_Init(&argc, &argv);
  Ctx c;
  c.errors = 0;
  MPI_Comm_rank(MPI_COMM_WORLD, &c.rank);
  MPI_Comm_size(MPI_COMM_WORLD, &c.size);

  std::vector<int> wbuf(DONE_BASE + c.size, 0);
  MPI_Win win;
  MPI_Win_create(wbuf.data(), wbuf.size() * sizeof(int), sizeof(int), MPI_INFO_NULL,
                 MPI_COMM_WORLD, &win);

  if (argc > 1 && !strcmp(argv[1], "badunlock")) {
    // AMPI built with AMPI_ERROR_CHECKING aborts with the error string;
    // otherwise MPI_Win_unlock returns the error code, printed here.
    int rc = MPI_SUCCESS;
    if (c.rank == 0 && c.size > 1) rc = MPI_Win_unlock(1, win); // not locked: must fail
    if (c.rank == 0) {
      char msg[MPI_MAX_ERROR_STRING];
      int len = 0;
      MPI_Error_string(rc, msg, &len);
      printf("badunlock: MPI_Win_unlock without a lock returned %s\n", msg);
    }
    MPI_Win_free(&win);
    MPI_Finalize();
    return rc == MPI_ERR_RMA_SYNC ? 0 : 1;
  }

  testShared(c, win);
  testExclusive(c, win);
  testExclusiveAfterShared(c, win);
  testFlush(c, win, wbuf.data());
  testLockAll(c, win);

  MPI_Win_free(&win);
  int total = 0;
  MPI_Allreduce(&c.errors, &total, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
  if (c.rank == 0) {
    if (total == 0) printf("All tests passed\n");
    else printf("%d failures\n", total);
  }
  MPI_Finalize();
  return total == 0 ? 0 : 1;
}

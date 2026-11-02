/**
 * MPI_Waitall (and MPI_Waitany, MPI_Waitsome, MPI_Testall) over requests of
 * non-blocking alltoall-family collectives, and several such collectives
 * outstanding on one communicator (charm issue #4097, items 1 and 2).
 *
 * Item 1: an MPI_Ialltoall request is an ATAReq that holds the requests of its
 * point-to-point blocks. MPI_Waitall counted the ATAReq as one blocked request,
 * but completions of its blocks never decremented that count, so a rank in
 * MPI_Waitall over an incomplete MPI_Ialltoall was never resumed. Stage
 * "single" is that case: rank 0 reaches MPI_Waitall before the other ranks
 * have entered the MPI_Ialltoall (on one PE it always does).
 *
 * Item 2: blocks of two outstanding MPI_Ialltoalls from the same sender use
 * the same tag and source, so they must be matched in send order. Stage
 * "large" sends blocks big enough for the zero-copy and the rendezvous
 * protocols between processes; its non-contiguous blocks also found that
 * MPI_Ialltoall placed send blocks by the type's size, not its extent.
 *
 * Every received value is checked. A failure calls MPI_Abort before
 * MPI_Finalize (AMPI's MPI_Finalize does not return, so a nonzero return from
 * main after it would be lost).
 *
 * Usage: ./waitall_ata +p1 +vp4 [stage]   (no stage: run all stages)
 * Buffers come from malloc, which is isomalloc memory under AMPI and migrates
 * with the rank (operator new memory does not on macOS).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <initializer_list>
#include "mpi.h"

// Rank and size are locals: a global would be shared by all virtual ranks of a process.
static void getRS(int &rank, int &size)
{
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &size);
}

static void fail(const char *stage, const char *what, long got, long want)
{
  int rank, size;
  getRS(rank, size);
  printf("[%d] ERROR in stage %s: %s: got %ld, expected %ld\n", rank, stage, what, got, want);
  fflush(stdout);
  MPI_Abort(MPI_COMM_WORLD, 1);
}

// Value of element k of the block that src sends to dst in collective number id
static int val(int id, int src, int dst, int k)
{
  return id * 1000003 + src * 1009 + dst * 31 + k;
}

// A block of `count` ints per destination. With stride > 1 the ints are
// `stride` apart and the block is sent as one element of a non-contiguous
// vector type, resized so that consecutive blocks follow each other; the
// receive side uses the same type, so the ints land `stride` apart again.
struct Ata {
  int id, count, stride, sendCount;
  int *sbuf, *rbuf;
  MPI_Datatype type;
  MPI_Request req;
};

static void ataInit(Ata &a, int id, int count, int stride = 1)
{
  int rank, size;
  getRS(rank, size);
  a.id = id;
  a.count = count;
  a.stride = stride;
  a.sbuf = (int *)malloc(sizeof(int) * count * stride * size);
  a.rbuf = (int *)malloc(sizeof(int) * count * stride * size);
  for (int i = 0; i < count * stride * size; i++) a.sbuf[i] = a.rbuf[i] = -1;
  for (int d = 0; d < size; d++)
    for (int k = 0; k < count; k++)
      a.sbuf[(d * count + k) * stride] = val(id, rank, d, k);
  if (stride == 1) {
    a.type = MPI_INT;
    a.sendCount = count;
  } else {
    MPI_Datatype vec;
    MPI_Type_vector(count, 1, stride, MPI_INT, &vec);
    MPI_Type_create_resized(vec, 0, (MPI_Aint)count * stride * sizeof(int), &a.type);
    MPI_Type_commit(&a.type);
    MPI_Type_free(&vec);
    a.sendCount = 1;
  }
  a.req = MPI_REQUEST_NULL;
}

static void ataStart(Ata &a)
{
  int rank, size;
  getRS(rank, size);
  MPI_Ialltoall(a.sbuf, a.sendCount, a.type, a.rbuf, a.sendCount, a.type, MPI_COMM_WORLD, &a.req);
}

static void ataCheck(const char *stage, Ata &a)
{
  int rank, size;
  getRS(rank, size);
  if (a.req != MPI_REQUEST_NULL) fail(stage, "request not set to MPI_REQUEST_NULL", a.req, MPI_REQUEST_NULL);
  for (int s = 0; s < size; s++)
    for (int k = 0; k < a.count; k++) {
      int got = a.rbuf[(s * a.count + k) * a.stride];
      int want = val(a.id, s, rank, k);
      if (got != want) {
        char what[128];
        snprintf(what, sizeof(what), "alltoall %d count %d stride %d, block from %d, element %d",
                 a.id, a.count, a.stride, s, k);
        fail(stage, what, got, want);
      }
    }
  if (a.stride > 1) MPI_Type_free(&a.type);
  free(a.sbuf);
  free(a.rbuf);
}

// 1. MPI_Waitall over a single incomplete MPI_Ialltoall request
static void stageSingle()
{
  int rank, size;
  getRS(rank, size);
  const char *st = "single";
  for (int count : {1, 3000, 10240}) { // 10240 ints: above the sync-message thresholds
    Ata a;
    ataInit(a, 1, count);
    ataStart(a);
    MPI_Waitall(1, &a.req, MPI_STATUSES_IGNORE);
    ataCheck(st, a);
  }
}

// 2. MPI_Waitall over MPI_Ialltoall, MPI_Isend/MPI_Irecv and MPI_Iallreduce
static void stageMix()
{
  int rank, size;
  getRS(rank, size);
  const char *st = "mix";
  Ata a;
  ataInit(a, 2, 5);
  int right = (rank + 1) % size, left = (rank + size - 1) % size;
  int sendv = 100 + rank, recvv = -1, sum = -1, in = rank + 1;
  MPI_Request reqs[4];
  MPI_Irecv(&recvv, 1, MPI_INT, left, 7, MPI_COMM_WORLD, &reqs[0]);
  ataStart(a);
  reqs[1] = a.req;
  MPI_Iallreduce(&in, &sum, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD, &reqs[2]);
  MPI_Isend(&sendv, 1, MPI_INT, right, 7, MPI_COMM_WORLD, &reqs[3]);
  MPI_Status sts[4];
  MPI_Waitall(4, reqs, sts);
  a.req = reqs[1];
  ataCheck(st, a);
  if (recvv != 100 + left) fail(st, "Irecv value", recvv, 100 + left);
  if (sts[0].MPI_SOURCE != left) fail(st, "Irecv status source", sts[0].MPI_SOURCE, left);
  if (sum != size * (size + 1) / 2) fail(st, "Iallreduce sum", sum, size * (size + 1) / 2);
  for (int i = 0; i < 4; i++)
    if (reqs[i] != MPI_REQUEST_NULL) fail(st, "request not freed", i, -1);
}

// 3. Two MPI_Ialltoalls outstanding on MPI_COMM_WORLD, completed in both orders
static void stageTwo()
{
  int rank, size;
  getRS(rank, size);
  const char *st = "two";
  int sizes[][2] = {{4, 4}, {10240, 4}, {4, 10240}, {10240, 10240}};
  for (auto &sz : sizes) {
    for (int order = 0; order < 3; order++) {
      Ata a, b;
      ataInit(a, 3, sz[0]);
      ataInit(b, 4, sz[1]);
      ataStart(a);
      ataStart(b);
      if (order == 0) { // first-posted first
        MPI_Wait(&a.req, MPI_STATUS_IGNORE);
        MPI_Waitall(1, &b.req, MPI_STATUSES_IGNORE);
      } else if (order == 1) { // last-posted first
        MPI_Waitall(1, &b.req, MPI_STATUSES_IGNORE);
        MPI_Wait(&a.req, MPI_STATUS_IGNORE);
      } else { // both in one Waitall, last-posted listed first
        MPI_Request r[2] = {b.req, a.req};
        MPI_Waitall(2, r, MPI_STATUSES_IGNORE);
        b.req = r[0];
        a.req = r[1];
      }
      ataCheck(st, a);
      ataCheck(st, b);
    }
  }
}

// 3b. Two outstanding MPI_Ialltoalls whose blocks are large enough to take
// the other protocols between processes: a contiguous block of 100 KB or more
// is sent with the zero-copy entry (ampi::genericRdma), and a non-contiguous
// block of 128 KB or more as a sync message whose data the receiver fetches
// (the rendezvous protocol; between processes the fetch completes later).
// Within a process, blocks above 8 KB (32 KB on SMP) are sync messages too.
static void stageLarge()
{
  int rank, size;
  getRS(rank, size);
  const char *st = "large";
  struct { int count, stride; } shapes[][2] = {
    {{25600, 1}, {4, 1}},       // 100 KB contiguous, then a small one
    {{4, 1}, {32768, 2}},       // small, then 128 KB non-contiguous
    {{32768, 2}, {25600, 1}},   // 128 KB non-contiguous, then 100 KB contiguous
    {{32768, 2}, {32768, 2}},   // both non-contiguous
  };
  for (auto &sh : shapes) {
    for (int order = 0; order < 3; order++) {
      Ata a, b;
      ataInit(a, 7, sh[0].count, sh[0].stride);
      ataInit(b, 8, sh[1].count, sh[1].stride);
      ataStart(a);
      ataStart(b);
      if (order == 0) {
        MPI_Wait(&a.req, MPI_STATUS_IGNORE);
        MPI_Wait(&b.req, MPI_STATUS_IGNORE);
      } else if (order == 1) {
        MPI_Wait(&b.req, MPI_STATUS_IGNORE);
        MPI_Wait(&a.req, MPI_STATUS_IGNORE);
      } else {
        MPI_Request r[2] = {b.req, a.req};
        MPI_Waitall(2, r, MPI_STATUSES_IGNORE);
        b.req = r[0];
        a.req = r[1];
      }
      ataCheck(st, a);
      ataCheck(st, b);
    }
  }
}

// 4. MPI_Ialltoallv: rank s sends s+d+1 ints to rank d; two outstanding
static void stageV()
{
  int rank, size;
  getRS(rank, size);
  const char *st = "alltoallv";
  for (int rep = 0; rep < 2; rep++) {
    int *scounts = (int *)malloc(sizeof(int) * size * 4);
    int *sdispls = scounts + size, *rcounts = scounts + 2 * size, *rdispls = scounts + 3 * size;
    int stot = 0, rtot = 0;
    for (int d = 0; d < size; d++) {
      scounts[d] = rank + d + 1; sdispls[d] = stot; stot += scounts[d];
      rcounts[d] = d + rank + 1; rdispls[d] = rtot; rtot += rcounts[d];
    }
    int *sbuf[2], *rbuf[2];
    MPI_Request req[2];
    for (int j = 0; j < 2; j++) {
      sbuf[j] = (int *)malloc(sizeof(int) * stot);
      rbuf[j] = (int *)malloc(sizeof(int) * rtot);
      for (int d = 0; d < size; d++)
        for (int k = 0; k < scounts[d]; k++) sbuf[j][sdispls[d] + k] = val(10 + j, rank, d, k);
      for (int i = 0; i < rtot; i++) rbuf[j][i] = -1;
      MPI_Ialltoallv(sbuf[j], scounts, sdispls, MPI_INT, rbuf[j], rcounts, rdispls, MPI_INT,
                     MPI_COMM_WORLD, &req[j]);
    }
    if (rep == 0) {
      MPI_Waitall(2, req, MPI_STATUSES_IGNORE);
    } else {
      MPI_Waitall(1, &req[1], MPI_STATUSES_IGNORE);
      MPI_Waitall(1, &req[0], MPI_STATUSES_IGNORE);
    }
    for (int j = 0; j < 2; j++) {
      for (int s = 0; s < size; s++)
        for (int k = 0; k < rcounts[s]; k++) {
          int got = rbuf[j][rdispls[s] + k], want = val(10 + j, s, rank, k);
          if (got != want) fail(st, "Ialltoallv element", got, want);
        }
      free(sbuf[j]);
      free(rbuf[j]);
    }
    free(scounts);
  }
}

// 5. MPI_Testall polling over an MPI_Ialltoall and an MPI_Irecv
static void stageTestall()
{
  int rank, size;
  getRS(rank, size);
  const char *st = "testall";
  Ata a;
  ataInit(a, 5, 6);
  int left = (rank + size - 1) % size, right = (rank + 1) % size;
  int recvv = -1, sendv = 200 + rank;
  MPI_Request reqs[2];
  MPI_Irecv(&recvv, 1, MPI_INT, left, 8, MPI_COMM_WORLD, &reqs[0]);
  ataStart(a);
  reqs[1] = a.req;
  MPI_Send(&sendv, 1, MPI_INT, right, 8, MPI_COMM_WORLD);
  int flag = 0;
  long polls = 0;
  while (!flag) {
    MPI_Testall(2, reqs, &flag, MPI_STATUSES_IGNORE);
    if (++polls > 100000000L) fail(st, "Testall never completed", polls, 0);
  }
  a.req = reqs[1];
  ataCheck(st, a);
  if (recvv != 200 + left) fail(st, "Irecv value", recvv, 200 + left);
}

// 6. MPI_Waitany and MPI_Waitsome over MPI_Ialltoall requests
static void stageAnySome()
{
  int rank, size;
  getRS(rank, size);
  const char *st = "waitany";
  {
    Ata a;
    ataInit(a, 6, 2);
    ataStart(a);
    int idx = -1;
    MPI_Waitany(1, &a.req, &idx, MPI_STATUS_IGNORE);
    if (idx != 0) fail(st, "Waitany index", idx, 0);
    ataCheck(st, a);
  }
  {
    // Two alltoalls in one Waitany/Waitany pair
    Ata a, b;
    ataInit(a, 7, 3);
    ataInit(b, 8, 10240);
    ataStart(a);
    ataStart(b);
    MPI_Request r[2] = {b.req, a.req};
    int done = 0;
    while (done < 2) {
      int idx = -1;
      MPI_Waitany(2, r, &idx, MPI_STATUS_IGNORE);
      if (idx != 0 && idx != 1) fail(st, "Waitany index", idx, 0);
      done++;
    }
    b.req = r[0];
    a.req = r[1];
    ataCheck(st, a);
    ataCheck(st, b);
  }
  st = "waitsome";
  {
    Ata a, b;
    ataInit(a, 9, 4);
    ataInit(b, 10, 10240);
    ataStart(a);
    ataStart(b);
    MPI_Request r[2] = {a.req, b.req};
    int done = 0;
    while (done < 2) {
      int outcount = 0, idx[2];
      MPI_Waitsome(2, r, &outcount, idx, MPI_STATUSES_IGNORE);
      if (outcount < 1 || outcount > 2) fail(st, "Waitsome outcount", outcount, 1);
      done += outcount;
    }
    a.req = r[0];
    b.req = r[1];
    ataCheck(st, a);
    ataCheck(st, b);
  }
}

// 7. Two MPI_Iscatters from one root outstanding
static void stageScatter()
{
  int rank, size;
  getRS(rank, size);
  const char *st = "iscatter";
  const int count = 3;
  int *sbuf[2] = {nullptr, nullptr};
  int rbuf[2][count];
  MPI_Request req[2];
  for (int j = 0; j < 2; j++) {
    if (rank == 0) {
      sbuf[j] = (int *)malloc(sizeof(int) * count * size);
      for (int d = 0; d < size; d++)
        for (int k = 0; k < count; k++) sbuf[j][d * count + k] = val(20 + j, 0, d, k);
    }
    for (int k = 0; k < count; k++) rbuf[j][k] = -1;
    MPI_Iscatter(sbuf[j], count, MPI_INT, rbuf[j], count, MPI_INT, 0, MPI_COMM_WORLD, &req[j]);
  }
  MPI_Waitall(1, &req[1], MPI_STATUSES_IGNORE);
  MPI_Waitall(1, &req[0], MPI_STATUSES_IGNORE);
  for (int j = 0; j < 2; j++) {
    for (int k = 0; k < count; k++)
      if (rbuf[j][k] != val(20 + j, 0, rank, k)) fail(st, "Iscatter element", rbuf[j][k], val(20 + j, 0, rank, k));
    free(sbuf[j]);
  }
}

// 8. An MPI_Ialltoall outstanding across AMPI_Migrate, a second one posted after
static void stageMigrate()
{
  int rank, size;
  getRS(rank, size);
  const char *st = "migrate";
  Ata a, b;
  ataInit(a, 30, 5);
  ataInit(b, 31, 10240);
  ataStart(a);
  AMPI_Migrate(AMPI_INFO_LB_SYNC);
  ataStart(b);
  MPI_Request r[2] = {b.req, a.req};
  MPI_Waitall(2, r, MPI_STATUSES_IGNORE);
  b.req = r[0];
  a.req = r[1];
  ataCheck(st, a);
  ataCheck(st, b);
}

struct Stage { const char *name; void (*fn)(); };
static const Stage stages[] = {
  {"single", stageSingle},   {"mix", stageMix},         {"two", stageTwo},
  {"large", stageLarge},
  {"alltoallv", stageV},     {"testall", stageTestall}, {"anysome", stageAnySome},
  {"iscatter", stageScatter}, {"migrate", stageMigrate},
};

int main(int argc, char **argv)
{
  MPI_Init(&argc, &argv);
  int rank, size;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &size);

  const char *only = nullptr; // first positional argument: run only that stage
  for (int i = 1; i < argc; i++)
    if (argv[i][0] != '+' && argv[i][0] != '-') { only = argv[i]; break; }

  int ran = 0;
  for (const Stage &s : stages) {
    if (only && strcmp(only, s.name)) continue;
    if (rank == 0) { printf("waitall_ata: stage %s\n", s.name); fflush(stdout); }
    s.fn();
    MPI_Barrier(MPI_COMM_WORLD);
    ran++;
  }
  if (ran == 0) {
    if (rank == 0) printf("waitall_ata: unknown stage '%s'\n", only);
    MPI_Abort(MPI_COMM_WORLD, 1);
  }
  if (rank == 0) { printf("waitall_ata: all %d stage(s) passed on %d ranks\n", ran, size); fflush(stdout); }
  MPI_Finalize();
  return 0;
}

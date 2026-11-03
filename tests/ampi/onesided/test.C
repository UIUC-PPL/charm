// One-sided communication: target displacements in units of disp_unit, the
// window bounds check, MPI_PROC_NULL targets, and non-contiguous target
// datatypes (charm issue #4098, items 3 to 6).
//
// Each rank operates on the window of the NEXT rank, so with +p1 every
// target is local (the winLocal* paths and the ckLocal() fast paths) and
// with +p2 some targets are on the other PE (the winRemote* entry methods).
//
// Usage: onesided [all|units|procnull|noncontig]
//        onesided abort <put|get|acc|fop|getacc|cas>
// The abort mode makes rank 0 access one element past the end of rank 1's
// disp_unit 4 window. AMPI must abort with "outside the MPI_Win"; if the
// call returns, the test reports the failure through MPI_Abort.
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "mpi.h"

// Per-rank state lives on the stack: AMPI ranks on one PE share globals.
struct Ctx {
  int rank, size, target, source;
  int errors;
};

#define check(ok, sect, what, got, want) checkImpl(c, ok, sect, what, got, want)
static void checkImpl(Ctx& c, bool ok, const char* sect, const char* what, double got, double want)
{
  if (!ok) {
    printf("[%d] FAIL %s %s: got %.17g, expected %.17g\n", c.rank, sect, what, got, want);
    c.errors++;
  }
}

// Window of N elements of T created with disp_unit du (du divides sizeof(T));
// element i of rank r starts as r*100+i and sits at displacement
// i*sizeof(T)/du. Every RMA call names its target in those units.
template <typename T>
static void testUnits(Ctx& c, MPI_Datatype dt, int du, bool cas, const char* name)
{
  const int rank = c.rank, target = c.target, source = c.source;
  const int N = 16;
  const MPI_Aint per = sizeof(T) / du; // displacement units per element
  T wbuf[N];
  for (int i = 0; i < N; i++) wbuf[i] = (T)(rank * 100 + i);
  MPI_Win win;
  MPI_Win_create(wbuf, sizeof(wbuf), du, MPI_INFO_NULL, MPI_COMM_WORLD, &win);

  T put[2] = {(T)(1000 + rank), (T)(1001 + rank)};
  T get[2] = {(T)-1, (T)-1};
  T last = (T)-1;
  T acc = (T)(rank + 1);
  T fopin = (T)7, fopres = (T)-1;
  T gain[2] = {(T)3, (T)3}, gares[2] = {(T)-1, (T)-1};
  T casorig = (T)(2000 + rank), cascomp = (T)(target * 100 + 12), casres = (T)-1;

  MPI_Win_fence(0, win);
  MPI_Put(put, 2, dt, target, 2 * per, 2, dt, win);
  MPI_Get(get, 2, dt, target, 5 * per, 2, dt, win);
  MPI_Get(&last, 1, dt, target, (N - 1) * per, 1, dt, win); // ends exactly at the window end
  MPI_Accumulate(&acc, 1, dt, target, 8 * per, 1, dt, MPI_SUM, win);
  MPI_Fetch_and_op(&fopin, &fopres, dt, target, 9 * per, MPI_SUM, win);
  MPI_Get_accumulate(gain, 2, dt, gares, 2, dt, target, 10 * per, 2, dt, MPI_SUM, win);
  if (cas) MPI_Compare_and_swap(&casorig, &cascomp, &casres, dt, target, 12 * per, win);
  MPI_Win_fence(0, win);

  check(get[0] == (T)(target * 100 + 5), name, "get[0]", get[0], target * 100 + 5);
  check(get[1] == (T)(target * 100 + 6), name, "get[1]", get[1], target * 100 + 6);
  check(last == (T)(target * 100 + N - 1), name, "get last", last, target * 100 + N - 1);
  check(fopres == (T)(target * 100 + 9), name, "fetch_and_op result", fopres, target * 100 + 9);
  check(gares[0] == (T)(target * 100 + 10), name, "get_accumulate result[0]", gares[0], target * 100 + 10);
  check(gares[1] == (T)(target * 100 + 11), name, "get_accumulate result[1]", gares[1], target * 100 + 11);
  if (cas) check(casres == (T)(target * 100 + 12), name, "cas result", casres, target * 100 + 12);

  for (int i = 0; i < N; i++) {
    T want = (T)(rank * 100 + i);
    if (i == 2) want = (T)(1000 + source);
    if (i == 3) want = (T)(1001 + source);
    if (i == 8) want = (T)(rank * 100 + 8 + source + 1);
    if (i == 9) want = (T)(rank * 100 + 9 + 7);
    if (i == 10 || i == 11) want = (T)(rank * 100 + i + 3);
    if (i == 12 && cas) want = (T)(2000 + source);
    char what[32];
    snprintf(what, sizeof(what), "window[%d]", i);
    check(wbuf[i] == want, name, what, wbuf[i], want);
  }
  MPI_Win_free(&win);
}

static void testAllUnits(Ctx& c)
{
  testUnits<int>(c, MPI_INT, 1, true, "int disp_unit 1");
  testUnits<int>(c, MPI_INT, 4, true, "int disp_unit 4");
  testUnits<double>(c, MPI_DOUBLE, 8, false, "double disp_unit 8");
  // disp_unit 1 on an 8-byte type: the remote Fetch_and_op reply must be
  // sized by the type (8 bytes), not by disp_unit*count (1 byte).
  testUnits<double>(c, MPI_DOUBLE, 1, false, "double disp_unit 1");
}

// Every RMA call with target MPI_PROC_NULL succeeds and changes nothing.
static void testProcNull(Ctx& c)
{
  const int rank = c.rank;
  const char* n = "proc_null";
  int wbuf[4] = {rank, rank, rank, rank};
  MPI_Win win;
  MPI_Win_create(wbuf, sizeof(wbuf), sizeof(int), MPI_INFO_NULL, MPI_COMM_WORLD, &win);

  int org[2] = {11, 12}, res[2] = {-5, -5}, comp = 0;
  int rc[12];
  MPI_Request req[4];
  MPI_Win_fence(0, win);
  rc[0] = MPI_Put(org, 2, MPI_INT, MPI_PROC_NULL, 0, 2, MPI_INT, win);
  rc[1] = MPI_Get(res, 2, MPI_INT, MPI_PROC_NULL, 0, 2, MPI_INT, win);
  rc[2] = MPI_Accumulate(org, 2, MPI_INT, MPI_PROC_NULL, 0, 2, MPI_INT, MPI_SUM, win);
  rc[3] = MPI_Get_accumulate(org, 2, MPI_INT, res, 2, MPI_INT, MPI_PROC_NULL, 0, 2, MPI_INT,
                             MPI_SUM, win);
  rc[4] = MPI_Fetch_and_op(org, res, MPI_INT, MPI_PROC_NULL, 0, MPI_SUM, win);
  rc[5] = MPI_Compare_and_swap(org, &comp, res, MPI_INT, MPI_PROC_NULL, 0, win);
  rc[6] = MPI_Rput(org, 2, MPI_INT, MPI_PROC_NULL, 0, 2, MPI_INT, win, &req[0]);
  rc[7] = MPI_Rget(res, 2, MPI_INT, MPI_PROC_NULL, 0, 2, MPI_INT, win, &req[1]);
  rc[8] = MPI_Raccumulate(org, 2, MPI_INT, MPI_PROC_NULL, 0, 2, MPI_INT, MPI_SUM, win, &req[2]);
  rc[9] = MPI_Rget_accumulate(org, 2, MPI_INT, res, 2, MPI_INT, MPI_PROC_NULL, 0, 2, MPI_INT,
                              MPI_SUM, win, &req[3]);
  rc[10] = MPI_Waitall(4, req, MPI_STATUSES_IGNORE);
  MPI_Win_fence(0, win);
  rc[11] = MPI_SUCCESS;

  for (int i = 0; i < 11; i++) {
    char what[32];
    snprintf(what, sizeof(what), "return code of call %d", i);
    check(rc[i] == MPI_SUCCESS, n, what, rc[i], MPI_SUCCESS);
  }
  check(res[0] == -5 && res[1] == -5, n, "result/origin buffer unchanged", res[0], -5);
  check(org[0] == 11 && org[1] == 12, n, "origin buffer unchanged", org[0], 11);
  for (int i = 0; i < 4; i++)
    check(wbuf[i] == rank, n, "own window unchanged", wbuf[i], rank);
  MPI_Win_free(&win);
}

// MPI_DOUBLE_INT is a predefined non-contiguous type: 12 bytes of data in a
// 16-byte extent (a double, an int, 4 bytes of padding). AMPI rejects derived
// target types (MPI_Put etc. abort on them), so this is the non-contiguous
// target type a program can use. The contiguous origin is the packed form,
// 12 bytes per element, given as MPI_BYTE.
struct DI { double d; int i; };

static void testNoncontig(Ctx& c, int du)
{
  const int rank = c.rank, target = c.target, source = c.source;
  const int N = 8;
  char name[64];
  snprintf(name, sizeof(name), "double_int disp_unit %d", du);
  const MPI_Aint per = sizeof(DI) / du;
  DI wbuf[N];
  memset(wbuf, 0xAB, sizeof(wbuf)); // padding bytes stay 0xAB unless overwritten
  for (int k = 0; k < N; k++) { wbuf[k].d = rank * 100 + k; wbuf[k].i = rank * 10 + k; }
  MPI_Win win;
  MPI_Win_create(wbuf, sizeof(wbuf), du, MPI_INFO_NULL, MPI_COMM_WORLD, &win);

  // Get elements 1..3 of the target into a packed buffer.
  char packed[3 * 12];
  memset(packed, 0, sizeof(packed));
  // Put a packed buffer into elements 5..6 of the target.
  char pin[2 * 12];
  for (int k = 0; k < 2; k++) {
    double d = 1000 + rank + k; int i = 500 + rank + k;
    memcpy(pin + 12 * k, &d, 8); memcpy(pin + 12 * k + 8, &i, 4);
  }
  // Get_accumulate MPI_MAXLOC on elements 0..1 with a packed (contiguous)
  // origin and a non-contiguous result and target: the result receives the
  // old values in the MPI_DOUBLE_INT layout; the origin values are smaller
  // than every window value, so the target keeps its values.
  char gaorg[2 * 12];
  DI gares[2];
  for (int k = 0; k < 2; k++) {
    double d = -1.0; int i = -1;
    memcpy(gaorg + 12 * k, &d, 8); memcpy(gaorg + 12 * k + 8, &i, 4);
    gares[k].d = -2.0; gares[k].i = -2;
  }
  // Accumulate MPI_MAXLOC on element 7 with a larger value: the target takes it.
  DI accorg[1];
  accorg[0].d = 9999.0; accorg[0].i = 900 + rank;

  MPI_Win_fence(0, win);
  MPI_Get(packed, 3 * 12, MPI_BYTE, target, 1 * per, 3, MPI_DOUBLE_INT, win);
  MPI_Put(pin, 2 * 12, MPI_BYTE, target, 5 * per, 2, MPI_DOUBLE_INT, win);
  MPI_Get_accumulate(gaorg, 2 * 12, MPI_BYTE, gares, 2, MPI_DOUBLE_INT, target, 0, 2,
                     MPI_DOUBLE_INT, MPI_MAXLOC, win);
  MPI_Accumulate(accorg, 1, MPI_DOUBLE_INT, target, 7 * per, 1, MPI_DOUBLE_INT, MPI_MAXLOC, win);
  MPI_Win_fence(0, win);

  for (int k = 0; k < 3; k++) {
    double d; int i;
    memcpy(&d, packed + 12 * k, 8); memcpy(&i, packed + 12 * k + 8, 4);
    check(d == target * 100 + 1 + k, name, "get noncontig target: double", d, target * 100 + 1 + k);
    check(i == target * 10 + 1 + k, name, "get noncontig target: int", i, target * 10 + 1 + k);
  }
  for (int k = 0; k < 2; k++) {
    check(gares[k].d == target * 100 + k, name, "get_accumulate result .d", gares[k].d, target * 100 + k);
    check(gares[k].i == target * 10 + k, name, "get_accumulate result .i", gares[k].i, target * 10 + k);
  }

  for (int k = 0; k < N; k++) {
    double wd = rank * 100 + k; int wi = rank * 10 + k;
    if (k == 5 || k == 6) { wd = 1000 + source + (k - 5); wi = 500 + source + (k - 5); }
    if (k == 7) { wd = 9999.0; wi = 900 + source; }
    char what[48];
    snprintf(what, sizeof(what), "window[%d].d", k);
    check(wbuf[k].d == wd, name, what, wbuf[k].d, wd);
    snprintf(what, sizeof(what), "window[%d].i", k);
    check(wbuf[k].i == wi, name, what, wbuf[k].i, wi);
    // Element 7 is skipped: the MPI_MAXLOC reduction function assigns whole
    // DoubleInt structs, padding included.
    if (k == 7) continue;
    unsigned char* pad = (unsigned char*)&wbuf[k] + 12;
    snprintf(what, sizeof(what), "window[%d] padding", k);
    check(pad[0] == 0xAB && pad[1] == 0xAB && pad[2] == 0xAB && pad[3] == 0xAB, name, what,
          pad[0], 0xAB);
  }
  MPI_Win_free(&win);
}

// Rank 0 accesses element 16 of rank 1's 16-int window (disp_unit 4). The
// buffer has 16 more ints after the window so that an unchecked access stays
// inside this stack frame.
static void testAbort(Ctx& c, const char* op)
{
  const int N = 16;
  int buf[2 * N];
  for (int i = 0; i < 2 * N; i++) buf[i] = c.rank;
  MPI_Win win;
  MPI_Win_create(buf, N * sizeof(int), sizeof(int), MPI_INFO_NULL, MPI_COMM_WORLD, &win);
  MPI_Win_fence(0, win);
  if (c.rank == 0) {
    int org = 5, res = -1, comp = 1;
    if (!strcmp(op, "put")) MPI_Put(&org, 1, MPI_INT, 1, N, 1, MPI_INT, win);
    else if (!strcmp(op, "get")) MPI_Get(&res, 1, MPI_INT, 1, N, 1, MPI_INT, win);
    else if (!strcmp(op, "acc")) MPI_Accumulate(&org, 1, MPI_INT, 1, N, 1, MPI_INT, MPI_SUM, win);
    else if (!strcmp(op, "fop")) MPI_Fetch_and_op(&org, &res, MPI_INT, 1, N, MPI_SUM, win);
    else if (!strcmp(op, "getacc"))
      MPI_Get_accumulate(&org, 1, MPI_INT, &res, 1, MPI_INT, 1, N, 1, MPI_INT, MPI_SUM, win);
    else if (!strcmp(op, "cas")) MPI_Compare_and_swap(&org, &comp, &res, MPI_INT, 1, N, win);
    else { printf("unknown abort op %s\n", op); MPI_Abort(MPI_COMM_WORLD, 1); }
  }
  MPI_Win_fence(0, win);
  if (c.rank == 0) {
    printf("FAIL abort %s: access outside the window was not rejected\n", op);
    MPI_Abort(MPI_COMM_WORLD, 1);
  }
  MPI_Barrier(MPI_COMM_WORLD);
  MPI_Win_free(&win);
}

int main(int argc, char** argv)
{
  MPI_Init(&argc, &argv);
  Ctx c;
  c.errors = 0;
  MPI_Comm_rank(MPI_COMM_WORLD, &c.rank);
  MPI_Comm_size(MPI_COMM_WORLD, &c.size);
  c.target = (c.rank + 1) % c.size;
  c.source = (c.rank + c.size - 1) % c.size;
  const int rank = c.rank, size = c.size;

  // Positional arguments: argv entries that do not start with '+' or '-'.
  const char* pos[2] = {"all", ""};
  int npos = 0;
  for (int i = 1; i < argc && npos < 2; i++)
    if (argv[i][0] != '+' && argv[i][0] != '-') pos[npos++] = argv[i];
  const char* mode = pos[0];

  if (!strcmp(mode, "abort")) {
    if (size < 2) {
      if (rank == 0) printf("abort mode needs at least 2 ranks\n");
      MPI_Abort(MPI_COMM_WORLD, 1);
    }
    testAbort(c, pos[1]);
  }
  bool all = !strcmp(mode, "all");
  if (all || !strcmp(mode, "units")) testAllUnits(c);
  if (all || !strcmp(mode, "procnull")) testProcNull(c);
  if (all || !strcmp(mode, "noncontig")) {
    testNoncontig(c, sizeof(DI));
    testNoncontig(c, 1);
  }

  int total = 0;
  MPI_Allreduce(&c.errors, &total, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
  if (rank == 0) {
    if (total == 0) printf("onesided test (%s) passed on %d ranks\n", mode, size);
    else printf("onesided test (%s) FAILED: %d errors\n", mode, total);
  }
  // AMPI's MPI_Finalize never returns (it ends in AMPI_Exit(0)), so a
  // failure must be reported through the exit status of MPI_Abort.
  if (total != 0) MPI_Abort(MPI_COMM_WORLD, 1);
  MPI_Finalize();
  return 0;
}

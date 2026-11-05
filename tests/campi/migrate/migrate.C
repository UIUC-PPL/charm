/*
 * campi test: restartable ranks. Each step of the loop has a Sendrecv ring
 * and five migrate sites: three in main (the third the unbraced body of an
 * if, taken on odd steps), one in phase() (a resumable function awaited with
 * CAMPI_CALL, with a Send/Recv ring before its site) and one in tally()
 * (awaited with CAMPI_CALL_TO, whose info asks for nothing). The
 * loop counter and every value carried across a site are in the State, so
 * a rank that migrates (+balancer), is rebuilt in place (+campi_fakemigrate)
 * or restarts from a checkpoint (-chkpt DIR, written at the second site of
 * main at step CHKPT_STEP; later run with +restart DIR) ends with the same
 * per-rank checksum, which each rank compares with its closed form.
 *
 * Before the first site, main also creates a reduction op (MPI_Op_create)
 * and a communicator keyval with copy and delete functions, and sets the
 * keyval's attribute on MPI_COMM_WORLD. After a site in every step, it uses
 * the op in an MPI_Allreduce and duplicates and frees MPI_COMM_WORLD, which
 * calls the copy and delete functions; their results are checked. A rank
 * that moved to another process or restarted from a checkpoint must find
 * these functions at their addresses in its new process.
 *
 *   ./migrate [-chkpt DIR] +vp4 [+pe P] [+balancer RotateLB] [+campi_fakemigrate]
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include "mpi.h"
#include "campi.h"

#define NSTEPS 6
#define CHKPT_STEP 3
#define KV_EXTRA ((void *)(intptr_t)4242) // extra_state: a value, not an address

// in + inout + 1: combining n values gives their sum plus n - 1, so the
// result shows that the op's own function did the combining.
static void sum_plus_one(void *in, void *inout, int *len, MPI_Datatype *)
{
  for (int i = 0; i < *len; i++) ((int *)inout)[i] += ((int *)in)[i] + 1;
}

// The attribute is an integer; the copy is 2 * value + 1.
static int kv_copy(MPI_Comm, int, void *extra, void *in, void *out, int *flag)
{
  if (extra != KV_EXTRA) return MPI_ERR_OTHER;
  *(void **)out = (void *)(2 * (intptr_t)in + 1);
  *flag = 1;
  return MPI_SUCCESS;
}

// Deletions seen by the rank running on this PE thread. Ranks on a PE do not
// interleave between two MPI calls that do not block, so a rank reads its own
// count by taking the difference around MPI_Comm_free.
static thread_local int kv_deletes = 0;
static int kv_delete(MPI_Comm, int, void *, void *extra)
{
  if (extra != KV_EXTRA) return MPI_ERR_OTHER;
  kv_deletes++;
  return MPI_SUCCESS;
}

struct State {
  int rank = 0, size = 0, step = 0;
  long long acc = 0;       // the checksum
  int recvd = 0;           // main's ring value, used after the first site
  int phaseGot = 0;        // phase()'s ring value, used after its site
  int tallied = 0;         // tally()'s result
  int extra = 0;           // set before the conditional site, used after it
  MPI_Op op = MPI_OP_NULL; // sum_plus_one
  int keyval = MPI_KEYVAL_INVALID;
  int fnErrors = 0;        // wrong results of the op, copy or delete function
  std::string chkpt_dir;
  MPI_Info chkpt_info = MPI_INFO_NULL;
  MPI_Info noop_info = MPI_INFO_NULL;

  void pup(PUP::er &p) {
    p | rank; p | size; p | step; p | acc; p | recvd; p | phaseGot; p | tallied;
    p | extra; p | op; p | keyval; p | fnErrors;
    p | chkpt_dir; p | chkpt_info; p | noop_info;
  }
};

static campi::task<void> phase(State &s)
{
  int left, right, v;
  CAMPI_RESUMABLE();
  right = (s.rank + 1) % s.size;
  left = (s.rank + s.size - 1) % s.size;
  v = s.rank * 7 + s.step;
  MPI_Send(&v, 1, MPI_INT, right, 10, MPI_COMM_WORLD);
  MPI_Recv(&s.phaseGot, 1, MPI_INT, left, 10, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
  CAMPI_MIGRATE(AMPI_INFO_LB_SYNC);
  s.acc += 3LL * s.phaseGot;
  CAMPI_END();
  co_return;
}

static campi::task<int> tally(State &s)
{
  CAMPI_RESUMABLE();
  CAMPI_MIGRATE(s.noop_info);
  CAMPI_END();
  co_return 2 * s.step + 1;
}

static long long expected(int rank, int size)
{
  long long left = (rank + size - 1) % size, e = 0;
  for (long long t = 0; t < NSTEPS; t++)
    e += (left * 1000 + t) * (t + 1) + 3 * (left * 7 + t) + (2 * t + 1) + 5 * t;
  return e;
}

CAMPI_MAIN_STATE(State, argc, argv, s)
{
  int token, errors;
  long long sum;

  CAMPI_RESUMABLE();

  MPI_Init(&argc, &argv);
  MPI_Comm_rank(MPI_COMM_WORLD, &s.rank);
  MPI_Comm_size(MPI_COMM_WORLD, &s.size);
  for (int i = 1; i + 1 < argc; i++)
    if (strcmp(argv[i], "-chkpt") == 0) s.chkpt_dir = argv[i + 1];
  if (!s.chkpt_dir.empty()) {
    std::string v = "to_file=" + s.chkpt_dir;
    MPI_Info_create(&s.chkpt_info);
    MPI_Info_set(s.chkpt_info, "ampi_checkpoint", v.c_str());
  }
  MPI_Info_create(&s.noop_info);
  MPI_Info_set(s.noop_info, "ampi_load_balance", "false");
  MPI_Op_create(sum_plus_one, 1, &s.op);
  MPI_Comm_create_keyval(kv_copy, kv_delete, &s.keyval, KV_EXTRA);
  MPI_Comm_set_attr(MPI_COMM_WORLD, s.keyval, (void *)(intptr_t)(s.rank + 1));

  for (s.step = 0; s.step < NSTEPS; s.step++) {
    token = s.rank * 1000 + s.step;
    MPI_Sendrecv(&token, 1, MPI_INT, (s.rank + 1) % s.size, 0, &s.recvd, 1, MPI_INT,
                 (s.rank + s.size - 1) % s.size, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    CAMPI_MIGRATE(AMPI_INFO_LB_SYNC);
    s.acc += (long long)s.recvd * (s.step + 1);
    {
      int v = s.rank + s.step, got = 0, flag = 0, before;
      void *attr = nullptr;
      MPI_Comm dup;
      MPI_Allreduce(&v, &got, 1, MPI_INT, s.op, MPI_COMM_WORLD);
      if (got != s.size * (s.size - 1) / 2 + s.size * s.step + s.size - 1) s.fnErrors++;
      MPI_Comm_dup(MPI_COMM_WORLD, &dup);
      MPI_Comm_get_attr(dup, s.keyval, &attr, &flag);
      if (!flag || (intptr_t)attr != 2 * (s.rank + 1) + 1) s.fnErrors++;
      before = kv_deletes;
      MPI_Comm_free(&dup);
      if (kv_deletes != before + 1) s.fnErrors++;
    }
    CAMPI_CALL(phase(s));
    if (s.rank == 0 && s.step == CHKPT_STEP && !s.chkpt_dir.empty())
      printf("migrate: step %d: checkpoint to %s\n", s.step, s.chkpt_dir.c_str());
    CAMPI_MIGRATE(s.step == CHKPT_STEP && !s.chkpt_dir.empty() ? s.chkpt_info
                                                                : AMPI_INFO_LB_SYNC);
    CAMPI_CALL_TO(s.tallied, tally(s));
    s.acc += s.tallied;
    s.extra = 5 * s.step;
    if (s.step % 2 == 1) CAMPI_MIGRATE(AMPI_INFO_LB_SYNC);
    s.acc += s.extra;
  }

  printf("migrate: rank %d checksum %lld (expected %lld)\n", s.rank, s.acc,
         expected(s.rank, s.size));
  if (s.fnErrors)
    printf("migrate: rank %d: %d wrong results of the user op or keyval functions\n",
           s.rank, s.fnErrors);
  errors = (s.acc != expected(s.rank, s.size)) + s.fnErrors;
  MPI_Allreduce(MPI_IN_PLACE, &errors, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
  sum = s.acc;
  MPI_Allreduce(MPI_IN_PLACE, &sum, 1, MPI_LONG_LONG, MPI_SUM, MPI_COMM_WORLD);
  if (s.rank == 0) {
    printf("migrate: %d ranks, %d steps, checksum sum %lld, errors=%d\n", s.size, NSTEPS,
           sum, errors);
    if (errors == 0) printf("All tests passed\n");
  }
  if (s.chkpt_info != MPI_INFO_NULL) MPI_Info_free(&s.chkpt_info);
  MPI_Info_free(&s.noop_info);
  MPI_Op_free(&s.op);
  MPI_Comm_delete_attr(MPI_COMM_WORLD, s.keyval);
  MPI_Comm_free_keyval(&s.keyval);
  MPI_Finalize();
  CAMPI_END();
  co_return errors == 0 ? 0 : 1;
}

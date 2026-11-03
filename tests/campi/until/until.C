// campi test: communicator creation (split, dup), Sendrecv, Gather/Scatter,
// Waitall with several outstanding Irecvs, other collectives (Scan and
// Exscan also over non-contiguous datatypes), MPI_Test and MPI_Iprobe
// polling loops, and nested
// campi::task helpers that block (resume of the innermost frame).
#include <algorithm>
#include <cstdio>
#include <vector>
#include "mpi.h"
#include "campi.h"

// Nested helper: a blocking receive one level down.
static campi::task<int> recv_one(int src, int tag, MPI_Comm comm) {
  int v = -1;
  MPI_Recv(&v, 1, MPI_INT, src, tag, comm, MPI_STATUS_IGNORE);
  co_return v;
}

// Two levels down, with blocking calls before and after the inner await.
static campi::task<int> ring_sum(int rank, int size, MPI_Comm comm) {
  int right = (rank + 1) % size, left = (rank + size - 1) % size;
  MPI_Request r;
  MPI_Isend(&rank, 1, MPI_INT, right, 77, comm, &r);
  int got = co_await recv_one(left, 77, comm);
  MPI_Wait(&r, MPI_STATUS_IGNORE);
  int sum = 0;
  MPI_Allreduce(&got, &sum, 1, MPI_INT, MPI_SUM, comm);
  co_return sum;
}

// User sum on MPI_Type_vector(VEC_N, 1, VEC_STRIDE, MPI_INT): add the ints
// of each element, leaving the gaps.
static const int VEC_N = 4, VEC_STRIDE = 2, VEC_EXTENT = (VEC_N - 1) * VEC_STRIDE + 1;
static void vec_sum(void *invec, void *inoutvec, int *len, MPI_Datatype *)
{
  const int *in = (const int *)invec;
  int *io = (int *)inoutvec;
  for (int e = 0; e < *len; e++)
    for (int j = 0; j < VEC_N; j++) io[e * VEC_EXTENT + j * VEC_STRIDE] += in[e * VEC_EXTENT + j * VEC_STRIDE];
}

CAMPI_MAIN(argc, argv)
{
  MPI_Init(&argc, &argv);
  int rank, size;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &size);
  long myerr = 0;
#define C(c)                                                                  \
  do {                                                                        \
    if (!(c)) { myerr++; printf("rank %d: check failed at line %d: %s\n",     \
                                rank, __LINE__, #c); }                        \
  } while (0)

  // 1. Sendrecv around a ring
  {
    int right = (rank + 1) % size, left = (rank + size - 1) % size, in = -1;
    MPI_Status st;
    MPI_Sendrecv(&rank, 1, MPI_INT, right, 1, &in, 1, MPI_INT, left, 1, MPI_COMM_WORLD, &st);
    C(in == left && st.MPI_SOURCE == left && st.MPI_TAG == 1);
  }

  // 2. Waitall with several outstanding Irecvs (posted before the sends)
  {
    const int K = 4;
    std::vector<int> in(size * K, -1), out(K);
    std::vector<MPI_Request> reqs;
    for (int src = 0; src < size; src++)
      for (int k = 0; k < K; k++) {
        reqs.emplace_back();
        MPI_Irecv(&in[src * K + k], 1, MPI_INT, src, 100 + k, MPI_COMM_WORLD, &reqs.back());
      }
    MPI_Barrier(MPI_COMM_WORLD);
    for (int dst = 0; dst < size; dst++)
      for (int k = 0; k < K; k++) {
        out[k] = rank * 1000 + k;
        MPI_Send(&out[k], 1, MPI_INT, dst, 100 + k, MPI_COMM_WORLD);
      }
    MPI_Waitall((int)reqs.size(), reqs.data(), MPI_STATUSES_IGNORE);
    for (int src = 0; src < size; src++)
      for (int k = 0; k < K; k++) C(in[src * K + k] == src * 1000 + k);
  }

  // 3. Gather / Scatter / Bcast / Reduce / Allgather / Alltoall / Scan
  {
    std::vector<int> all(size, -1);
    MPI_Gather(&rank, 1, MPI_INT, all.data(), 1, MPI_INT, 0, MPI_COMM_WORLD);
    if (rank == 0) for (int i = 0; i < size; i++) C(all[i] == i);
    std::vector<int> sc(size);
    for (int i = 0; i < size; i++) sc[i] = 10 * i + 3;
    int mine = -1;
    MPI_Scatter(sc.data(), 1, MPI_INT, &mine, 1, MPI_INT, 0, MPI_COMM_WORLD);
    C(mine == 10 * rank + 3);
    int b = (rank == 0) ? 4242 : 0;
    MPI_Bcast(&b, 1, MPI_INT, 0, MPI_COMM_WORLD);
    C(b == 4242);
    int red = 0;
    MPI_Reduce(&rank, &red, 1, MPI_INT, MPI_SUM, 0, MPI_COMM_WORLD);
    if (rank == 0) C(red == size * (size - 1) / 2);
    std::fill(all.begin(), all.end(), -1);
    MPI_Allgather(&rank, 1, MPI_INT, all.data(), 1, MPI_INT, MPI_COMM_WORLD);
    for (int i = 0; i < size; i++) C(all[i] == i);
    std::vector<int> a2aout(size), a2ain(size, -1);
    for (int i = 0; i < size; i++) a2aout[i] = rank * 100 + i;
    MPI_Alltoall(a2aout.data(), 1, MPI_INT, a2ain.data(), 1, MPI_INT, MPI_COMM_WORLD);
    for (int i = 0; i < size; i++) C(a2ain[i] == i * 100 + rank);
    int sc_in = 1, sc_out = 0;
    MPI_Scan(&sc_in, &sc_out, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    C(sc_out == rank + 1);
    int ex_out = -1;
    MPI_Exscan(&sc_in, &ex_out, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    if (rank > 0) C(ex_out == rank);
    // Gatherv: rank i contributes i+1 copies of i
    std::vector<int> gv_in(rank + 1, rank), counts(size), displs(size);
    int tot = 0;
    for (int i = 0; i < size; i++) { counts[i] = i + 1; displs[i] = tot; tot += i + 1; }
    std::vector<int> gv_out(tot, -1);
    MPI_Gatherv(gv_in.data(), rank + 1, MPI_INT, gv_out.data(), counts.data(), displs.data(),
                MPI_INT, 0, MPI_COMM_WORLD);
    if (rank == 0)
      for (int i = 0; i < size; i++)
        for (int j = 0; j < i + 1; j++) C(gv_out[displs[i] + j] == i);
  }

  // 3b. Scan and Exscan over non-contiguous datatypes: a strided vector of
  // ints with a user sum (the gaps between its ints must stay untouched) and
  // MPI_DOUBLE_INT with MPI_MAXLOC
  {
    const int N = VEC_N, STRIDE = VEC_STRIDE, LEN = VEC_EXTENT, GAP = -7;
    MPI_Datatype vec;
    MPI_Type_vector(N, 1, STRIDE, MPI_INT, &vec);
    MPI_Type_commit(&vec);
    std::vector<int> vin(LEN, GAP), vsc(LEN, GAP), vex(LEN, GAP);
    for (int i = 0; i < N; i++) vin[i * STRIDE] = rank * 10 + i + 1;
    MPI_Op vop; // AMPI's predefined ops take only basic types: a user sum
    MPI_Op_create(vec_sum, 1, &vop);
    MPI_Scan(vin.data(), vsc.data(), 1, vec, vop, MPI_COMM_WORLD);
    MPI_Exscan(vin.data(), vex.data(), 1, vec, vop, MPI_COMM_WORLD);
    MPI_Op_free(&vop);
    for (int j = 0; j < LEN; j++) {
      if (j % STRIDE != 0) {
        C(vsc[j] == GAP);
        C(vex[j] == GAP);
        continue;
      }
      int i = j / STRIDE, incl = 0;
      for (int r = 0; r <= rank; r++) incl += r * 10 + i + 1;
      C(vsc[j] == incl);
      if (rank > 0) C(vex[j] == incl - (rank * 10 + i + 1));
    }
    MPI_Type_free(&vec);

    struct DI { double v; int i; };
    auto val = [](int r, int k) { return k == 0 ? (double)((r * 3) % 5) : -(double)r; };
    DI din[2], dsc[2], dex[2];
    for (int k = 0; k < 2; k++) {
      din[k].v = val(rank, k);
      din[k].i = rank;
      dsc[k].v = dex[k].v = -1.0;
      dsc[k].i = dex[k].i = -1;
    }
    MPI_Scan(din, dsc, 2, MPI_DOUBLE_INT, MPI_MAXLOC, MPI_COMM_WORLD);
    MPI_Exscan(din, dex, 2, MPI_DOUBLE_INT, MPI_MAXLOC, MPI_COMM_WORLD);
    for (int k = 0; k < 2; k++) {
      // expected MAXLOC over ranks [0, upto]: largest value, lowest rank on ties
      auto expect = [&](int upto) {
        DI e{val(0, k), 0};
        for (int r = 1; r <= upto; r++)
          if (val(r, k) > e.v) e = DI{val(r, k), r};
        return e;
      };
      DI e = expect(rank);
      C(dsc[k].v == e.v && dsc[k].i == e.i);
      if (rank > 0) {
        DI x = expect(rank - 1);
        C(dex[k].v == x.v && dex[k].i == x.i);
      }
    }
  }

  // 4. Comm_split into even/odd ranks, ordered by descending world rank
  MPI_Comm half;
  {
    MPI_Comm_split(MPI_COMM_WORLD, rank % 2, -rank, &half);
    int hr, hs;
    MPI_Comm_rank(half, &hr);
    MPI_Comm_size(half, &hs);
    int expect_size = (size / 2) + ((size % 2) && (rank % 2 == 0) ? 1 : 0);
    C(hs == expect_size);
    std::vector<int> members(hs, -1);
    MPI_Allgather(&rank, 1, MPI_INT, members.data(), 1, MPI_INT, half);
    for (int i = 0; i + 1 < hs; i++) C(members[i] > members[i + 1]);
    C(members[hr] == rank);
  }

  // 5. Comm_dup and Comm_dup_with_info of the split communicator; traffic on
  // them stays separate
  {
    MPI_Comm dup;
    MPI_Comm_dup(half, &dup);
    int hr, hs, dr, ds;
    MPI_Comm_rank(half, &hr); MPI_Comm_size(half, &hs);
    MPI_Comm_rank(dup, &dr);  MPI_Comm_size(dup, &ds);
    C(hr == dr && hs == ds && dup != half);
    int s1 = 0, s2 = 0;
    MPI_Allreduce(&rank, &s1, 1, MPI_INT, MPI_SUM, half);
    MPI_Allreduce(&rank, &s2, 1, MPI_INT, MPI_SUM, dup);
    C(s1 == s2);
    MPI_Comm_free(&dup);
    MPI_Comm dupi;
    MPI_Comm_dup_with_info(half, MPI_INFO_NULL, &dupi);
    MPI_Comm_rank(dupi, &dr);  MPI_Comm_size(dupi, &ds);
    C(hr == dr && hs == ds && dupi != half);
    int s3 = 0;
    MPI_Allreduce(&rank, &s3, 1, MPI_INT, MPI_SUM, dupi);
    C(s1 == s3);
    MPI_Comm_free(&dupi);
  }

  // 6. Nested tasks with blocking calls, on world and on the split comm
  {
    int s = co_await ring_sum(rank, size, MPI_COMM_WORLD);
    C(s == size * (size - 1) / 2);
    int hr, hs;
    MPI_Comm_rank(half, &hr); MPI_Comm_size(half, &hs);
    int s2 = co_await ring_sum(hr, hs, half);
    C(s2 == hs * (hs - 1) / 2);
  }
  MPI_Comm_free(&half);

  // 7. Polling loops: MPI_Test on an Irecv posted before its send exists, and
  // MPI_Iprobe on another pair (a miss must let the sender's message arrive)
  if (size >= 2) {
    const int trecv = 0, tsend = 1;
    const int precv = size >= 4 ? 2 : 0, psend = size >= 4 ? 3 : 1;
    int go = 1;
    if (rank == trecv) {
      int in[3] = {-1, -1, -1}, flag = 0;
      long polls = 0;
      MPI_Request req;
      MPI_Status st;
      MPI_Irecv(in, 3, MPI_INT, tsend, 300, MPI_COMM_WORLD, &req);
      MPI_Send(&go, 1, MPI_INT, tsend, 299, MPI_COMM_WORLD); // only now may tsend send
      while (!flag) { MPI_Test(&req, &flag, &st); polls++; }
      int cnt = -1;
      MPI_Get_count(&st, MPI_INT, &cnt);
      C(in[0] == 7 && in[1] == 8 && in[2] == 9);
      C(st.MPI_SOURCE == tsend && st.MPI_TAG == 300 && cnt == 3);
      C(req == MPI_REQUEST_NULL && polls >= 1);
    }
    if (rank == tsend) {
      int out[3] = {7, 8, 9};
      MPI_Recv(&go, 1, MPI_INT, trecv, 299, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
      MPI_Send(out, 3, MPI_INT, trecv, 300, MPI_COMM_WORLD);
    }
    if (rank == precv) {
      int flag = 0, cnt = -1;
      MPI_Status st;
      MPI_Send(&go, 1, MPI_INT, psend, 298, MPI_COMM_WORLD);
      while (!flag) MPI_Iprobe(MPI_ANY_SOURCE, 301, MPI_COMM_WORLD, &flag, &st);
      MPI_Get_count(&st, MPI_INT, &cnt);
      C(st.MPI_SOURCE == psend && st.MPI_TAG == 301 && cnt == 2);
      int in[2] = {-1, -1};
      MPI_Recv(in, 2, MPI_INT, st.MPI_SOURCE, st.MPI_TAG, MPI_COMM_WORLD, &st);
      C(in[0] == 41 && in[1] == 42);
    }
    if (rank == psend) {
      int out[2] = {41, 42};
      MPI_Recv(&go, 1, MPI_INT, precv, 298, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
      MPI_Send(out, 2, MPI_INT, precv, 301, MPI_COMM_WORLD);
    }
  }

  long total = 0;
  MPI_Allreduce(&myerr, &total, 1, MPI_LONG, MPI_SUM, MPI_COMM_WORLD);
  if (rank == 0)
    printf("campi until: %d ranks, errors=%ld: %s\n", size, total, total == 0 ? "PASSED" : "FAILED");
  MPI_Finalize();
  co_return total == 0 ? 0 : 1;
}

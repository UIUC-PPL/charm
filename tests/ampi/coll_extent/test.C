/*
 * Send-side block offsets of the collectives that send one block per peer,
 * with a non-contiguous send type (charm PR #4105, Aditya's review).
 *
 * MPI places block j of the send buffer at j*sendcount*extent(sendtype) (and
 * element displs[j] at displs[j]*extent for the v forms). A type's size is
 * less than its extent when the type has gaps, so an offset computed from the
 * size sends the wrong data for such a type. The receive sides already used
 * the extent. Covered here, each with a resized vector type whose extent is
 * twice its size: MPI_Scatter, MPI_Iscatter, MPI_Scatterv and MPI_Iscatterv
 * on an intercommunicator (the intracommunicator versions were right), and
 * MPI_Neighbor_alltoall, MPI_Ineighbor_alltoall, MPI_Neighbor_alltoallv and
 * MPI_Ineighbor_alltoallv on a periodic one-dimensional Cartesian
 * communicator (in whichever neighbor order the implementation uses, see
 * charm issue #4101). MPI_Alltoall and MPI_Ialltoall on MPI_COMM_WORLD are
 * run too.
 *
 * Needs at least 2 ranks (the intercommunicator has one group per parity).
 * Every received value is checked; a mismatch calls MPI_Abort.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mpi.h"

static void getRS(MPI_Comm comm, int &rank, int &size)
{
  MPI_Comm_rank(comm, &rank);
  MPI_Comm_size(comm, &size);
}

static void fail(const char *what, int block, int k, long got, long want)
{
  int rank, size;
  getRS(MPI_COMM_WORLD, rank, size);
  printf("[%d] ERROR in %s: block %d element %d: got %ld, expected %ld\n", rank, what, block, k, got, want);
  fflush(stdout);
  MPI_Abort(MPI_COMM_WORLD, 1);
}

// Value k of the block that src sends to dst in collective id
static int val(int id, int src, int dst, int k) { return id * 100003 + src * 1009 + dst * 31 + k; }

// A block of COUNT ints, STRIDE apart, sent as one element of a vector type
// resized so that consecutive blocks follow each other.
#define COUNT 5
#define STRIDE 2
#define BLOCK (COUNT * STRIDE)

static MPI_Datatype blockType()
{
  MPI_Datatype vec, t;
  MPI_Type_vector(COUNT, 1, STRIDE, MPI_INT, &vec);
  MPI_Type_create_resized(vec, 0, (MPI_Aint)BLOCK * sizeof(int), &t);
  MPI_Type_commit(&t);
  MPI_Type_free(&vec);
  return t;
}

static int *blocks(int n) // n blocks, every int -1
{
  int *b = (int *)malloc(sizeof(int) * BLOCK * n);
  for (int i = 0; i < BLOCK * n; i++) b[i] = -1;
  return b;
}

static void fillBlock(int *buf, int j, int id, int src, int dst)
{
  for (int k = 0; k < COUNT; k++) buf[j * BLOCK + k * STRIDE] = val(id, src, dst, k);
}

static void checkBlock(const char *what, const int *buf, int j, int id, int src, int dst)
{
  for (int k = 0; k < COUNT; k++) {
    int got = buf[j * BLOCK + k * STRIDE], want = val(id, src, dst, k);
    if (got != want) fail(what, j, k, got, want);
  }
}

// MPI_COMM_WORLD alltoall, blocking and nonblocking
static void testAlltoall(MPI_Datatype t)
{
  int rank, size;
  getRS(MPI_COMM_WORLD, rank, size);
  for (int nb = 0; nb < 2; nb++) {
    const int id = 1 + nb;
    int *sbuf = blocks(size), *rbuf = blocks(size);
    for (int d = 0; d < size; d++) fillBlock(sbuf, d, id, rank, d);
    if (nb == 0) {
      MPI_Alltoall(sbuf, 1, t, rbuf, 1, t, MPI_COMM_WORLD);
    } else {
      MPI_Request req;
      MPI_Ialltoall(sbuf, 1, t, rbuf, 1, t, MPI_COMM_WORLD, &req);
      MPI_Wait(&req, MPI_STATUS_IGNORE);
    }
    for (int s = 0; s < size; s++) checkBlock(nb ? "Ialltoall" : "Alltoall", rbuf, s, id, s, rank);
    free(sbuf);
    free(rbuf);
  }
}

// Intercommunicator scatter from rank 0 of the even group to the odd group,
// in the four forms; the odd group's rank r receives block r.
static void testIntercommScatter(MPI_Datatype t)
{
  int rank, size;
  getRS(MPI_COMM_WORLD, rank, size);
  const int color = rank % 2;
  MPI_Comm local, inter;
  MPI_Comm_split(MPI_COMM_WORLD, color, rank, &local);
  MPI_Intercomm_create(local, 0, MPI_COMM_WORLD, color == 0 ? 1 : 0, 17, &inter);
  int lrank, lsize, rsize;
  getRS(local, lrank, lsize);
  MPI_Comm_remote_size(inter, &rsize);

  for (int form = 0; form < 4; form++) {
    const char *names[4] = {"intercomm Scatter", "intercomm Iscatter", "intercomm Scatterv", "intercomm Iscatterv"};
    const int id = 10 + form;
    const bool v = form >= 2, nb = form % 2 == 1;
    int *sbuf = NULL, *rbuf = blocks(1);
    int *counts = NULL, *displs = NULL;
    MPI_Request req = MPI_REQUEST_NULL;
    if (color == 0 && lrank == 0) {
      // Root: one block per remote rank; the v forms place block r at
      // element displacement r*2 (every other block of a doubled buffer).
      sbuf = blocks(v ? 2 * rsize : rsize);
      counts = (int *)malloc(sizeof(int) * rsize);
      displs = (int *)malloc(sizeof(int) * rsize);
      for (int r = 0; r < rsize; r++) {
        counts[r] = 1;
        displs[r] = v ? 2 * r : r;
        fillBlock(sbuf, displs[r], id, 0, r);
      }
    }
    int root = (color == 0) ? (lrank == 0 ? MPI_ROOT : MPI_PROC_NULL) : 0;
    switch (form) {
      case 0: MPI_Scatter(sbuf, 1, t, rbuf, 1, t, root, inter); break;
      case 1: MPI_Iscatter(sbuf, 1, t, rbuf, 1, t, root, inter, &req); MPI_Wait(&req, MPI_STATUS_IGNORE); break;
      case 2: MPI_Scatterv(sbuf, counts, displs, t, rbuf, 1, t, root, inter); break;
      case 3: MPI_Iscatterv(sbuf, counts, displs, t, rbuf, 1, t, root, inter, &req); MPI_Wait(&req, MPI_STATUS_IGNORE); break;
    }
    if (color == 1) checkBlock(names[form], rbuf, 0, id, 0, lrank);
    free(sbuf);
    free(rbuf);
    free(counts);
    free(displs);
  }
  MPI_Comm_free(&inter);
  MPI_Comm_free(&local);
}

// Neighbor alltoalls on a periodic ring: neighbor 0 is the left rank,
// neighbor 1 the right one (MPI 3.1 7.6, Cartesian order), in the four forms.
static void testNeighbor(MPI_Datatype t)
{
  int rank, size;
  getRS(MPI_COMM_WORLD, rank, size);
  int dims[1] = {size}, periods[1] = {1};
  MPI_Comm cart;
  MPI_Cart_create(MPI_COMM_WORLD, 1, dims, periods, 0, &cart);
  int crank;
  MPI_Comm_rank(cart, &crank);
  // The neighbor order: MPI 3.1 7.6 lists the Cartesian neighbors as left,
  // right, and AMPI lists them as right, left (charm issue #4101). The order
  // is not under test here, so take it from the implementation: each rank
  // sends its own rank to both neighbors, and block j then holds the rank
  // of neighbor j.
  int sendRanks[2] = {crank, crank}, nbr[2] = {-1, -1};
  MPI_Neighbor_alltoall(sendRanks, 1, MPI_INT, nbr, 1, MPI_INT, cart);
  const int left = (crank + size - 1) % size, right = (crank + 1) % size;
  if (!((nbr[0] == left && nbr[1] == right) || (nbr[0] == right && nbr[1] == left)))
    fail("Neighbor_alltoall neighbor ranks", nbr[0], nbr[1], left, right);

  for (int form = 0; form < 4; form++) {
    const char *names[4] = {"Neighbor_alltoall", "Ineighbor_alltoall", "Neighbor_alltoallv", "Ineighbor_alltoallv"};
    const int id = 20 + form;
    const bool v = form >= 2;
    // The v forms place block i at element displacement 3*i.
    int *sbuf = blocks(v ? 4 : 2), *rbuf = blocks(v ? 4 : 2);
    int counts[2] = {1, 1}, sdispls[2] = {0, v ? 3 : 1}, rdispls[2] = {0, v ? 3 : 1};
    for (int i = 0; i < 2; i++) fillBlock(sbuf, sdispls[i], id, crank, nbr[i]);
    MPI_Request req = MPI_REQUEST_NULL;
    switch (form) {
      case 0: MPI_Neighbor_alltoall(sbuf, 1, t, rbuf, 1, t, cart); break;
      case 1: MPI_Ineighbor_alltoall(sbuf, 1, t, rbuf, 1, t, cart, &req); MPI_Wait(&req, MPI_STATUS_IGNORE); break;
      case 2: MPI_Neighbor_alltoallv(sbuf, counts, sdispls, t, rbuf, counts, rdispls, t, cart); break;
      case 3: MPI_Ineighbor_alltoallv(sbuf, counts, sdispls, t, rbuf, counts, rdispls, t, cart, &req); MPI_Wait(&req, MPI_STATUS_IGNORE); break;
    }
    // Block i of the receive buffer came from neighbor i.
    for (int i = 0; i < 2; i++) checkBlock(names[form], rbuf, rdispls[i], id, nbr[i], crank);
    free(sbuf);
    free(rbuf);
  }
  MPI_Comm_free(&cart);
}

int main(int argc, char **argv)
{
  MPI_Init(&argc, &argv);
  int rank, size;
  getRS(MPI_COMM_WORLD, rank, size);
  if (size < 2) {
    if (rank == 0) printf("coll_extent needs at least 2 ranks\n");
    MPI_Abort(MPI_COMM_WORLD, 1);
  }
  MPI_Datatype t = blockType();
  testAlltoall(t);
  testIntercommScatter(t);
  testNeighbor(t);
  MPI_Type_free(&t);
  MPI_Barrier(MPI_COMM_WORLD);
  if (rank == 0) { printf("coll_extent: all tests passed on %d ranks\n", size); fflush(stdout); }
  MPI_Finalize();
  return 0;
}

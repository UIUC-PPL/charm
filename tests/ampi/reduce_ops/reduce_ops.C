/* Every predefined reduction operation on every predefined type it applies
 * to, with MPI_Reduce and MPI_Allreduce, checked against the same fold
 * computed by rank 0 from the inputs every rank would contribute.
 *
 * The cases cover the three ways AMPI reduces: a Charm++ built-in reducer
 * (for example MPI_SUM on MPI_INT), AMPI's own reducer running the op's
 * MPI function (MPI_BOR on MPI_LONG_LONG), and the rank-ordered path for
 * non-contiguous types (MPI_MAXLOC on the pair types, which are structs)
 * and for non-commutative user operations. Each wrong element is reported;
 * any failure aborts the program after the report.
 */
#include <mpi.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <type_traits>
#include <string>

static const int COUNT = 3;

/* Per-rank state. AMPI ranks in one process share the program's globals
 * unless the program is privatized, so nothing a rank writes is global. */
struct Ctx { int rank, size, failures, caseNo; };

/* Numeric input of rank r for element i, op-dependent so that every op has
 * a result that is exact in every type: products stay at most 2, sums stay
 * below 128 for up to 11 ranks, logical inputs are 0 or 1. */
template <typename T> static T input(int op, int r, int i) {
  if (op == MPI_PROD) return (T)((r == i % 4) ? 2 : 1);
  if (op == MPI_LAND || op == MPI_LOR || op == MPI_LXOR) return (T)(((r + i) % 3) != 0);
  return (T)((r * 7 + i * 3) % 11 + 1);
}

template <typename T> static T applyBits(int op, T a, T acc, std::true_type) {
  if (op == MPI_BAND) return (T)(acc & a);
  if (op == MPI_BOR) return (T)(acc | a);
  if (op == MPI_BXOR) return (T)(acc ^ a);
  return acc;
}
template <typename T> static T applyBits(int, T, T acc, std::false_type) { return acc; }

template <typename T> static T apply(int op, T a, T acc) {
  if (op == MPI_MAX) return a > acc ? a : acc;
  if (op == MPI_MIN) return a < acc ? a : acc;
  if (op == MPI_SUM) return (T)(acc + a);
  if (op == MPI_PROD) return (T)(acc * a);
  if (op == MPI_LAND) return (T)(acc && a);
  if (op == MPI_LOR) return (T)(acc || a);
  if (op == MPI_LXOR) return (T)((!acc) != (!a));
  return applyBits<T>(op, a, acc, std::is_integral<T>());
}

template <typename T> static std::string show(T v) {
  char b[64];
  if (std::is_floating_point<T>::value) snprintf(b, sizeof b, "%g", (double)v);
  else snprintf(b, sizeof b, "%lld", (long long)v);
  return b;
}

// Fill the result buffer with a value no case produces, so a reduction
// that writes nothing is caught. A bool holds only two values; true is the
// one fewer cases produce.
template <typename T> static void poison(T *out) { memset(out, 0xA5, sizeof(T) * COUNT); }
static void poison(bool *out) { for (int i = 0; i < COUNT; i++) out[i] = true; }

static const char *opName(int op) {
  switch (op) {
    case MPI_MAX: return "MAX"; case MPI_MIN: return "MIN"; case MPI_SUM: return "SUM";
    case MPI_PROD: return "PROD"; case MPI_LAND: return "LAND"; case MPI_LOR: return "LOR";
    case MPI_LXOR: return "LXOR"; case MPI_BAND: return "BAND"; case MPI_BOR: return "BOR";
    case MPI_BXOR: return "BXOR"; case MPI_MAXLOC: return "MAXLOC"; case MPI_MINLOC: return "MINLOC";
    default: return "user";
  }
}

/* Run one (op, type) case: reduce to a root that changes with the case, then
 * allreduce; check against expect[]. equal() and show() come from the caller
 * so the same driver serves the numeric and the pair types. */
template <typename T, typename Eq, typename Show>
static void runCase(Ctx &c, MPI_Datatype dt, const char *tname, MPI_Op op, const char *oname,
                    const T *in, const T *expect, Eq equal, Show shw) {
  int root = c.caseNo++ % c.size;
  T out[COUNT];
  for (int pass = 0; pass < 2; pass++) {
    poison(out);
    bool check;
    if (pass == 0) {
      MPI_Reduce(in, out, COUNT, dt, op, root, MPI_COMM_WORLD);
      check = (c.rank == root);
    } else {
      MPI_Allreduce(in, out, COUNT, dt, op, MPI_COMM_WORLD);
      check = true;
    }
    if (!check) continue;
    for (int i = 0; i < COUNT; i++) {
      if (!equal(out[i], expect[i])) {
        c.failures++;
        printf("[%d] FAIL %s %s %s elem %d: got %s expected %s\n", c.rank,
               pass == 0 ? "Reduce" : "Allreduce", oname, tname, i,
               shw(out[i]).c_str(), shw(expect[i]).c_str());
      }
    }
  }
}

template <typename T> static void numeric(Ctx &c, MPI_Datatype dt, const char *tname, const int *ops, int nops) {
  for (int k = 0; k < nops; k++) {
    int op = ops[k];
    T in[COUNT], expect[COUNT];
    for (int i = 0; i < COUNT; i++) {
      in[i] = input<T>(op, c.rank, i);
      T acc = input<T>(op, 0, i);
      for (int r = 1; r < c.size; r++) acc = apply<T>(op, input<T>(op, r, i), acc);
      expect[i] = acc;
    }
    runCase<T>(c, dt, tname, op, opName(op), in, expect,
               [](T a, T b) { return a == b; }, [](T v) { return show<T>(v); });
  }
}

static const int arithOps[] = {MPI_MAX, MPI_MIN, MPI_SUM, MPI_PROD};
static const int logicalOps[] = {MPI_LAND, MPI_LOR, MPI_LXOR};
static const int bitOps[] = {MPI_BAND, MPI_BOR, MPI_BXOR};
static const int allIntOps[] = {MPI_MAX, MPI_MIN, MPI_SUM, MPI_PROD, MPI_LAND, MPI_LOR, MPI_LXOR, MPI_BAND, MPI_BOR, MPI_BXOR};

/* Pair types for MAXLOC and MINLOC: value of rank r for element i, with
 * ties, so the index rule (the lowest rank among equal values) is tested. */
template <typename V> struct Pair { V v; int i; };
template <typename V> static Pair<V> pairInput(int r, int i) { Pair<V> p; p.v = (V)((r * 5 + i * 3) % 7); p.i = r; return p; }
template <typename V> static Pair<V> pairApply(int op, Pair<V> a, Pair<V> acc) {
  if (op == MPI_MAXLOC) { if (a.v > acc.v || (a.v == acc.v && a.i < acc.i)) return a; return acc; }
  if (a.v < acc.v || (a.v == acc.v && a.i < acc.i)) return a; return acc;
}
template <typename V> static void pairs(Ctx &c, MPI_Datatype dt, const char *tname) {
  const int ops[] = {MPI_MAXLOC, MPI_MINLOC};
  for (int op : ops) {
    Pair<V> in[COUNT], expect[COUNT];
    for (int i = 0; i < COUNT; i++) {
      in[i] = pairInput<V>(c.rank, i);
      Pair<V> acc = pairInput<V>(0, i);
      for (int r = 1; r < c.size; r++) acc = pairApply<V>(op, pairInput<V>(r, i), acc);
      expect[i] = acc;
    }
    runCase<Pair<V>>(c, dt, tname, op, opName(op), in, expect,
                     [](Pair<V> a, Pair<V> b) { return a.v == b.v && a.i == b.i; },
                     [](Pair<V> p) { return "(" + show<V>(p.v) + "," + show<int>(p.i) + ")"; });
  }
}

/* User operations: a commutative one on MPI_2INT (a non-contiguous type,
 * so AMPI reduces it in rank order), and a non-commutative one on MPI_INT,
 * whose result depends on the fold order the standard prescribes. */
static void userPairFn(void *in, void *inout, int *len, MPI_Datatype *) {
  Pair<int> *a = (Pair<int> *)in, *b = (Pair<int> *)inout;
  for (int k = 0; k < *len; k++) { b[k].v += a[k].v; if (a[k].i > b[k].i) b[k].i = a[k].i; }
}
static void userNoncommFn(void *in, void *inout, int *len, MPI_Datatype *) {
  int *a = (int *)in, *b = (int *)inout;
  for (int k = 0; k < *len; k++) b[k] = b[k] * 3 + a[k];
}

static void userOps(Ctx &c) {
  MPI_Op pairOp, noncommOp;
  MPI_Op_create(userPairFn, 1, &pairOp);
  MPI_Op_create(userNoncommFn, 0, &noncommOp);
  {
    Pair<int> in[COUNT], expect[COUNT];
    for (int i = 0; i < COUNT; i++) {
      in[i] = pairInput<int>(c.rank, i);
      Pair<int> acc = pairInput<int>(0, i);
      for (int r = 1; r < c.size; r++) { Pair<int> a = pairInput<int>(r, i); acc.v += a.v; if (a.i > acc.i) acc.i = a.i; }
      expect[i] = acc;
    }
    runCase<Pair<int>>(c, MPI_2INT, "MPI_2INT", pairOp, "user-commutative", in, expect,
                       [](Pair<int> a, Pair<int> b) { return a.v == b.v && a.i == b.i; },
                       [](Pair<int> p) { return "(" + show<int>(p.v) + "," + show<int>(p.i) + ")"; });
  }
  {
    int in[COUNT], expect[COUNT];
    for (int i = 0; i < COUNT; i++) {
      in[i] = input<int>(MPI_SUM, c.rank, i);
      int acc = input<int>(MPI_SUM, 0, i);
      for (int r = 1; r < c.size; r++) acc = acc * 3 + input<int>(MPI_SUM, r, i);
      expect[i] = acc;
    }
    runCase<int>(c, MPI_INT, "MPI_INT", noncommOp, "user-noncommutative", in, expect,
                 [](int a, int b) { return a == b; }, [](int v) { return show<int>(v); });
  }
  MPI_Op_free(&pairOp);
  MPI_Op_free(&noncommOp);
}

int main(int argc, char **argv) {
  MPI_Init(&argc, &argv);
  Ctx c = {0, 0, 0, 0};
  MPI_Comm_rank(MPI_COMM_WORLD, &c.rank);
  MPI_Comm_size(MPI_COMM_WORLD, &c.size);

  // MPI_CHAR is a text type: the standard allows no logical or bitwise
  // operation on it, so it gets the arithmetic ones only.
  numeric<char>(c, MPI_CHAR, "MPI_CHAR", arithOps, 4);
#define INT_TYPE(T, DT) numeric<T>(c, DT, #DT, allIntOps, 10)
  INT_TYPE(signed char, MPI_SIGNED_CHAR);
  INT_TYPE(unsigned char, MPI_UNSIGNED_CHAR);
  INT_TYPE(short, MPI_SHORT);
  INT_TYPE(unsigned short, MPI_UNSIGNED_SHORT);
  INT_TYPE(int, MPI_INT);
  INT_TYPE(unsigned, MPI_UNSIGNED);
  INT_TYPE(long, MPI_LONG);
  INT_TYPE(unsigned long, MPI_UNSIGNED_LONG);
  INT_TYPE(long long, MPI_LONG_LONG);
  INT_TYPE(unsigned long long, MPI_UNSIGNED_LONG_LONG);
  INT_TYPE(int8_t, MPI_INT8_T);
  INT_TYPE(int16_t, MPI_INT16_T);
  INT_TYPE(int32_t, MPI_INT32_T);
  INT_TYPE(int64_t, MPI_INT64_T);
  INT_TYPE(uint8_t, MPI_UINT8_T);
  INT_TYPE(uint16_t, MPI_UINT16_T);
  INT_TYPE(uint32_t, MPI_UINT32_T);
  INT_TYPE(uint64_t, MPI_UINT64_T);
#undef INT_TYPE
  numeric<float>(c, MPI_FLOAT, "MPI_FLOAT", arithOps, 4);
  numeric<double>(c, MPI_DOUBLE, "MPI_DOUBLE", arithOps, 4);
  numeric<long double>(c, MPI_LONG_DOUBLE, "MPI_LONG_DOUBLE", arithOps, 4);
  numeric<bool>(c, MPI_C_BOOL, "MPI_C_BOOL", logicalOps, 3);
  numeric<unsigned char>(c, MPI_BYTE, "MPI_BYTE", bitOps, 3);

  pairs<float>(c, MPI_FLOAT_INT, "MPI_FLOAT_INT");
  pairs<double>(c, MPI_DOUBLE_INT, "MPI_DOUBLE_INT");
  pairs<long>(c, MPI_LONG_INT, "MPI_LONG_INT");
  pairs<int>(c, MPI_2INT, "MPI_2INT");
  pairs<short>(c, MPI_SHORT_INT, "MPI_SHORT_INT");
  pairs<long double>(c, MPI_LONG_DOUBLE_INT, "MPI_LONG_DOUBLE_INT");

  userOps(c);

  int total = 0;
  MPI_Allreduce(&c.failures, &total, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
  if (c.rank == 0) {
    if (total == 0) printf("All tests passed (%d cases, %d ranks)\n", c.caseNo, c.size);
    else printf("%d failures in %d cases\n", total, c.caseNo);
  }
  if (total != 0) MPI_Abort(MPI_COMM_WORLD, 1);
  MPI_Finalize();
  return 0;
}

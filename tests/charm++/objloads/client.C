// Client half of the CkPerfObjLoads test. Connects to a running objloads
// server, fetches "CkPerfObjLoads" (the trace) and "LBObjLoads" (the load
// database) and checks that they describe the same objects:
//   - same set of element ids on every PE, and as many distinct ids as
//     elements were created (nothing merged, nothing split);
//   - each element's time agrees between the two, including the elements
//     whose time arrives in resumed thread segments;
//   - per PE, elapsed - idle is close to the sum of the object times;
//   - a second trace snapshot a second later has not gone backwards.
//
//   ./client <host> <port> <expected number of elements, or 0 to skip that check>
#include "ccs-client.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <unistd.h>

typedef unsigned long long u64;
typedef std::pair<int, u64> Key;   // collection (object manager), id
struct Obj { double wall; };
struct Pe { double now, total, idle; std::map<Key, Obj> objs; };
typedef std::map<int, Pe> Reply;

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static bool decode(const char *d, int len, Reply &out) {
  int pos = 0;
  auto get = [&](void *p, int n) -> bool {
    if (pos + n > len) return false;
    memcpy(p, d + pos, n); pos += n; return true;
  };
  int npes;
  if (!get(&npes, 4)) return false;
  for (int i = 0; i < npes; i++) {
    int pe, n; Pe p;
    if (!get(&pe, 4) || !get(&p.now, 8) || !get(&p.total, 8) || !get(&p.idle, 8) || !get(&n, 4)) return false;
    for (int j = 0; j < n; j++) {
      u64 id; int coll; Obj o; unsigned char mig;
      if (!get(&id, 8) || !get(&coll, 4) || !get(&o.wall, 8) || !get(&mig, 1)) return false;
      p.objs[Key(coll, id)] = o;
    }
    out[pe] = p;
  }
  return pos == len;
}

static bool fetch(CcsServer &svr, const char *handler, Reply &out) {
  CcsSendRequest(&svr, handler, 0, 0, NULL);
  int size = 0; void *buf = NULL;
  if (CcsRecvResponseMsg(&svr, &size, &buf, 60) < 0) { printf("FAIL: no reply to %s\n", handler); return false; }
  bool ok = decode((const char *)buf, size, out);
  free(buf);
  if (!ok) printf("FAIL: %s reply did not decode (%d bytes)\n", handler, size);
  return ok;
}

int main(int argc, char **argv) {
  if (argc < 4) { fprintf(stderr, "usage: %s host port expected-elements\n", argv[0]); return 2; }
  int expected = atoi(argv[3]);
  CcsServer svr;
  if (CcsConnect(&svr, argv[1], atoi(argv[2]), NULL) < 0) { printf("FAIL: cannot connect\n"); return 1; }
  int npes = CcsNumPes(&svr);

  Reply trace, lb;
  if (!fetch(svr, "CkPerfObjLoads", trace) || !fetch(svr, "LBObjLoads", lb)) return 1;
  CHECK((int)trace.size() == npes, "trace reply has %zu PEs, job has %d", trace.size(), npes);
  CHECK((int)lb.size() == npes, "LBObjLoads reply has %zu PEs, job has %d", lb.size(), npes);

  std::set<Key> all;
  int compared = 0, offBy = 0;
  double worst = 0;
  for (auto &kv : trace) {
    int pe = kv.first;
    const Pe &t = kv.second;
    if (!lb.count(pe)) { CHECK(false, "PE %d missing from LBObjLoads", pe); continue; }
    const Pe &l = lb.at(pe);
    for (auto &o : t.objs) {
      all.insert(o.first);
      if (!l.objs.count(o.first)) { CHECK(false, "PE %d: trace id %d/%llx not in the load database", pe, o.first.first, o.first.second); continue; }
      double tw = o.second.wall, lw = l.objs.at(o.first).wall;
      double diff = tw > lw ? tw - lw : lw - tw;
      double tol = 0.05 * (tw > lw ? tw : lw) + 0.02;
      compared++;
      if (diff > worst) worst = diff;
      if (diff > tol) { offBy++; printf("  PE %d id %d/%llx: trace %.4f s, database %.4f s\n", pe, o.first.first, o.first.second, tw, lw); }
    }
    for (auto &o : l.objs)
      CHECK(t.objs.count(o.first), "PE %d: database id %d/%llx not in the trace", pe, o.first.first, o.first.second);
    double busy = t.total - t.idle, sum = 0;
    for (auto &o : t.objs) sum += o.second.wall;
    CHECK(busy >= 0 && sum <= busy + 0.1 * t.total + 0.05 && sum >= busy - 0.1 * t.total - 0.05,
          "PE %d: elapsed %.3f - idle %.3f = %.3f busy, but objects sum to %.3f", pe, t.total, t.idle, busy, sum);
    printf("PE %d: %zu objects, elapsed %.2f s, idle %.2f s, objects %.2f s\n", pe, t.objs.size(), t.total, t.idle, sum);
  }
  if (expected > 0)
    CHECK((int)all.size() == expected, "%zu distinct ids, %d elements created", all.size(), expected);
  CHECK(offBy == 0, "%d of %d elements differ between trace and database by more than 5%% + 20 ms", offBy, compared);
  printf("%d elements compared, largest difference %.4f s\n", compared, worst);

  sleep(1);
  Reply later;
  if (!fetch(svr, "CkPerfObjLoads", later)) return 1;
  int grew = 0;
  for (auto &kv : later)
    for (auto &o : kv.second.objs) {
      if (!trace.count(kv.first) || !trace.at(kv.first).objs.count(o.first)) continue;
      double before = trace.at(kv.first).objs.at(o.first).wall;
      CHECK(o.second.wall >= before, "PE %d id %d/%llx went from %.4f to %.4f", kv.first, o.first.first, o.first.second, before, o.second.wall);
      if (o.second.wall > before) grew++;
    }
  CHECK(grew > 0, "no object's time grew in a second");

  printf(failures ? "objloads client: FAIL (%d)\n" : "objloads client: PASS\n", failures);
  return failures ? 1 : 0;
}

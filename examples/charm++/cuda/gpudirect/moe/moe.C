#include "hapi.h"
#include "moe.decl.h"
#include "moe.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>
#include <sys/time.h>
#include <unistd.h>

// CHARM_MOE_TRACE=1: timestamped LB, migration and dispatcher-step events per
// PE, for reconstructing what a load-balancing step costs and where; =2 adds
// the experts' per-step events. Wall-clock seconds so lines from different
// processes on a node line up.
static inline double tnow() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec + tv.tv_usec * 1e-6;
}
static inline int traceLevel() {
  static const int l = getenv("CHARM_MOE_TRACE") ? atoi(getenv("CHARM_MOE_TRACE")) : 0;
  return l;
}
// CHARM_MOE_HOSTSYNC=1: the original migration behaviour, for A/B runs --
// the pup and the destructor stop the host until the shared stream drains.
static inline bool hostSyncMode() {
  static const bool on = (getenv("CHARM_MOE_HOSTSYNC") != nullptr);
  return on;
}
#define TRACE(lvl, fmt, ...) do { \
    if (traceLevel() >= (lvl)) \
      CkPrintf("[T %.6f pe%d] " fmt "\n", tnow(), CkMyPe(), ##__VA_ARGS__); \
  } while (0)

// Device buffers come from hapiMalloc/hapiFree, which are the device pool
// under +gpupool (an arena the peers have already opened, no driver call, no
// device sync) and hapiMalloc/hapiFree without it. The runtime owns the switch.

/* readonly */ CProxy_Main main_proxy;
/* readonly */ CProxy_Dispatcher disp_proxy;
/* readonly */ CProxy_Expert expert_proxy;
/* readonly */ int n_experts;
/* readonly */ int d_model;
/* readonly */ int d_ff;
/* readonly */ int n_tokens;
/* readonly */ int top_k;
/* readonly */ int n_steps;
/* readonly */ int warmup_steps;
/* readonly */ int chunk_tokens;
/* readonly */ int n_lanes;
/* readonly */ int cap_src;
/* readonly */ int n_disp;
/* readonly */ int drift_period;
/* readonly */ int first_lb;
/* readonly */ int lb_freq;
/* readonly */ int async_lb;
/* readonly */ int lb_wait_lag;
/* readonly */ int checksum_freq;
/* readonly */ int stats_freq;
/* readonly */ int use_adam;
/* readonly */ int use_tf32;
/* readonly */ int use_bf16;
/* readonly */ int serve_only;
/* readonly */ int use_instrument;
/* readonly */ double capacity_factor;
/* readonly */ int fuse_sources;
/* readonly */ int stream_outputs;
/* readonly */ int send_ahead;
/* readonly */ int tok_channels;
/* readonly */ int print_place;
/* readonly */ int flight_log;
/* readonly */ int rep_max;
/* readonly */ int rep_init_expert;
/* readonly */ double zipf_z;
/* readonly */ std::string trace_path;   // -E: routing replayed from a recorded trace ("" = hashed Zipf)
/* readonly */ int use_attn;              // -w: an attention sublayer (32 heads, sequences of 1024) before the MoE
/* readonly */ double learning_rate;
/* readonly */ long moe_seed;

// -F: every slab and output message carries its sender's clock and PE; the
// receiver notes the envelope (the post hook that picks the landing buffer)
// and the payload (the SDAG body), and each PE prints its records after the
// last step. CLOCK_REALTIME in ns, the clock the +gputrace records use, so the
// records line up with a trace; across nodes the clocks agree only to NTP.
static inline long long moeNowNs() {
  struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
  return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
struct MoeFlight { char kind; int step, spe, dpe, expert; long bytes; long long t_send, t_env, t_land; };
static thread_local std::vector<MoeFlight> g_flights;
static void moeFlightDump() {
  for (const MoeFlight& f : g_flights)
    CkPrintf("[FLIGHT] %c step %d src %d dst %d expert %d bytes %ld send %lld env %lld land %lld\n", f.kind,
        f.step, f.spe, f.dpe, f.expert, f.bytes, f.t_send, f.t_env, f.t_land);
  g_flights.clear();
}

// ---- host hashing: routing is a pure function of (seed, step, PE, token,
// slot), so every run with the same arguments routes identically whatever the
// placement, and the checksums can be compared across runs.

static inline uint64_t sm64(uint64_t x) {
  x += 0x9E3779B97F4A7C15ULL;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
  return x ^ (x >> 31);
}
static inline uint64_t hmix(uint64_t a, uint64_t b) {
  return sm64(a ^ (b * 0x9E3779B97F4A7C15ULL + 0x632BE59BD9B4E019ULL));
}
static inline double u01(uint64_t h) {
  return ((double)(h >> 11) + 0.5) / 9007199254740992.0;
}

// The hot set of a phase: a hashed permutation of the experts; rank r draws a
// share proportional to (r+1)^-z, so perm[0] is the hottest expert.
// A routing trace (ref/trace_routing.py): header int32[4] {0x4d4f4554, n_experts, k, n_tokens},
// then int32 ids[n_tokens][k], the top-k experts of each token of a text in text order. Token t of
// dispatcher d at step s replays text token ((s - 1) * n_disp + d) * n_tokens + t, modulo the trace
// length; the PyTorch harnesses apply the same rule, so every implementation routes the same tokens.
static void loadTrace(const std::string& path, int E, int K, std::vector<int32_t>& ids) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) CkAbort("-E %s: cannot open the routing trace\n", path.c_str());
  int32_t hdr[4];
  if (fread(hdr, sizeof(int32_t), 4, f) != 4 || hdr[0] != 0x4d4f4554)
    CkAbort("-E %s: not a routing trace\n", path.c_str());
  if (hdr[1] != E || hdr[2] != K)
    CkAbort("-E %s: the trace has %d experts and top-%d, the run %d and top-%d\n", path.c_str(),
        hdr[1], hdr[2], E, K);
  ids.resize((size_t)hdr[3] * K);
  if (fread(ids.data(), sizeof(int32_t), ids.size(), f) != ids.size())
    CkAbort("-E %s: short read\n", path.c_str());
  fclose(f);
}

static void hotSetPermutation(int phase, std::vector<int>& perm) {
  perm.resize(n_experts);
  for (int i = 0; i < n_experts; i++) perm[i] = i;
  for (int i = n_experts - 1; i > 0; i--) {
    const uint64_t h = hmix(hmix(moe_seed, 77), (uint64_t)phase * n_experts + i);
    const int j = (int)(h % (uint64_t)(i + 1));
    std::swap(perm[i], perm[j]);
  }
}

// Token channels per (expert, dispatcher): one per step parity when the next
// step's tokens can land while this step computes, else one.
static inline int tokParities() { return send_ahead ? 2 : 1; }

static inline bool isLBStep(int s) {
  return s == first_lb || (s != 0 && lb_freq > 0 && s % lb_freq == 0);
}
static inline int nextLBStep(int s) {
  if (s < first_lb) return first_lb;
  if (lb_freq <= 0) return INT_MAX;
  return ((s / lb_freq) + 1) * lb_freq;
}
// A step inside a balancing window: a balancing step, or one of the lag steps
// after it under the split barrier. The dispatchers send no tokens ahead
// across such a step (Dispatcher::sendAhead), so its outputs are in before
// the next step is routed, and a change of an expert's copies announced in
// them takes effect cleanly at the next step.
static inline bool inLBWindow(int s) {
  const int lag = async_lb ? lb_wait_lag : 0;
  for (int L = s - lag; L <= s; L++)
    if (L >= 1 && isLBStep(L)) return true;
  return false;
}
// Steps from the previous balancing step to step s: the span the balancer's
// own host-side loads accumulate over, and so the span a declared device
// load has to cover to be commensurate with them (LBLoadDim.h). Before the
// first balancing step, the run so far.
static inline int stepsSinceLastLB(int s) {
  if (s <= first_lb) return s > 0 ? s : 1;
  int prev = (lb_freq > 0) ? (s / lb_freq) * lb_freq : 0;
  if (prev == s) prev -= lb_freq;
  if (prev < first_lb) prev = first_lb;
  return s - prev > 0 ? s - prev : 1;
}

// Replication: an expert may exist as k copies (e, 0) .. (e, k-1). The
// dispatchers are in K_PART fixed partitions, p = d % K_PART, whatever k is;
// copy r of k serves the contiguous block of partitions [K_PART r / k,
// K_PART (r+1) / k), a single partition or an aligned pair. The step's
// gradient is always the fixed tree ((P0 + P1) + (P2 + P3)) over the
// partitions' accumulators (Expert::treeNodes): a copy computes its block's
// leaves, folds them to its block sum, exchanges block sums with the other
// copies and assembles the two tree nodes; an unreplicated expert computes
// all four leaves itself and folds the same way. Every copy then applies the
// same update, so their weights stay identical, and out2 and w2 are the same
// bits whatever the balancer did (doc/internals/lb-multiplicity.md).
#ifndef MOE_K_PART
#define MOE_K_PART 4
#endif
static const int K_PART = MOE_K_PART;   // dispatcher partitions, fixed at build time (-DMOE_K_PART=2 for the two-partition variant)
static const int K_MAX_COPIES = 4;  // copies the channel tables are sized for
static inline int partitionOf(int disp) { return disp % K_PART; }
static inline int partitionSize(int p) {
  int n = 0;
  for (int d = 0; d < n_disp; d++) if (partitionOf(d) == p) n++;
  return n;
}
static inline int blockLo(int r, int k) { return (K_PART * r) / k; }
static inline int blockHi(int r, int k) { return (K_PART * (r + 1)) / k; }
// Which copy of an expert with `mult` copies serves dispatcher d.
static inline int copyFor(int disp, int mult) {
  const int p = partitionOf(disp);
  for (int r = 0; r < mult; r++) if (p < blockHi(r, mult)) return r;
  return mult - 1;
}

// Bytes per element of a token, an activation or an output: bf16 under -B
// (moe.h), else fp32. The master weights, the gradient accumulators and the
// checksums are fp32 either way.
static inline size_t actEsz() { return use_bf16 ? sizeof(moe_bf16) : sizeof(float); }

// Per-step figures from one of the two report streams, held until the other
// stream's record of the same step arrives so one line reports both.
struct DispRec { double out2; double drop; double t; };
struct ExpRec { double tokratio, gpuratio, gmax_ms, expratio, w2; long sum_tok; };

class Main : public CBase_Main {
  double init_start_time;
  double start_time;
  double window_start_time;
  int disp_step;   // steps whose dispatcher reports have all arrived
  int exp_step;    // steps whose expert reports have all arrived
  int done_count;
  std::deque<DispRec> disp_q;
  std::deque<ExpRec> exp_q;

public:
  Main(CkArgMsg* m) {
    main_proxy = thisProxy;

    n_experts = 64;
    d_model = 2048;
    d_ff = 8192;
    n_tokens = 8192;
    top_k = 1;
    n_steps = 50;
    warmup_steps = 5;
    zipf_z = 1.0;
    use_attn = 0;
    drift_period = 10;
    chunk_tokens = 1024;
    n_lanes = 0;   // resolved below: -n, else four per GPU shared by its PEs
    double headroom = 0.0;
    learning_rate = 1e-4;
    checksum_freq = 5;
    stats_freq = 1;
    first_lb = 10;
    lb_freq = 9999;
    lb_wait_lag = 3;
    moe_seed = 12345;
    async_lb = 0;
    use_adam = 0;
    use_tf32 = 0;
    use_bf16 = 0;
    serve_only = 0;
    use_instrument = 0;
    capacity_factor = 0.0;
    fuse_sources = 1;
    stream_outputs = 1;
    send_ahead = 1;
    tok_channels = 1;
    print_place = 0;
    flight_log = 0;
    rep_max = 2;
    rep_init_expert = -1;
    bool rep_init = false;
    disp_step = exp_step = done_count = 0;

    int c;
    while ((c = getopt(m->argc, m->argv, "e:m:h:t:k:i:u:z:p:c:r:L:C:s:f:b:l:S:n:D:R:E:aAUTIPFOYMXBGw")) != -1) {
      switch (c) {
        case 'e': n_experts = atoi(optarg); break;
        case 'm': d_model = atoi(optarg); break;
        case 'h': d_ff = atoi(optarg); break;
        case 't': n_tokens = atoi(optarg); break;
        case 'k': top_k = atoi(optarg); break;
        case 'i': n_steps = atoi(optarg); break;
        case 'u': warmup_steps = atoi(optarg); break;
        case 'z': zipf_z = atof(optarg); break;
        case 'E': trace_path = optarg; break;
        case 'w': use_attn = 1; break;
        case 'p': drift_period = atoi(optarg); break;
        case 'c': chunk_tokens = atoi(optarg); break;
        case 'n': n_lanes = atoi(optarg); break;
        case 'r': headroom = atof(optarg); break;
        case 'L': learning_rate = atof(optarg); break;
        case 'C': checksum_freq = atoi(optarg); break;
        case 's': stats_freq = atoi(optarg); break;
        case 'f': first_lb = atoi(optarg); break;
        case 'b': lb_freq = atoi(optarg); break;
        case 'l': lb_wait_lag = atoi(optarg); break;
        case 'S': moe_seed = atol(optarg); break;
        case 'R': rep_max = atoi(optarg); break;
        case 'X': rep_init = true; break;
        case 'a': async_lb = 1; break;
        case 'A': use_adam = 1; break;
        case 'T': use_tf32 = 1; break;
        case 'B': use_bf16 = 1; break;
        case 'G': serve_only = 1; break;
        case 'I': use_instrument = 1; break;
        case 'D': capacity_factor = atof(optarg); break;
        case 'U': fuse_sources = 0; break;
        case 'O': stream_outputs = 0; break;
        case 'Y': send_ahead = 0; break;
        case 'M': tok_channels = 0; break;
        case 'P': print_place = 1; break;
        case 'F': flight_log = 1; break;
        default:
          CkPrintf(
              "Usage: %s -e [experts] -m [d_model] -h [d_ff] -t [tokens per process per step]\n"
              "  -k [top-k] -i [steps] -u [warmup steps]\n"
              "  -z [zipf exponent of expert popularity, 0 = uniform]\n"
              "  -E [routing trace to replay (ref/trace_routing.py) instead of the hashed Zipf]\n"
              "  -w [a transformer block: an attention sublayer (32 heads, sequences of 1024) before the MoE]\n"
              "  -p [drift period: steps between reshuffles of the hot set, 0 = static]\n"
              "  -c [chunk: tokens per GEMM] -r [capacity headroom, 0 = auto from zipf]\n"
              "  -n [compute lanes per PE: streams experts are spread over;\n"
              "      default four per GPU divided among the process's PEs]\n"
              "  With several PEs per process (+ppn N, MOE_PPN=N through the\n"
              "  wrapper) the process still has one dispatcher and -t tokens, on\n"
              "  its first PE; the experts spread over all of its PEs.\n"
              "  -L [learning rate] -A (Adam: 3x payload) -T (TF32 GEMMs)\n"
              "  -B (bf16 GEMM operands: the tokens, activations and outputs, and a bf16\n"
              "      copy of each weight the GEMMs read; fp32 accumulation, fp32 master\n"
              "      weights, moments and gradient accumulators. Halves the exchange bytes.\n"
              "      The checksums differ from fp32's and are placement-independent as before.)\n"
              "  -G (serving: the forward alone, no gradient, no update, and no exchange\n"
              "      between an expert's copies, which then cost one state send each.\n"
              "      w2 stays its initial value; out2 is still the oracle across placements.)\n"
              "  -D [capacity factor: cap each expert at f x the mean tokens per\n"
              "      expert and DROP the overflow, the GShard/Switch policy. Bounds\n"
              "      the imbalance without moving anything; the dropped tokens get a\n"
              "      zero output, so the checksums differ from every other mode and\n"
              "      the drop rate is reported.]\n"
              "  -U (one chunked pass per source instead of one per expert: many more\n"
              "      chunks, kept for comparison. Either way the chunk boundaries follow\n"
              "      the routing alone, so the checksums are placement-independent; the\n"
              "      two modes' values differ.)\n"
              "  -R [copies the balancer may keep of one expert: 2 (default) lets it\n"
              "      replicate an expert hotter than a GPU's share; 1 never]\n"
              "  -X (start with the hottest expert of the first hot set replicated,\n"
              "      its second copy on the first PE of process 1 mod processes: the\n"
              "      replicated arithmetic with no balancer, same checksums as without)\n"
              "  -C [checksum frequency, 0 disables] -s [stats frequency, 0 disables]\n"
              "  -f [first LB step] -b [LB frequency]\n"
              "  -a (async LB: overlap the step; needs +LBAsync) -l [wait lag]\n"
              "  -I (CUPTI-measured loads in a window before each LB step, instead of\n"
              "      the experts' token-count estimates)\n"
              "  -S [seed] -P (print final placement)\n"
              "  -F (print every slab and output message: bytes, sent, envelope, landed)\n"
              "  -O (send an expert's outputs after its whole step, not each dispatcher's\n"
              "      as soon as its tokens are done)\n"
              "  -Y (send a step's tokens only after the previous step's outputs are back,\n"
              "      not while the experts still compute it)\n"
              "  -M (send tokens as device messages the expert reads, not as channel puts\n"
              "      the dispatcher writes)\n",
              m->argv[0]);
          CkExit();
      }
    }
    delete m;

    if (top_k < 1 || top_k > MOE_MAX_TOPK || top_k > n_experts)
      CkAbort("-k must be in [1, min(%d, experts)]\n", MOE_MAX_TOPK);
    if (chunk_tokens < 1) CkAbort("-c must be positive\n");
    if (rep_max < 1 || rep_max > K_MAX_COPIES) CkAbort("-R must be 1 to 4\n");
    if (use_attn && (n_tokens % 1024 != 0 || d_model % 32 != 0))
      CkAbort("-w needs -t a multiple of 1024 and -m a multiple of 32\n");
    if (use_bf16 && use_tf32) CkAbort("-B and -T exclude each other\n");
    // Several PEs per process (+ppn) share one GPU, and each PE's dispatcher
    // owns its own lanes -- a stream, a cuBLAS handle with a fixed workspace,
    // and fused-source scratch sized by the dispatcher count. Lanes exist to
    // overlap experts on one device; PEs already do that, so the default
    // keeps about four lanes per GPU rather than four per PE, which at eight
    // PEs would mean 32 handles and gigabytes of scratch on one card.
    const int pes_per_proc = CkNodeSize(CkMyNode());
    if (n_lanes == 0) n_lanes = std::max(1, 4 / pes_per_proc);
    if (n_lanes < 1 || n_lanes > MOE_MAX_LANES)
      CkAbort("-n must be in [1, %d]\n", MOE_MAX_LANES);
    // The split barrier is the application's choice (-a), declared to the
    // runtime here as sph3d, pic2d and leanmd do; no +LBAsync on the command
    // line. (Until 2026-10-31 this asked for the flag and fell back to the
    // unsplit barrier without it, so every "-a" run whose script did not
    // pass +LBAsync ran synchronously.)
    if (async_lb) CkLBSetAsync(true);
    if (async_lb && (lb_wait_lag < 1 || lb_wait_lag >= lb_freq)) {
      CkAbort("-l (%d) must be in [1, lb_freq): the wait must land before "
              "the next AtSyncStart\n", lb_wait_lag);
    }
    // One dispatcher per process, on its first PE. The other PEs of a process
    // hold no tokens; they carry experts, each on its own PE, and provide the
    // compute context those experts run in. So the work per GPU, the slabs
    // and the routing are the same at any +ppn, and the checksums match the
    // one-PE run bit for bit.
    n_disp = CkNumNodes();
    // A replayed trace: the hottest expert and the slab capacity come from the
    // trace itself, p_max being the largest share any expert takes of one
    // dispatcher's tokens in one step (one chunk of n_tokens text tokens).
    std::vector<int32_t> trace_ids;
    double trace_share_max = 0.0, trace_step_max = 0.0;
    int trace_hot = -1;
    if (!trace_path.empty()) {
      loadTrace(trace_path, n_experts, top_k, trace_ids);
      const size_t n = trace_ids.size() / top_k;
      std::vector<long> cnt(n_experts, 0), ccnt(n_experts);
      for (size_t i = 0; i < trace_ids.size(); i++) cnt[trace_ids[i]]++;
      for (int e = 0; e < n_experts; e++) if (trace_hot < 0 || cnt[e] > cnt[trace_hot]) trace_hot = e;
      trace_share_max = (double)cnt[trace_hot] / (double)trace_ids.size();
      for (size_t c = 0; c + (size_t)n_tokens <= n; c += n_tokens) {
        std::fill(ccnt.begin(), ccnt.end(), 0);
        for (size_t i = c * top_k; i < (c + n_tokens) * top_k; i++) ccnt[trace_ids[i]]++;
        for (int e = 0; e < n_experts; e++)
          trace_step_max = std::max(trace_step_max, (double)ccnt[e] / ((double)top_k * n_tokens));
      }
    }
    if (rep_init) {
      if (n_disp < 2)
        CkAbort("-X needs at least two processes: a copy serves the second half of the dispatcher partitions\n");
      std::vector<int> perm;
      hotSetPermutation(0, perm);
      rep_init_expert = trace_path.empty() ? perm[0] : trace_hot;
    }

    // Per-(source, expert) slab capacity. The hottest expert draws a share
    // p_max of every dispatcher's tokens; auto sizes for 1.5x that plus slack.
    double p_max;
    if (!trace_path.empty()) {
      p_max = trace_step_max;
    } else if (zipf_z > 0.0) {
      double H = 0.0;
      for (int r = 0; r < n_experts; r++) H += pow((double)(r + 1), -zipf_z);
      p_max = 1.0 / H;
    } else {
      p_max = 1.0 / n_experts;
    }
    const double avg_src = (double)top_k * n_tokens / n_experts;
    double cap = (headroom > 0.0) ? headroom * avg_src
                                  : 1.5 * p_max * top_k * n_tokens + 64.0;
    cap = std::min(cap, (double)top_k * n_tokens);
    cap_src = std::max(64, (int)((ceil(cap) + 31.0) / 32.0) * 32);

    const size_t wbytes = sizeof(float) * (size_t)d_model * d_ff * 2 *
                          (use_adam ? 3 : 1) +
                          (use_bf16 ? sizeof(moe_bf16) * (size_t)d_model * d_ff * 2 : 0);
    // Inputs and outputs, and a second input slab when the next step's tokens
    // can land while this step computes (send_ahead).
    const size_t slab_bytes = actEsz() * (size_t)n_disp * cap_src *
                              d_model * (send_ahead ? 3 : 2);
    const double gflop = 10.0 * avg_src * n_disp * d_model * d_ff * 1e-9;

    CkPrintf("\n[CUDA mixture-of-experts layer]\n");
    CkPrintf("Experts: %d, d_model %d, d_ff %d, %s%s, up to %d cop%s each\n", n_experts, d_model,
        d_ff, serve_only ? "serving (forward only)" : use_adam ? "Adam" : "SGD",
        use_bf16 ? ", bf16 operands with fp32 master weights" : use_tf32 ? ", TF32" : "", rep_max,
        rep_max == 1 ? "y" : "ies");
    CkPrintf("Tokens: %d per process per step on %d processes of %d PE%s, "
        "top-%d, chunk %d, slab capacity %d per (process, expert), %d compute "
        "lanes per PE%s\n",
        n_tokens, n_disp, pes_per_proc, pes_per_proc == 1 ? "" : "s", top_k,
        chunk_tokens, cap_src, n_lanes,
        fuse_sources ? "" : ", one pass per source");
    if (capacity_factor > 0.0)
      CkPrintf("Capacity factor: %.2f (GShard/Switch token dropping; no state moves)\n",
          capacity_factor);
    if (!trace_path.empty())
      CkPrintf("Routing: trace %s (%zu tokens; hottest expert %.1fx average over the text, "
          "%.1fx in its hottest step; replayed in text order, a chunk of %d tokens per "
          "dispatcher and step)\n", trace_path.c_str(), trace_ids.size() / top_k,
          trace_share_max * n_experts, trace_step_max * n_experts, n_tokens);
    else
      CkPrintf("Routing: zipf %.2f (hottest expert %.1fx average), hot set "
          "reshuffled every %d steps, seed %ld\n", zipf_z, p_max * n_experts,
          drift_period, moe_seed);
    if (use_attn)
      CkPrintf("Block: an attention sublayer (q/k/v/o %d x %d, 32 heads, causal, sequences of 1024, %s) runs on each "
          "dispatcher's tokens before the MoE, trained on its own loss%s\n", d_model, d_model,
          moeAttnKernels(use_bf16 != 0), serve_only ? " (forward only)" : "");
    if (rep_init_expert >= 0)
      CkPrintf("Expert %d starts as two copies (-X): copy 1 on PE %d serves dispatcher partitions 2 and 3 of 4\n",
          rep_init_expert, CkNodeFirst(1 % CkNumNodes()));
    CkPrintf("Per expert: %.1f MB migratable state, %.1f MB slabs, "
        "%.1f GFLOP per step at average load\n", wbytes / 1048576.0,
        slab_bytes / 1048576.0, gflop);
    CkPrintf("Steps: %d (+%d warmup), first LB: %d, LB frequency: %d%s, "
        "loads: %s; next step's tokens sent %s, %s\n\n", n_steps, warmup_steps, first_lb, lb_freq,
        async_lb ? ", async" : "",
        use_instrument ? "CUPTI-measured" : "token-count estimates",
        send_ahead ? "while this step computes" : "after its outputs are back",
        tok_channels ? "as channel puts" : "as device messages");

    disp_proxy = CProxy_Dispatcher::ckNew();
    // (e, 0) for every expert; (e, 1) is inserted when a copy is made. The
    // bounds give the second index room in the element id -- created as
    // (n, 1) alone, the id would have no bit for it and (e, 1) would take
    // another element's id.
    CkArrayOptions opts(n_experts, 1);
    opts.setBounds(n_experts, K_MAX_COPIES);
    expert_proxy = CProxy_Expert::ckNew(opts);
    if (rep_init_expert >= 0)
      expert_proxy(rep_init_expert, 1).insert(CkNodeFirst(1 % CkNumNodes()));
    init_start_time = CkWallTimer();
    disp_proxy.init();
  }

  // Before the experts initialise, PE 0 measures what one partial's transfer
  // costs to a PE on its node and to one on another node (Dispatcher::probe):
  // with copies possible (-R > 1), every expert declares those times, scaled
  // to the balancing interval, as a copy's recurring cost, so the balancer
  // prices a copy against what it buys. Serving exchanges nothing: cost 0.
  void dispReady() {
    if (rep_max > 1 && !serve_only) disp_proxy[0].probe();
    else probeDone(0.0, 0.0);
  }
  void probeDone(double node_s, double remote_s) {
    if (rep_max > 1 && !serve_only)
      CkPrintf("Exchange probe: one partial (%.0f MB) takes %.1f ms to a GPU on this host, %.1f ms to "
               "another host; a copy is priced with these per step\n",
               2.0 * sizeof(float) * (double)d_model * d_ff / 1048576.0, node_s * 1e3, remote_s * 1e3);
    disp_proxy.setXchg(node_s, remote_s);
    expert_proxy.init();
  }

  // The steps start once the experts are initialised and, with token channels,
  // every dispatcher holds every expert's channels.
  bool experts_ready = false;
  int chan_ready_n = 0;
  void expertsReady() { experts_ready = true; maybeStart(); }
  void dispChannelsReady() { chan_ready_n++; maybeStart(); }

  void maybeStart() {
    if (!experts_ready || (tok_channels && chan_ready_n < n_disp)) return;
    CkPrintf("Init time: %.3lf s\n", CkWallTimer() - init_start_time);
    start_time = CkWallTimer();
    window_start_time = start_time;
    // Only the dispatcher branch on each process's first PE runs steps.
    for (int d = 0; d < n_disp; d++) disp_proxy[CkNodeFirst(d)].runStep();
    expert_proxy.runStep();
  }

  // One report per dispatcher per step. A step is in hand once all n_disp
  // have reported; their values are then added in dispatcher order, the same
  // fixed order the old slot reduction summed in, so the checksum is the same
  // bits on every run and every placement. Dispatchers run at most a step
  // apart, so the map holds one or two steps.
  std::map<int, std::vector<double>> pend_out2, pend_drop;
  std::map<int, int> pend_n;
  int disp_done_n = 0;

  void dispStep(int disp, int step, double out2_d, double drop_d) {
    std::vector<double>& o = pend_out2[step];
    std::vector<double>& d = pend_drop[step];
    if (o.empty()) { o.assign(n_disp, 0.0); d.assign(n_disp, 0.0); }
    o[disp] = out2_d;
    d[disp] = drop_d;
    if (++pend_n[step] < n_disp) return;
    double out2 = 0.0, drop = 0.0;
    for (int p = 0; p < n_disp; p++) out2 += o[p];
    for (int p = 0; p < n_disp; p++) drop += d[p];
    pend_out2.erase(step);
    pend_drop.erase(step);
    pend_n.erase(step);
    disp_step++;
    const double now = CkWallTimer();
    if (disp_step == warmup_steps) {
      start_time = now;
      window_start_time = now;
    }
    DispRec r;
    r.out2 = out2;
    r.drop = drop;
    r.t = now;
    disp_q.push_back(r);
    tryPrint();
  }

  // One report per expert per step, from its rank-0 copy, which folds in what
  // its other copy saw. Held per step until all n_experts are in; the weight
  // checksums are added in expert order, a fixed order, as the dispatchers'
  // are. Experts run at most a step or two apart.
  struct ExpPend {
    int n = 0;
    long sum_tok = 0, max_tok = 0;
    std::vector<double> w2e;
    std::vector<long> tokpe;
    std::vector<double> gpupe;
    std::vector<double> pool_busy, pool_tok;   // per PE, the latest report's figure
  };
  std::map<int, ExpPend> pend_exp;
  int exp_done_n = 0;
  double rate_sent = 0.0;

  void expertStep(int e, int step, long n_tot, double w2, int npe, int* pes, long* ns, double* gs,
                  double pool_busy, double pool_tok) {
    ExpPend& p = pend_exp[step];
    if (p.w2e.empty()) {
      p.w2e.assign(n_experts, 0.0);
      p.tokpe.assign(CkNumPes(), 0);
      p.gpupe.assign(CkNumPes(), 0.0);
      p.pool_busy.assign(CkNumPes(), 0.0);
      p.pool_tok.assign(CkNumPes(), 0.0);
    }
    p.sum_tok += n_tot;
    p.max_tok = std::max(p.max_tok, n_tot);
    p.w2e[e] = w2;
    for (int i = 0; i < npe; i++) {
      if (pes[i] < 0) continue;
      p.tokpe[pes[i]] += ns[i];
      p.gpupe[pes[i]] += gs[i];
      if (i == 0) { p.pool_busy[pes[i]] = pool_busy; p.pool_tok[pes[i]] = pool_tok; }
    }
    if (++p.n < n_experts) return;
    // The job-wide rate: every GPU's busy time over every GPU's tokens. One
    // scale for every expert's declared load (MoeCtx::global_spt).
    {
      double busy = 0.0, tok = 0.0;
      for (int q = 0; q < CkNumPes(); q++) { busy += p.pool_busy[q]; tok += p.pool_tok[q]; }
      if (tok > 0.0 && busy > 0.0) {
        const double rate = busy / tok;
        if (rate_sent == 0.0 || std::fabs(rate - rate_sent) > 0.01 * rate_sent) {
          rate_sent = rate;
          disp_proxy.setRate(rate);
        }
      }
    }
    const long sum_tok = p.sum_tok;
    const long max_tok = p.max_tok;
    double w2sum = 0.0;
    for (int i = 0; i < n_experts; i++) w2sum += p.w2e[i];
    exp_step++;

    // With a capacity factor the experts legitimately see fewer tokens than
    // were routed; the pair is checked in tryPrint, where the dispatchers'
    // drop count for the same step is also in hand.
    const long expect = (long)top_k * n_tokens * n_disp;
    if (capacity_factor == 0.0 && sum_tok != expect)
      CkAbort("Token count not conserved at step %d: experts saw %ld of %ld\n",
          exp_step, sum_tok, expect);

    // Per PE, whatever the process count: the experts' spread over PEs.
    const int npes = CkNumPes();
    long max_pe = 0;
    double gsum = 0.0, gmax = 0.0;
    for (int q = 0; q < npes; q++) {
      max_pe = std::max(max_pe, p.tokpe[q]);
      gsum += p.gpupe[q];
      gmax = std::max(gmax, p.gpupe[q]);
    }
    ExpRec r;
    r.tokratio = (double)max_pe / ((double)sum_tok / npes);
    r.gpuratio = gsum > 0.0 ? gmax / (gsum / npes) : 0.0;
    r.gmax_ms = gmax * 1e3;
    r.expratio = (double)max_tok / ((double)sum_tok / n_experts);
    r.w2 = w2sum;
    r.sum_tok = sum_tok;
    exp_q.push_back(r);
    pend_exp.erase(step);
    tryPrint();
  }

  void tryPrint() {
    while (!disp_q.empty() && !exp_q.empty()) {
      const DispRec d = disp_q.front();
      const ExpRec e = exp_q.front();
      disp_q.pop_front();
      exp_q.pop_front();
      // The step both records describe: as many as have been popped so far.
      static int step = 0;
      step++;
      // Conservation, now that both halves of the step are in hand: every
      // routed slot was either seen by an expert or dropped for capacity.
      const long routed = (long)top_k * n_tokens * n_disp;
      if (e.sum_tok + (long)llround(d.drop) != routed)
        CkAbort("Step %d: %ld tokens seen + %ld dropped != %ld routed\n",
            step, e.sum_tok, (long)llround(d.drop), routed);
      if (stats_freq > 0 && step > warmup_steps &&
          (step - warmup_steps) % stats_freq == 0) {
        const double ms = (d.t - window_start_time) / stats_freq * 1e3;
        window_start_time = d.t;
        char dropbuf[64];
        dropbuf[0] = '\0';
        if (capacity_factor > 0.0)
          snprintf(dropbuf, sizeof(dropbuf), ", dropped %.2lf%%",
              100.0 * d.drop / ((double)top_k * n_tokens * n_disp));
        // %.15e on the checksums: a placement-to-placement comparison needs
        // every digit.
        if (checksum_freq > 0 && step % checksum_freq == 0)
          CkPrintf("Step %d: %.3lf ms/step, tokens/PE max/avg %.2lf, "
              "gpu/PE max %.1lf ms max/avg %.2lf (prev), expert max/avg %.2lf%s, "
              "out2 %.15e, w2 %.15e\n", step, ms, e.tokratio, e.gmax_ms,
              e.gpuratio, e.expratio, dropbuf, d.out2, e.w2);
        else
          CkPrintf("Step %d: %.3lf ms/step, tokens/PE max/avg %.2lf, "
              "gpu/PE max %.1lf ms max/avg %.2lf (prev), expert max/avg "
              "%.2lf%s\n", step, ms, e.tokratio, e.gmax_ms, e.gpuratio,
              e.expratio, dropbuf);
      }
    }
  }

  void dispDone() {
    if (++disp_done_n < n_disp) return;
    finish();
  }
  void expertDone(int e) {
    if (++exp_done_n < n_experts) return;
    finish();
  }

  void finish() {
    if (++done_count < 2) return;
    const double total_time = CkWallTimer() - start_time;
    CkPrintf("\nTotal time: %.3lf s\nAverage step time: %.3lf ms\n",
        total_time, total_time / n_steps * 1e3);
    CkExit();
  }
};

// ---------------------------------------------------------------------------

class Dispatcher : public CBase_Dispatcher {
  Dispatcher_SDAG_CODE

 public:
  MoeCtx gpu;
  int my_step = 0;
  int recv_count = 0;
  bool drain_pending = false;
  bool chk_due = false;
  int cur_phase = -1;
  std::vector<int> perm;        // expert at each popularity rank, this phase
  std::vector<double> cdf;      // cumulative popularity by rank
  std::vector<int> fill_;
  std::vector<int> slot_expert; // expert chosen for each (token, j)
  std::vector<int32_t> trace_ids; // -E: the replayed trace, loaded on first use
  MoeAttn attn;                 // -w: the attention sublayer on this dispatcher's tokens
  hapiEvent_t ev_attn;
  std::vector<long long> env_t;   // -F: envelope arrival per (step parity, expert)

  // One step's routing and the buffers that carry it. Two alternate by step
  // parity: with send_ahead, step s+1 is routed, gathered and sent while step
  // s's outputs are still landing and before s is combined.
  struct StepBuf {
    std::vector<int> counts;    // tokens per expert
    std::vector<int> offs;      // slab offsets per expert (n_experts + 1)
    long dropped = 0;           // tokens the capacity factor refused
    void* d_send = nullptr;     // [(n_tokens*k + 1) x d_model], tokens sorted by expert; fp32 or bf16 (actEsz)
    void* d_recv = nullptr;     // same layout, expert outputs; +2 rows, see init()
    int* d_send_idx = nullptr;  // token for each slot of d_send
    int* d_recv_pos = nullptr;  // slot of d_recv for each (token, j)
    int* h_send_idx = nullptr;  // pinned copies
    int* h_recv_pos = nullptr;
    int outstanding = 0;        // this step's token sends not yet complete
  };
  StepBuf sb[2];
  StepBuf& stepBuf(int step) { return sb[step & 1]; }
  int send_step = 0;      // the step issueGather routed; sendTokens sends it
  int sent_through = 0;   // the last step whose tokens are out
  // How many copies of each expert serve this step, and the change an
  // expert announced for the next step (0: none). The copy this dispatcher
  // sends expert e's slab to is copyFor(dispId, mult[e]).
  std::vector<int> mult;
  std::vector<int> pend_mult;
  // Sender copies of the expert copies' token channels, by (expert, copy,
  // step parity); NULL where no copy of that rank sends here. Pointers, so a
  // copy's channels can be destroyed when it retires (an assignment over a
  // channel would leave its registration behind).
  std::vector<CkDeviceChannel*> tok_ch;
  int chans_in = 0;          // rank-0 channel sets held (init handshake)
  int chans_new_in = 0;      // rank > 0 channel sets taken by runStep so far
  int chans_new_needed = 0;  // ... and how many steps so far have needed one

  void* d_x;          // [n_tokens x d_model], fixed for the run; fp32 or bf16 (-B)
  float* d_y;         // [n_tokens x d_model], combined outputs
  double* d_chk;
  double* h_chk;
  // A send is tagged with a stream, and the runtime records "data ready" as
  // that stream's tail at send time. The comm stream's tail may be a landing
  // copy waiting on a remote PE, so the sends carry a stream of their own that
  // waits only on the event recorded right after the gather.
  hapiStream_t send_stream;
  hapiEvent_t ev_gather;
  // Which process's batch this branch owns, and whether it owns one at all:
  // only the first PE of a process dispatches. The other branches exist for
  // the compute context (lanes) the experts on their PE run in.
  int dispId = -1;
  bool active = false;
  // The exchange probe's state (PE 0 and its targets) and its result on every PE.
  char* probe_buf = nullptr;
  char* probe_land = nullptr;
  int probe_target = -1, probe_phase = 0, probe_reps = 0;
  double probe_t0 = 0.0, probe_node_s = 0.0, probe_remote_s = 0.0;
  double xchg_node_s = 0.0, xchg_remote_s = 0.0;   // seconds one partial takes to a copy on this node / another
  // The last balancing round decided nothing (LBManager::LastRoundDecisions):
  // nothing migrates or changes its copies, so the tokens of the steps in its
  // window may go ahead after all. Reset at every balancing step.
  bool lb_round_idle = false;

  // A fixed load balancing object on this PE: the dispatcher's host time and
  // kernels are its own, not unattributed background that DiffusionLB ignores
  // and MetisLB reads as PE overhead.
  Dispatcher() { ckRegisterWithLB(); }

  size_t slots() const { return (size_t)n_tokens * top_k; }
  static int chIdx(int e, int r, int q) { return (e * K_MAX_COPIES + r) * 2 + q; }

  void init() {
    // The experts declare their own loads (token count x measured seconds per
    // token); with instrumentation on, the balancer would overwrite them with
    // CUPTI's measurement, and pay CUPTI's cost every step. -I keeps it.

    dispId = CkMyNode();
    active = (CkMyRank() == 0);

    memset(&gpu, 0, sizeof(gpu));
    gpu.n_lanes = n_lanes;
    gpu.n_iv = 0;
    // See MoeCtx in moe.h for why communication has its own stream.
    hapiCheck(hapiStreamCreateWithPriority(&gpu.comm.stream,
        hapiStreamNonBlocking, -1));
    hapiCheck(hapiEventCreate(&gpu.ref_ev));
    hapiCheck(hapiEventRecord(gpu.ref_ev, gpu.comm.stream));
    hapiCheck(hapiMalloc((void**)&gpu.comm.partials,
        sizeof(double) * MOE_RED_BLOCKS));
    hapiCheck(hapiStreamCreateWithPriority(&send_stream, hapiStreamNonBlocking,
        -1));
    hapiCheck(hapiEventCreateWithFlags(&ev_gather, hapiEventDisableTiming));
    const size_t wsz = (size_t)d_model * d_ff;
    // A lane's buffers are scratch of this process's compute: never sent, never migrated. They come from plain
    // device memory, not the registered pool: with 2+ nodes every pool arena is registered with the fabric and a
    // process may hold at most 4096 MB of them, which the default size (-e 64 -m 2048 -h 8192 -t 8192) exceeded by
    // ~1.8 GB of lane scratch (4 lanes x ~450 MB) on top of its experts' 3.7 GB (2026-10-25).
    for (int l = 0; l < n_lanes; l++) {
      MoeGpu& g = gpu.lanes[l];
      hapiCheck(hapiStreamCreateWithPriority(&g.stream, hapiStreamNonBlocking,
          0));
      g.bf16 = use_bf16 != 0;
      hapiCheck(hapiMallocUnattributed((void**)&g.h,
          actEsz() * (size_t)chunk_tokens * d_ff));
      hapiCheck(hapiMallocUnattributed((void**)&g.dh,
          actEsz() * (size_t)chunk_tokens * d_ff));
      // Three accumulator pairs (moe.h): the four partitions' leaves, folded
      // into the two tree nodes as they are computed (Expert::treeNodes).
      for (int p = 0; p < 3; p++) {
        hapiCheck(hapiMallocUnattributed((void**)&g.acc1[p], sizeof(float) * wsz));
        hapiCheck(hapiMallocUnattributed((void**)&g.acc2[p], sizeof(float) * wsz));
      }
      if (fuse_sources) {
        // Bounded by what one expert can be sent in a step, the same bound its
        // own slabs use. Sized once: growing it would have to free a block the
        // lane's queued kernels may still be reading.
        const size_t frows = (size_t)n_disp * cap_src;
        hapiCheck(hapiMallocUnattributed((void**)&g.fx, actEsz() * frows * d_model));
        hapiCheck(hapiMallocUnattributed((void**)&g.fy, actEsz() * frows * d_model));
      }
      g.workspace_bytes = (size_t)32 << 20;
      hapiCheck(hapiMallocUnattributed(&g.workspace, g.workspace_bytes));
      hapiCheck(hapiMallocUnattributed((void**)&g.partials,
          sizeof(double) * MOE_RED_BLOCKS));
      if (moeBlasCreate(&g, use_tf32 != 0))
        CkAbort("hapiBlasCreate failed on PE %d lane %d\n", CkMyPe(), l);
    }

    if (!active) {
      // Context only; no batch, no steps. Still a member of the group's
      // init reduction.
      contribute(CkCallback(CkReductionTarget(Main, dispReady), main_proxy));
      return;
    }

    const size_t TK = slots();
    const size_t esz = actEsz();
    hapiCheck(hapiMalloc((void**)&d_x, esz * n_tokens * d_model));
    for (StepBuf& b : sb) {
      hapiCheck(hapiMalloc((void**)&b.d_send, esz * (TK + 1) * d_model));
      // TK+2 rows: [TK] absorbs the one-row dummy a zero-token expert sends,
      // [TK+1] is held at zero for tokens dropped by the capacity factor.
      hapiCheck(hapiMalloc((void**)&b.d_recv, esz * (TK + 2) * d_model));
      hapiCheck(hapiMemset((char*)b.d_recv + (TK + 1) * d_model * esz, 0,
          esz * d_model));
      hapiCheck(hapiMalloc((void**)&b.d_send_idx, sizeof(int) * TK));
      hapiCheck(hapiMalloc((void**)&b.d_recv_pos, sizeof(int) * TK));
      hapiCheck(hapiMallocHost((void**)&b.h_send_idx, sizeof(int) * TK));
      hapiCheck(hapiMallocHost((void**)&b.h_recv_pos, sizeof(int) * TK));
      b.counts.assign(n_experts, 0);
      b.offs.assign(n_experts + 1, 0);
    }
    hapiCheck(hapiMalloc((void**)&d_y, sizeof(float) * n_tokens * d_model));
    hapiCheck(hapiMalloc((void**)&d_chk, sizeof(double)));
    hapiCheck(hapiMallocHost((void**)&h_chk, sizeof(double)));

    fill_.assign(n_experts, 0);
    slot_expert.assign(TK, 0);
    mult.assign(n_experts, 1);
    pend_mult.assign(n_experts, 0);
    // -X: the copy exists from the start; it takes effect at step 1 like any
    // announced change, so the channel wait below covers it too.
    if (rep_init_expert >= 0) pend_mult[rep_init_expert] = 2;
    tok_ch.assign(chIdx(n_experts, 0, 0), nullptr);
    LBManagerObj()->AddMigrationDoneFn(this, &Dispatcher::roundDone);

    // Unit-variance inputs, a pure function of (seed, PE, element); under -B
    // drawn in fp32 and rounded once to bf16, so the bf16 run's tokens are the
    // fp32 run's rounded.
    if (use_bf16) {
      float* tmp;
      hapiCheck(hapiMalloc((void**)&tmp, sizeof(float) * (size_t)n_tokens * d_model));
      moeInitUniform(tmp, (size_t)n_tokens * d_model,
          hmix(hmix(moe_seed, 1000), dispId), sqrtf(3.0f), gpu.comm.stream);
      moeCastBf16(tmp, (moe_bf16*)d_x, (size_t)n_tokens * d_model, gpu.comm.stream);
      hapiCheck(hapiStreamSynchronize(gpu.comm.stream));
      hapiCheck(hapiFree(tmp));
    } else {
      moeInitUniform((float*)d_x, (size_t)n_tokens * d_model,
          hmix(hmix(moe_seed, 1000), dispId), sqrtf(3.0f), gpu.comm.stream);
      hapiCheck(hapiStreamSynchronize(gpu.comm.stream));
    }
    if (use_attn) {
      // Lane 0 has the BLAS handle; the gather (send stream) waits for the sublayer through ev_attn.
      moeAttnInit(&gpu.lanes[0], &attn, n_tokens, d_model, 32, 1024, hmix(hmix(moe_seed, 2000), dispId));
      hapiCheck(hapiEventCreateWithFlags(&ev_attn, hapiEventDisableTiming));
    }
    contribute(CkCallback(CkReductionTarget(Main, dispReady), main_proxy));
  }

  // The hot set of a phase: a hashed permutation of the experts, with rank r
  // drawing a share proportional to (r+1)^-z.
  void buildPhase(int phase) {
    hotSetPermutation(phase, perm);
    cdf.resize(n_experts);
    double s = 0.0;
    for (int r = 0; r < n_experts; r++) {
      s += (zipf_z > 0.0) ? pow((double)(r + 1), -zipf_z) : 1.0;
      cdf[r] = s;
    }
    for (int r = 0; r < n_experts; r++) cdf[r] /= s;
  }

  // Route this step's tokens: counts per expert, slab offsets, and the two
  // index maps the gather and combine kernels use. Tokens land in an expert's
  // slab in (token, j) order, which is what makes the expert's reduction order
  // independent of anything but the routing.
  void route(int step) {
    StepBuf& sbuf = stepBuf(step);
    std::vector<int>& counts = sbuf.counts;
    std::vector<int>& offs = sbuf.offs;
    const int E = n_experts, T = n_tokens, K = top_k;
    const int phase = drift_period > 0 ? (step - 1) / drift_period : 0;
    if (phase != cur_phase) {
      cur_phase = phase;
      buildPhase(phase);
    }
    std::fill(counts.begin(), counts.end(), 0);
    if (!trace_path.empty()) {
      if (trace_ids.empty()) loadTrace(trace_path, E, K, trace_ids);
      const size_t n = trace_ids.size() / K;
      const size_t g0 = ((size_t)(step - 1) * n_disp + dispId) * T;
      for (int t = 0; t < T; t++) {
        const int32_t* row = &trace_ids[((g0 + t) % n) * K];
        for (int j = 0; j < K; j++) {
          slot_expert[(size_t)t * K + j] = row[j];
          counts[row[j]]++;
        }
      }
    } else
    for (int t = 0; t < T; t++) {
      int chosen[MOE_MAX_TOPK];
      for (int j = 0; j < K; j++) {
        int e = -1;
        for (int attempt = 0; attempt < 16 && e < 0; attempt++) {
          const uint64_t h = hmix(hmix(hmix(hmix(hmix(moe_seed, step),
              dispId), t), j), attempt);
          int r = (int)(std::upper_bound(cdf.begin(), cdf.end(), u01(h)) -
                        cdf.begin());
          if (r >= E) r = E - 1;
          const int cand = perm[r];
          bool dup = false;
          for (int q = 0; q < j; q++) if (chosen[q] == cand) dup = true;
          if (!dup) e = cand;
        }
        if (e < 0) {
          // Sixteen collisions: walk to the next expert not yet chosen.
          e = (chosen[j - 1] + 1) % E;
          for (;;) {
            bool dup = false;
            for (int q = 0; q < j; q++) if (chosen[q] == e) dup = true;
            if (!dup) break;
            e = (e + 1) % E;
          }
        }
        chosen[j] = e;
        slot_expert[(size_t)t * K + j] = e;
        counts[e]++;
      }
    }
    // Capacity factor (-D), the GShard/Switch policy: an expert accepts at
    // most cap tokens from this dispatcher and the rest are dropped, which
    // bounds the imbalance without moving any state. Dropped tokens keep their
    // routing but take no slab slot; the combine reads them from a row held at
    // zero, so their contribution to the layer output is nothing. Tokens are
    // admitted in token order, so which ones are dropped is still a pure
    // function of the routing and independent of placement.
    long& dropped = sbuf.dropped;
    dropped = 0;
    if (capacity_factor > 0.0) {
      const int cap = std::max(1, (int)ceil(capacity_factor * (double)K *
                                            (double)T / (double)E));
      for (int e = 0; e < E; e++) {
        if (counts[e] > cap) { dropped += counts[e] - cap; counts[e] = cap; }
      }
    }
    offs[0] = 0;
    for (int e = 0; e < E; e++) {
      offs[e + 1] = offs[e] + counts[e];
      if (counts[e] > cap_src)
        CkAbort("PE %d step %d: %d tokens for expert %d exceed the slab "
                "capacity %d; increase -r\n", CkMyPe(), step, counts[e], e,
                cap_src);
    }
    std::fill(fill_.begin(), fill_.end(), 0);
    const size_t TK = slots();
    const int zero_row = (int)TK + 1;   // d_recv row kept at zero; TK is the n=0 slack
    for (size_t i = 0; i < TK; i++) {
      const int e = slot_expert[i];
      if (fill_[e] >= counts[e]) { sbuf.h_recv_pos[i] = zero_row; continue; }
      const int pos = offs[e] + fill_[e]++;
      sbuf.h_send_idx[pos] = (int)(i / K);
      sbuf.h_recv_pos[i] = pos;
    }
  }

  // The copies announced for this step take effect: a new copy that serves
  // this dispatcher must send its channels before the step is routed
  // (chans_new_needed, waited for in runStep); a copy that has gone loses
  // its channels here and is told so, which lets it close them and leave.
  void beginStep() {
    my_step++;
    recv_count = 0;
    if (isLBStep(my_step)) lb_round_idle = false;
    for (int e = 0; e < n_experts; e++) {
      if (pend_mult[e] == 0) continue;
      const int old = mult[e];
      mult[e] = pend_mult[e];
      pend_mult[e] = 0;
      const int cOld = copyFor(dispId, old), cNew = copyFor(dispId, mult[e]);
      if (cOld == cNew) continue;   // the same copy serves this dispatcher either way
      // The copy now serving this dispatcher hands it channels, or a note
      // that it already has them (Expert::refreshChannels): one arrival per
      // change, waited for before this step is routed.
      if (tok_channels) chans_new_needed++;
      TRACE(1, "disp step %d: expert %d has %d copies from this step; copy %d serves this dispatcher",
          my_step, e, mult[e], cNew);
      if (mult[e] < old && cOld == old - 1) {
        // The highest-ranked copy retires: its channels go before it does.
        for (int q = 0; q < 2; q++) {
          delete tok_ch[chIdx(e, cOld, q)];
          tok_ch[chIdx(e, cOld, q)] = nullptr;
        }
        expert_proxy(e, cOld).channelsDropped();
        TRACE(1, "disp step %d: expert %d: copy %d's channels dropped", my_step, e, cOld);
      }
    }
    TRACE(1, "disp step %d begin", my_step);
  }

  // Send the next step's tokens now, before this step's outputs are back.
  // They depend on nothing this step computes -- x is fixed and the routing
  // is a function of the step -- so their exchange overlaps the experts'
  // compute of this step instead of every GPU idling through it at the start
  // of the next (z 0.5, 2 nodes: 21-28 ms of a 98 ms step, GPUs 64% busy).
  // Not across a balancing step: a migration must find no slab of a later
  // step landed or in flight at the expert, as without lookahead, and under
  // async LB migrations can happen until the expert's AtSyncWait lb_wait_lag
  // steps on, so those steps' tokens wait for the previous step's outputs.
  // The same window is where an expert's copies change (an expert acts on a
  // clone or retire decision in the step the balancer released it in), so a
  // change announced in this step's outputs is always routed from the next.
  bool sendAhead() const {
    const int u = my_step + 1;
    if (!send_ahead || u > warmup_steps + n_steps) return false;
    return !inLBWindow(my_step) || lb_round_idle;
  }
  // Every PE's migration-done callback: the round is over here. With no
  // decision anywhere, the window's remaining steps need no pause.
  void roundDone() {
    const int n = LBManagerObj()->LastRoundDecisions();
    if (n == 0) lb_round_idle = true;
    TRACE(1, "disp step %d: balancing round done, %d decisions job-wide%s", my_step, n,
        n == 0 ? "; tokens go ahead again" : "");
  }

  // ---- the exchange probe (PE 0) ----
  size_t probeBytes() const { return 2 * sizeof(float) * (size_t)d_model * d_ff; }   // dW1 and dW2, fp32
  void probe() {
    hapiCheck(hapiMalloc((void**)&probe_buf, probeBytes()));
    probe_phase = 0;
    probeNext();
  }
  // Phase 0: another PE on this physical host (one GPU per process, so a
  // Charm++ node is a process and the host is CmiPhysicalNodeID); phase 1:
  // the first PE of the next host, when there is one. Two transfers each:
  // the first warms the registrations and the paths, the second is the
  // measurement.
  void probeNext() {
    for (;;) {
      if (probe_phase == 0) {
        int t = -1;
        for (int pe = 1; pe < CkNumPes(); pe++)
          if (CmiPhysicalNodeID(pe) == CmiPhysicalNodeID(0)) { t = pe; break; }
        if (t >= 0) { probe_target = t; break; }
        probe_node_s = 0.0;
        probe_phase = 1;
      } else if (probe_phase == 1) {
        int t = -1;
        for (int pe = 1; pe < CkNumPes(); pe++)
          if (CmiPhysicalNodeID(pe) != CmiPhysicalNodeID(0)) { t = pe; break; }
        if (t >= 0) { probe_target = t; break; }
        probe_remote_s = probe_node_s;
        probe_phase = 2;
      } else {
        hapiCheck(hapiFree(probe_buf));
        probe_buf = nullptr;
        main_proxy.probeDone(probe_node_s, probe_remote_s);
        return;
      }
    }
    probe_reps = 0;
    probeSend();
  }
  void probeSend() {
    probe_t0 = CkWallTimer();
    disp_proxy[probe_target].probeLand((int)probeBytes(),
        CkDeviceBuffer(probe_buf, CkCallback(CkIndex_Dispatcher::probeSendDone(), thisProxy[CkMyPe()]),
            gpu.comm.stream));
  }
  void probeSendDone() {}
  void probeAck() {
    const double dt = CkWallTimer() - probe_t0;
    if (++probe_reps < 2) { probeSend(); return; }
    if (probe_phase == 0) probe_node_s = dt; else probe_remote_s = dt;
    probe_phase++;
    probeNext();
  }
  // The target: the partial lands on the comm stream; the ack goes once it has.
  void probeLand(int n, char*& buf, CkDeviceBufferPost* devicePost) {
    if (!probe_land) hapiCheck(hapiMalloc((void**)&probe_land, (size_t)n));
    buf = probe_land;
    devicePost[0].hapi_stream = gpu.comm.stream;
  }
  void probeLand(int n, char* buf) {
    hapiCheck(hapiStreamSynchronize(gpu.comm.stream));
    disp_proxy[0].probeAck();
  }
  void setXchg(double node_s, double remote_s) {
    xchg_node_s = node_s;
    xchg_remote_s = remote_s;
    if (probe_land) { hapiCheck(hapiFree(probe_land)); probe_land = nullptr; }
  }

  // Route step `step` into its parity's buffers and gather its tokens. The
  // buffers were last used by step - 2, which was combined and whose sends
  // all completed (gateDrain) before step - 1 began.
  void issueGather(int step) {
    send_step = step;
    route(step);
    StepBuf& b = stepBuf(step);
    const size_t TK = slots();
    // The gather depends on nothing that lands on the comm stream (x is
    // fixed, the send slab is quiet once its previous step's sends drained),
    // so it runs on the send stream: the sends are then tagged with the very
    // stream that produced their data, and the gather never queues behind a
    // landing. d_recv_pos is read by the combine on the comm stream, which
    // runs only after every output of its step has landed there.
    hapiCheck(hapiMemcpyAsync(b.d_send_idx, b.h_send_idx, sizeof(int) * TK,
        hapiMemcpyHostToDevice, send_stream));
    hapiCheck(hapiMemcpyAsync(b.d_recv_pos, b.h_recv_pos, sizeof(int) * TK,
        hapiMemcpyHostToDevice, send_stream));
    const void* x_src = d_x;
    if (use_attn) {
      // The step's attention sublayer, on lane 0, before its tokens are gathered: trained on its own
      // loss unless serving; its output is what the MoE routes.
      moeAttnStep(&gpu.lanes[0], &attn, d_x, serve_only == 0, (float)(learning_rate / n_tokens));
      hapiCheck(hapiEventRecord(ev_attn, gpu.lanes[0].stream));
      hapiCheck(hapiStreamWaitEvent(send_stream, ev_attn, 0));
      x_src = attn.h;
    }
    moeGather(x_src, b.d_send_idx, b.d_send, (int)TK, d_model, use_bf16 != 0, send_stream);
    hapiCheck(hapiEventRecord(ev_gather, send_stream));
    TRACE(2, "disp step %d gather of step %d issued", my_step, step);
    CkCallback* cb = new CkCallback(CkIndex_Dispatcher::gathered(),
        thisProxy[CkMyPe()]);
    hapiAddCallback(send_stream, cb);
  }

  // One slab per expert, empty ones included (the expert's step waits for one
  // message per dispatcher). len = max(n,1) rows so an empty message still
  // carries a valid buffer; the expert lands it in the unused slab. The slab
  // goes to the copy that serves this dispatcher.
  void sendTokens() {
    const double ts0 = tnow();
    StepBuf& b = stepBuf(send_step);
    const CkCallback done = (send_step & 1)
        ? CkCallback(CkIndex_Dispatcher::sendDoneOdd(), thisProxy[CkMyPe()])
        : CkCallback(CkIndex_Dispatcher::sendDoneEven(), thisProxy[CkMyPe()]);
    hapiCheck(hapiStreamWaitEvent(send_stream, ev_gather, 0));
    if (tok_channels) {
      // Written into the expert's slab by this GPU (same node) or one-sided
      // across nodes: on the node every GPU reading its peers at once shares
      // ~20 GB/s, writing ~50 GB/s (A40 x4, no NVLink). The put's size is the
      // token count; an empty one is a note alone. The tag is the step.
      const int q = send_step % tokParities();
      for (int e = 0; e < n_experts; e++) {
        const int n = b.counts[e];
        CkDeviceChannel* ch = tok_ch[chIdx(e, copyFor(dispId, mult[e]), q)];
        if (ch == nullptr)
          CkAbort("PE %d step %d: no channel to copy %d of expert %d\n", CkMyPe(), send_step,
              copyFor(dispId, mult[e]), e);
        b.outstanding++;
        ch->put((char*)b.d_send + (size_t)b.offs[e] * d_model * actEsz(),
            actEsz() * (size_t)n * d_model, send_stream, done,
            (uint64_t)send_step);
      }
    } else {
      for (int e = 0; e < n_experts; e++) {
        const int n = b.counts[e];
        const int len = (int)(std::max(n, 1) * d_model * actEsz());   // bytes
        expert_proxy(e, copyFor(dispId, mult[e])).receiveTokens(send_step, dispId, n, moeNowNs(),
            CkMyPe(), len,
            (b.outstanding++,
             CkDeviceBuffer((char*)b.d_send + (size_t)b.offs[e] * d_model * actEsz(), done,
                 send_stream)));
      }
    }
    sent_through = send_step;
    // CHARM_MOE_SLABPROBE=1 (fp32, the first two steps): the sum of squares of each
    // expert's slab as sent, to match against what the expert finds in its segments
    // (the diagnostic that found the truncated off-node landings of 2026-10-31).
    static const bool slab_probe = getenv("CHARM_MOE_SLABPROBE") != NULL;
    if (slab_probe && !use_bf16 && send_step <= 2) {
      for (int e = 0; e < n_experts; e++) {
        const int n = b.counts[e];
        if (n <= 0) continue;
        hapiCheck(hapiMemsetAsync(d_chk, 0, sizeof(double), send_stream));
        moeSumSqAdd((const float*)((char*)b.d_send + (size_t)b.offs[e] * d_model * actEsz()),
            (size_t)n * d_model, &gpu.comm, d_chk, send_stream);
        hapiCheck(hapiMemcpyAsync(h_chk, d_chk, sizeof(double), hapiMemcpyDeviceToHost, send_stream));
        hapiCheck(hapiStreamSynchronize(send_stream));
        CkPrintf("[SLAB] disp %d step %d expert %d n=%d sent sumsq %.17g\n", dispId, send_step, e, n, *h_chk);
      }
    }
    TRACE(1, "disp step %d tokens sent%s", my_step,
        send_step > my_step ? " for the next step" : "");
    TRACE(2, "disp step %d issued %d sends in %.1f ms", my_step, n_experts,
        (tnow() - ts0) * 1e3);
  }

  // Post entry method: an expert's outputs land in its segment of the receive
  // slab of their step's parity. An empty message lands in the slack row past
  // the end. Outputs of the next step can arrive before this one is combined:
  // that step's tokens went out early, and an expert can finish it first.
  void receiveOutput(int ref, int e, int n, long long ts, int spe, int mult_next, int& len, char*& buf,
      CkDeviceBufferPost* devicePost) {
    if (flight_log) { env_t.resize(2 * n_experts); env_t[(ref & 1) * n_experts + e] = moeNowNs(); }
    if (ref < my_step || ref > sent_through)
      CkAbort("PE %d: outputs for step %d arrived during step %d (tokens sent "
          "through step %d)\n", CkMyPe(), ref, my_step, sent_through);
    StepBuf& b = stepBuf(ref);
    buf = (n > 0) ? (char*)b.d_recv + (size_t)b.offs[e] * d_model * actEsz()
                  : (char*)b.d_recv + slots() * d_model * actEsz();
    devicePost[0].hapi_stream = gpu.comm.stream;
    TRACE(3, "disp step %d output expert %d n=%d posted", ref, e, n);
  }

  void setRate(double spt) { gpu.global_spt = spt; }

  void takeChannels(int e, int r, int late, CkDeviceChannel c0, CkDeviceChannel c1) {
    if (tok_ch.empty()) tok_ch.assign(chIdx(n_experts, 0, 0), nullptr);
    for (int q = 0; q < 2; q++) {
      delete tok_ch[chIdx(e, r, q)];
      tok_ch[chIdx(e, r, q)] = new CkDeviceChannel(q == 0 ? c0 : c1);
    }
    // One serving copy per expert hands this dispatcher its channels at
    // start-up, whichever copy that is (-X: copy 1 for its dispatchers); a
    // set that arrives later (a new copy's, or an existing copy's block that
    // grew) is what runStep waits for before routing the first step it
    // serves. -X's copy 1 counts as both.
    if (!late && ++chans_in == n_experts) main_proxy.dispChannelsReady();
    if (late || r > 0) thisProxy[CkMyPe()].chanArrived();
  }

  void noteOutput(int e, int n, long long ts, int spe, int mult_next) {
    if (flight_log)
      g_flights.push_back({'O', my_step, spe, CkMyPe(), e, (long)std::max(n, 1) * d_model * (long)actEsz(), ts,
                           env_t[(my_step & 1) * n_experts + e], moeNowNs()});
    TRACE(3, "disp step %d output expert %d n=%d consumed", my_step, e, n);
    if (n != stepBuf(my_step).counts[e])
      CkAbort("PE %d step %d: expert %d returned %d rows for %d tokens\n",
          CkMyPe(), my_step, e, n, stepBuf(my_step).counts[e]);
    if (mult_next != mult[e]) {
      if (copyFor(dispId, mult_next) == copyFor(dispId, mult[e])) {
        // The same copy serves this dispatcher either way (copy 0 for the
        // even ones): the count changes, the routing does not.
        mult[e] = mult_next;
      } else {
        // Announced for the next step, whose tokens must not have gone out
        // already (lookahead is off in the steps where a copy is made or
        // retired).
        if (sent_through > my_step)
          CkAbort("PE %d step %d: expert %d changes to %d copies for step %d, whose tokens are "
                  "already out\n", CkMyPe(), my_step, e, mult_next, my_step + 1);
        pend_mult[e] = mult_next;
      }
    }
  }

  void combine() {
    TRACE(1, "disp step %d outputs in", my_step);
    StepBuf& b = stepBuf(my_step);
    moeCombine(b.d_recv, b.d_recv_pos, d_y, n_tokens, top_k, d_model,
        1.0f / top_k, use_bf16 != 0, gpu.comm.stream);
    chk_due = checksum_freq > 0 && (my_step % checksum_freq == 0);
    if (chk_due) {
      hapiCheck(hapiMemsetAsync(d_chk, 0, sizeof(double), gpu.comm.stream));
      moeSumSqAdd(d_y, (size_t)n_tokens * d_model, &gpu.comm, d_chk, gpu.comm.stream);
      hapiCheck(hapiMemcpyAsync(h_chk, d_chk, sizeof(double),
          hapiMemcpyDeviceToHost, gpu.comm.stream));
    }
    CkCallback* cb = new CkCallback(CkIndex_Dispatcher::combined(),
        thisProxy[CkMyPe()]);
    hapiAddCallback(gpu.comm.stream, cb);
  }

  // This step's send slab is gathered into again two steps on; every send
  // from it must have confirmed completion first.
  void gateDrain() {
    if (stepBuf(my_step).outstanding == 0) thisProxy[CkMyPe()].sendsDrained();
    else drain_pending = true;
  }

  void sendDoneEven() { sendDone(0); }
  void sendDoneOdd() { sendDone(1); }
  void sendDone(int parity) {
    sb[parity].outstanding--;
    if (drain_pending && (my_step & 1) == parity && sb[parity].outstanding == 0) {
      drain_pending = false;
      thisProxy[CkMyPe()].sendsDrained();
    }
  }

  void endStep() {
    TRACE(1, "disp step %d end", my_step);
    if (chk_due) TRACE(2, "disp %d step %d out2 %.17g", dispId, my_step, *h_chk);
    // One slot per PE, summed elementwise: exact whatever order the reduction
    // combines contributions in, so the checksum is the same bits on every
    // run. Main adds the slots in PE order. (A plain sum_double of the values
    // differed in the last digit from run to run, with zero migrations.)
    // Reported point-to-point rather than through a group reduction: only the
    // active branch of each process runs steps, and a group reduction would
    // wait on the branches that never do. Main sums the reports in dispatcher
    // order, which is the same fixed-order sum the reduction's slots gave.
    main_proxy.dispStep(dispId, my_step, chk_due ? *h_chk : 0.0, (double)stepBuf(my_step).dropped);
    if (my_step < warmup_steps + n_steps) {
      thisProxy[CkMyPe()].runStep();
    } else {
      if (flight_log) moeFlightDump();
      main_proxy.dispDone();
    }
  }
};

// ---------------------------------------------------------------------------

class Expert : public CBase_Expert {
  Expert_SDAG_CODE

 public:
  int my_step = 0;
  int recv_count = 0;
  int n_expect = 0;   // slabs this step waits for: one per dispatcher this copy serves
  // 0: receiving slabs (x segments live); 1: kernels issued (y segments
  // live); 2: outputs sent (nothing live but the weights)
  int phase = 0;
  std::vector<int> n_src;   // rows received from each dispatcher, -1 = none
  std::vector<long long> env_t;   // -F: envelope arrival per (step mod 3, dispatcher) (not migrated)
  int n_tot = 0;            // tokens this copy received this step
  int outstanding_sends = 0;
  bool drain_pending = false;
  bool chk_due = false;
  // The outputs went out from compute(), each dispatcher's behind the chunk
  // that finished its tokens (stream_outputs); sendOutputs only drains.
  bool outputs_sent = false;
  int cur_lane = -1;      // see pickLane; not migrated: a new PE picks afresh
  long lane_claim = 0;    // tokens this expert holds against cur_lane
  std::vector<hapiEvent_t> ev_seg;   // per dispatcher: its outputs are in y_seg
  // Receive channels on the input slab segments, [parity * n_disp + dispatcher]
  // (tok_channels), opened for the dispatchers this copy serves. Released once
  // the step's kernels are done with the slab.
  std::vector<CkDeviceChannel> tok_ch;
  std::vector<char> ch_open;
  // Per channel: a put has landed and its note was taken, but the step loop
  // has not consumed it (its slabIn is queued for a later step). Its bytes are
  // in the slab only, so a migration has to carry that segment: the runtime
  // holds a migration for a device message until it is consumed, but slabIn
  // carries no device buffer.
  std::vector<char> slab_held;
  bool arrived_by_pup = false;   // the channels reopen in ckJustMigrated

  // Async LB state: the step was joined at lb_start_step and AtSyncWait is
  // owed lb_wait_lag steps later.
  bool lb_waiting = false;
  int lb_start_step = 0;
  // AtSync or AtSyncWait was called and ResumeFromSync is owed; a resume the
  // element did not ask for (a copy inserted during a step's resume) is
  // ignored.
  bool awaiting_resume = false;
  // This step's resumed() came from the balancer releasing the element: the
  // step in which a clone or retire decision takes effect.
  bool resumed_by_lb = false;

  // Load estimate: seconds per token, an EMA of the measured step time over
  // its token count. Rate-aware for free: a slower device shows up here.
  double spt = 0.0;
  double gpu_last = 0.0;   // measured device time of the last sampled step
  int n_last = 0;
  int step_last = 0;       // the step those events were recorded in
  int adam_t = 0;
  bool ev_pending = false;

  // ---- replication (see partitionOf above) ----
  int nc = 1;                   // copies of this expert now, this one included
  bool replicated = false;      // nc > 1: the other copies serve their blocks of partitions
  int nc_next = 1;              // copies from the next step (mult_next, announced in the outputs)
  int gp_in = 0;                // the SDAG loop over the other copies' block sums
  float* dWown1 = NULL;         // this copy's block sum of the step's gradient (its leaves folded)
  float* dWown2 = NULL;
  float* dWownS1 = NULL;        // scratch for the block's second leaf
  float* dWownS2 = NULL;
  // The other copies' block sums, by peer slot (slotOf) and step parity: the
  // next step's may land while this step's is read.
  float* dWpeer1[K_MAX_COPIES - 1][2] = {};
  float* dWpeer2[K_MAX_COPIES - 1][2] = {};
  bool grad_in_flight = false;  // the block sum's sends have not all completed
  int grad_bufs_done = 0;
  int grad_bufs_expected = 0;
  // The balancer's decision for this round, known to every copy of the group
  // (ckClone / ckRetire on the copy concerned, the notices on the others):
  // a copy of rank decided_rank is made (clone_decided) or retired
  // (retire_decided) at the end of the step the round releases us in.
  bool clone_decided = false;
  bool retire_decided = false;
  int decided_rank = -1;
  bool clone_in_flight = false; // a clone's state send has not completed
  int clone_bufs_done = 0;
  int clone_bufs_expected = 0;
  int clone_to_pe = -1;         // a clone decision waiting for its step
  int clone_rank = -1;
  bool retire_pending = false;  // a retire decision waiting for its step
  bool retiring = false;        // this step is this copy's last
  int mult_next = 1;            // copies serving the next step, told to the dispatchers
  // The other copies' step, by rank: tokens, PE, device time, whether it retires.
  long peer_n[K_MAX_COPIES] = {};
  int peer_pe[K_MAX_COPIES] = {-1, -1, -1, -1};
  double peer_gpu[K_MAX_COPIES] = {};
  bool peer_ret[K_MAX_COPIES] = {};
  bool exchange_free_due = false;   // alone again: the exchange buffers go once the last partial's send is done
  int chans_dropped = 0;        // dispatchers that dropped their channels to this retiring copy
  bool step_ended = false;      // retiring: the last step is over (teardown waits for both)
  bool state_in = false;        // a new copy: the weights (and moments) have landed
  bool moments_in = false;
  bool started = false;         // init() ran (an original) or the state is in (a new copy)

  float* W1;   // [d_model x d_ff], the master weights (fp32 always)
  float* W2;   // [d_ff x d_model]
  // -B: the bf16 copies the GEMMs read, rewritten by every update in its own
  // pass; wb_stale says the master weights are new on this device (init, a
  // migration, a clone's state) and the copies are remade before the next
  // compute. Not migrated: 4 bytes of transfer saved per 2 of cast.
  moe_bf16* W1b;
  moe_bf16* W2b;
  bool wb_stale = false;
  float* m1;   // Adam moments, -A only
  float* v1;
  float* m2;
  float* v2;
  char* x_seg;    // [n_disp x cap_src x d_model] elements of actEsz() bytes, one
                  // segment per dispatcher; two such slabs by step parity with
                  // send_ahead (xSeg)
  char* y_seg;    // same layout, outputs
  double* d_chk;
  double* h_chk;
  hapiEvent_t ev_start;
  hapiEvent_t ev_end;
  // Recorded after the checksum's device-to-host copy. A pack reads h_chk on
  // the host, which the copy may still be writing; pup_device_order only
  // orders DEVICE-mode copies and leaves that host read racing. Losing it
  // costs one expert's slot in the w2 reduction, seen once in eight async
  // runs at Llama size as a deficit of exactly 1/64.
  hapiEvent_t ev_chk;
  hapiEvent_t ev_grad;   // the partial is complete on the lane (replicated)
  // Same reason as the dispatcher's: the lane's tail at send time includes
  // other experts' queued GEMMs, and a landing that waited on it stalled the
  // receiver's comm stream for a remote expert's compute. This stream waits
  // only on ev_end, recorded after this expert's own kernels.
  hapiStream_t send_stream;

  Expert() {
    usesAtSync = true;
    n_src.assign(n_disp, -1);
    W1 = W2 = m1 = v1 = m2 = v2 = NULL;
    W1b = W2b = NULL;
    x_seg = y_seg = NULL;
    d_chk = NULL;
    h_chk = NULL;
  }

  Expert(CkMigrateMessage* m) {
    usesAtSync = true;
    W1 = W2 = m1 = v1 = m2 = v2 = NULL;
    W1b = W2b = NULL;
    x_seg = y_seg = NULL;
    d_chk = NULL;
    h_chk = NULL;
  }

  ~Expert() {
    if (W1 == NULL) return;   // a copy destroyed before its state arrived
    // Under async LB this runs mid-step on a migrating element: kernels may
    // still be in flight on the shared stream. Settle them before freeing.
    const double t0 = tnow();
    std::vector<void*> bufs = {W1, W2, W1b, W2b, m1, v1, m2, v2, x_seg, y_seg, dWown1, dWown2, dWownS1, dWownS2};
    for (int sl = 0; sl < K_MAX_COPIES - 1; sl++)
      for (int q = 0; q < 2; q++) { bufs.push_back(dWpeer1[sl][q]); bufs.push_back(dWpeer2[sl][q]); }
    if (hostSyncMode() || !CkDevicePoolOn()) {
      hapiCheck(hapiStreamSynchronize(lane()->stream));
      TRACE(1, "expert %d.%d dtor step %d phase %d synced after %.3f ms",
          expert(), rank(), my_step, phase, (tnow() - t0) * 1e3);
    } else {
      // Stream-ordered frees instead of a host wait: each block is noted as
      // read on this stream, and the pool parks it until the work queued
      // here retires. The pack copies' own note (migration stream) stays
      // alongside; the pool honours every stream a block was noted on.
      for (void* b : bufs)
        if (b) hapiDevPoolNoteRead(b, lane()->stream);
      if (d_chk) hapiDevPoolNoteRead(d_chk, lane()->stream);
      TRACE(1, "expert %d.%d dtor step %d phase %d stream-ordered", expert(), rank(),
          my_step, phase);
    }
    hapiCheck(hapiEventDestroy(ev_start));
    hapiCheck(hapiEventDestroy(ev_end));
    hapiCheck(hapiEventDestroy(ev_chk));
    hapiCheck(hapiEventDestroy(ev_grad));
    for (hapiEvent_t e : ev_seg) hapiCheck(hapiEventDestroy(e));
    hapiCheck(hapiStreamDestroy(send_stream));
    if (outstanding_sends != 0) {
      // A transport may still be reading a send buffer. Leak rather than pull
      // memory out from under it -- the pic2d convention.
      if (getenv("CHARM_DEBUG_MIGRATE"))
        CkPrintf("[%d] expert %d.%d leaking device buffers at destruction: %d "
                 "sends outstanding\n", CkMyPe(), expert(), rank(), outstanding_sends);
      return;
    }
    for (void* b : bufs)
      if (b) hapiCheck(hapiFree(b));
    hapiCheck(hapiFree(d_chk));
    hapiCheck(hapiFreeHost(h_chk));
  }

  int expert() const { return thisIndex.x; }
  int rank() const { return thisIndex.y; }
  // Whether this copy serves dispatcher d this step.
  int ncopies() const { return nc; }
  // The landing slot of copy r's block sum (the copies other than this one, in rank order).
  int slotOf(int r) const { return r < rank() ? r : r - 1; }
  // The block (copy) that holds partition p under k copies.
  static int blockOfPartition(int p, int k) {
    for (int r = 0; r < k; r++) if (p < blockHi(r, k)) return r;
    return k - 1;
  }
  void setCopies(int k) {
    nc = k;
    replicated = k > 1;
    if (replicated) allocateExchange();
  }
  long peerTokens() const {
    long n = 0;
    for (int r = 0; r < nc; r++) if (r != rank()) n += peer_n[r];
    return n;
  }
  bool serves(int d) const { return copyFor(d, ncopies()) == rank(); }
  int servedCount() const {
    int n = 0;
    for (int d = 0; d < n_disp; d++) if (serves(d)) n++;
    return n;
  }
  CProxyElement_Expert peerAt(int r) { return expert_proxy(expert(), r); }
  CProxyElement_Expert self() { return thisProxy[thisIndex]; }

  // Never cached: a different object on every PE this expert lands on.
  MoeCtx* ctx() { return &disp_proxy.ckLocalBranch()->gpu; }
  // The lane this expert's current (or last) step was issued on.
  MoeGpu* lane() { return &ctx()->lanes[cur_lane >= 0 ? cur_lane : expert() % n_lanes]; }

  // A step's whole chain goes on one lane, behind whatever that lane already
  // holds; with lanes[thisIndex % n_lanes] an expert that happened to share
  // the hot expert's lane queued behind its entire step, so its outputs --
  // which every dispatcher needs -- left at the end and all eight dispatchers
  // started their next step together (large MoE, 2 nodes: the hot expert then
  // waited ~30 ms for its slabs, vs 1-9 ms when its neighbours were on other
  // lanes; job 23192278). The lane with the fewest tokens still in flight goes
  // first. Safe to switch: compute() runs only after the previous step's
  // kernels completed (computeDone), and the results do not depend on lanes.
  void pickLane() {
    MoeCtx* c = ctx();
    int best = 0;
    for (int l = 1; l < n_lanes; l++)
      if (c->lane_pending[l] < c->lane_pending[best]) best = l;
    cur_lane = best;
    lane_claim = std::max(n_tot, 1);
    c->lane_pending[best] += lane_claim;
  }
  void releaseLane() {
    if (lane_claim == 0) return;
    ctx()->lane_pending[cur_lane] -= lane_claim;
    lane_claim = 0;
  }
  hapiStream_t commStream() { return ctx()->comm.stream; }
  size_t segStride() const { return (size_t)cap_src * d_model; }   // elements
  size_t segBytes() const { return segStride() * actEsz(); }
  // The weights the GEMMs read: the bf16 copies under -B, else the masters.
  const void* wRead1() const { return use_bf16 ? (const void*)W1b : (const void*)W1; }
  const void* wRead2() const { return use_bf16 ? (const void*)W2b : (const void*)W2; }
  size_t wsz() const { return (size_t)d_model * d_ff; }
  // What one copy holds beyond the expert's own state: its accumulator and
  // the two landing buffers for the other copy's (moe.h, LBObjData::
  // replicaExtraBytes).
  size_t replicaExtraBytes() const { return serve_only ? 0 : sizeof(float) * wsz() * (4 + 4 * (size_t)(rep_max - 1)); }
  // A step's input slab. With send_ahead a dispatcher sends step s+1 before
  // it has combined step s, so slabs of s+1 and s+2 can arrive while this
  // expert is still in step s; s+2 lands in s's slab, which is free by then:
  // a dispatcher sends s+2 only once it has this expert's step-s output,
  // which leaves behind the copies of s's slab into the lane (fused) or
  // after the whole step (unfused, or -O).
  char* xSeg(int step) {
    return x_seg + (send_ahead ? (size_t)(step & 1) * segBytes() * n_disp : 0);
  }

  void allocate() {
    const size_t seg = segStride() * n_disp;
    hapiCheck(hapiMalloc((void**)&W1, sizeof(float) * wsz()));
    hapiCheck(hapiMalloc((void**)&W2, sizeof(float) * wsz()));
    if (use_adam) {
      hapiCheck(hapiMalloc((void**)&m1, sizeof(float) * wsz()));
      hapiCheck(hapiMalloc((void**)&v1, sizeof(float) * wsz()));
      hapiCheck(hapiMalloc((void**)&m2, sizeof(float) * wsz()));
      hapiCheck(hapiMalloc((void**)&v2, sizeof(float) * wsz()));
    }
    if (use_bf16) {
      hapiCheck(hapiMalloc((void**)&W1b, sizeof(moe_bf16) * wsz()));
      hapiCheck(hapiMalloc((void**)&W2b, sizeof(moe_bf16) * wsz()));
      wb_stale = true;
    }
    hapiCheck(hapiMalloc((void**)&x_seg, actEsz() * seg * (send_ahead ? 2 : 1)));
    hapiCheck(hapiMalloc((void**)&y_seg, actEsz() * seg));
    hapiCheck(hapiMalloc((void**)&d_chk, sizeof(double)));
    hapiCheck(hapiMallocHost((void**)&h_chk, sizeof(double)));
  }

  // The exchange buffers of a replicated copy. Freed, stream-ordered, when the
  // expert is alone again.
  void allocateExchange() {
    if (dWown1 || serve_only) return;   // serving: nothing is exchanged
    hapiCheck(hapiMalloc((void**)&dWown1, sizeof(float) * wsz()));
    hapiCheck(hapiMalloc((void**)&dWown2, sizeof(float) * wsz()));
    hapiCheck(hapiMalloc((void**)&dWownS1, sizeof(float) * wsz()));
    hapiCheck(hapiMalloc((void**)&dWownS2, sizeof(float) * wsz()));
    for (int sl = 0; sl < rep_max - 1; sl++)
      for (int q = 0; q < 2; q++) {
        hapiCheck(hapiMalloc((void**)&dWpeer1[sl][q], sizeof(float) * wsz()));
        hapiCheck(hapiMalloc((void**)&dWpeer2[sl][q], sizeof(float) * wsz()));
      }
  }
  void freeExchange() {
    std::vector<float*> bufs = {dWown1, dWown2, dWownS1, dWownS2};
    for (int sl = 0; sl < K_MAX_COPIES - 1; sl++)
      for (int q = 0; q < 2; q++) { bufs.push_back(dWpeer1[sl][q]); bufs.push_back(dWpeer2[sl][q]); }
    for (float* b : bufs) {
      if (!b) continue;
      if (CkDevicePoolOn() && !hostSyncMode()) hapiDevPoolNoteRead(b, lane()->stream);
      else hapiCheck(hapiStreamSynchronize(lane()->stream));
      hapiCheck(hapiFree(b));
    }
    dWown1 = dWown2 = dWownS1 = dWownS2 = NULL;
    for (int sl = 0; sl < K_MAX_COPIES - 1; sl++)
      for (int q = 0; q < 2; q++) dWpeer1[sl][q] = dWpeer2[sl][q] = NULL;
  }

  void createEvents() {
    hapiCheck(hapiEventCreate(&ev_start));
    hapiCheck(hapiEventCreate(&ev_end));
    hapiCheck(hapiEventCreateWithFlags(&ev_chk, hapiEventDisableTiming));
    hapiCheck(hapiEventCreateWithFlags(&ev_grad, hapiEventDisableTiming));
    hapiCheck(hapiStreamCreateWithPriority(&send_stream, hapiStreamNonBlocking,
        -1));
    ev_seg.resize(n_disp);
    for (hapiEvent_t& e : ev_seg)
      hapiCheck(hapiEventCreateWithFlags(&e, hapiEventDisableTiming));
    ev_pending = false;
  }

  // Token channels for the dispatchers this copy serves, handed to them:
  // at start (late 0, the init handshake) and, when the copies change and
  // this copy's block grows, for the dispatchers it newly serves (late 1,
  // which the dispatcher waits for before routing the step).
  void openChannelsFor(int d, int late) {
    const int np = tokParities();
    for (int q = 0; q < np; q++) {
      CkDeviceChannel& c = tok_ch[q * n_disp + d];
      c = CkDeviceChannel(xSeg(q) + (size_t)d * segBytes(), segBytes(),
                          CkCallback(CkIndex_Expert::slabArrived(NULL), self()));
      c.open();
      ch_open[q * n_disp + d] = 1;
    }
    disp_proxy[CkNodeFirst(d)].takeChannels(expert(), rank(), late, tok_ch[d],
                                            tok_ch[(np - 1) * n_disp + d]);
  }
  void openChannels() {
    if (!tok_channels) return;
    const int np = tokParities();
    tok_ch.resize(np * n_disp);
    ch_open.assign(np * n_disp, 0);
    slab_held.assign(np * n_disp, 0);
    for (int d = 0; d < n_disp; d++)
      if (serves(d)) openChannelsFor(d, 0);
  }
  // When the copies change: every dispatcher this copy newly serves gets
  // its channels, or a note when it has them already from an earlier
  // block (channels are never dropped except by a retiring copy); the
  // dispatcher counts one arrival per change.
  void refreshChannels(int k_old, int k_new) {
    if (!tok_channels) return;
    for (int d = 0; d < n_disp; d++) {
      if (copyFor(d, k_new) != rank() || copyFor(d, k_old) == rank()) continue;
      if (!ch_open[d]) openChannelsFor(d, 1);
      else disp_proxy[CkNodeFirst(d)].chanArrived();
    }
  }

  void init() {
    started = true;
    allocate();
    createEvents();
    // -X: both copies of the chosen expert exist from the start.
    if (expert() == rep_init_expert) setCopies(2);
    openChannels();
    MoeGpu* g = lane();
    // The same seed for both copies: identical weights without a transfer.
    moeInitUniform(W1, wsz(), hmix(hmix(moe_seed, 1), expert()),
        1.0f / sqrtf((float)d_model), g->stream);
    moeInitUniform(W2, wsz(), hmix(hmix(moe_seed, 2), expert()),
        1.0f / sqrtf((float)d_ff), g->stream);
    if (use_adam) {
      hapiCheck(hapiMemsetAsync(m1, 0, sizeof(float) * wsz(), g->stream));
      hapiCheck(hapiMemsetAsync(v1, 0, sizeof(float) * wsz(), g->stream));
      hapiCheck(hapiMemsetAsync(m2, 0, sizeof(float) * wsz(), g->stream));
      hapiCheck(hapiMemsetAsync(v2, 0, sizeof(float) * wsz(), g->stream));
    }
    CkCallback* cb = new CkCallback(CkIndex_Expert::initDone(), self());
    hapiAddCallback(g->stream, cb);
  }

  void initDone() {
    contribute(CkCallback(CkReductionTarget(Main, expertsReady), main_proxy));
  }

  void pup(PUP::er& p) {
    // The migration copies weights and slabs off the device on a stream of
    // its own; settle this PE's stream first so nothing half-written travels
    // and no in-flight kernel loses its buffers to the frees that follow.
    const double t0 = tnow();
    if (p.isPacking()) {
      if (hostSyncMode()) {
        // The original way: stop the host until every kernel queued on the
        // shared stream -- other experts' too -- has finished, so the pack
        // copies read final weights. Up to a full step of GEMMs, host-blocked.
        hapiCheck(hapiStreamSynchronize(lane()->stream));
        TRACE(1, "expert %d.%d pack step %d phase %d synced after %.3f ms",
            expert(), rank(), my_step, phase, (tnow() - t0) * 1e3);
      } else {
        // Order the migration stream's copies behind this stream's queued
        // work on the device instead; the host goes on. Safe because the
        // runtime notes every pool source as read on the migration stream,
        // so the destructor's frees park until the copies retire.
        p.pup_device_order((void*)lane()->stream);
      }
      releaseLane();   // this PE's lane count must not keep a departed expert's tokens
    }
    p | my_step;
    p | recv_count;
    p | n_expect;
    p | phase;
    p | n_src;
    p | n_tot;
    // SDAG state: which whens are outstanding, buffered ordinary messages.
    _sdag_pup(p);
    p | outstanding_sends;
    p | outputs_sent;
    p | drain_pending;
    p | chk_due;
    p | lb_waiting;
    p | lb_start_step;
    p | awaiting_resume;
    p | spt;
    p | gpu_last;
    p | n_last;
    p | step_last;
    p | adam_t;
    // Replication state. The exchange buffers hold nothing live at the join
    // point (the previous step's partial was consumed before this step
    // began, and this step's is produced after the join), so only their
    // existence travels; a partial's send still in flight is held by the
    // runtime (outstanding device sends) like any other.
    p | started;
    p | replicated;
    p | nc;
    p | nc_next;
    p | grad_in_flight;
    p | grad_bufs_done;
    p | clone_in_flight;
    p | clone_bufs_done;
    p | clone_bufs_expected;
    p | clone_to_pe;
    p | clone_rank;
    p | retire_pending;
    p | exchange_free_due;
    p | mult_next;
    // The token channels travel with the element and reopen on its new slab.
    // A put that landed after the last note taken (unseen), or whose note was
    // taken but not yet consumed (slab_held), has its bytes in the slab only,
    // so that segment goes with the pup.
    p | tok_ch;
    p | ch_open;
    p | slab_held;
    std::vector<char> carry(tok_ch.size(), 0);
    if (!p.isUnpacking())   // the sizing pass must count what the packing pass emits
      for (size_t i = 0; i < tok_ch.size(); i++)
        carry[i] = ch_open[i] && (tok_ch[i].unseenBytes() != 0 || slab_held[i]);
    p | carry;

    if (p.isUnpacking()) {
      allocate();
      createEvents();   // a pending timing sample is lost with the old events
      if (replicated) allocateExchange();
      // Control events queue until the channels reopen, in ckJustMigrated: an
      // ordinary entry context once the element's state is in place, not the
      // unpack, which can run inside the device poll's own callback (leanmd's
      // Compute does the same).
      for (size_t i = 0; i < tok_ch.size(); i++)
        if (ch_open[i]) tok_ch[i].arriving();
      arrived_by_pup = !tok_ch.empty();
    }
    for (size_t i = 0; i < tok_ch.size(); i++) {
      if (!carry[i]) continue;
      TRACE(2, "expert %d.%d step %d phase %d %s channel %zu (step parity %zu, dispatcher %zu): %s",
          expert(), rank(), my_step, phase, p.isUnpacking() ? "unpacks" : "packs", i, i / n_disp,
          i % n_disp, slab_held[i] ? "held" : "unseen");
      p(xSeg((int)(i / n_disp)) + (i % n_disp) * segBytes(), segBytes(),
          PUP::PUPMode::DEVICE);
    }
    // The checksum's copy into h_chk is asynchronous; wait for it before the
    // host reads it, or a migration here packs whatever was there before.
    if (p.isPacking() && h_chk && chk_due)
      hapiCheck(hapiEventSynchronize(ev_chk));
    double chk = (h_chk && chk_due) ? *h_chk : 0.0;
    p | chk;
    if (p.isUnpacking()) *h_chk = chk;

    p(W1, wsz(), PUP::PUPMode::DEVICE);
    p(W2, wsz(), PUP::PUPMode::DEVICE);
    if (use_adam) {
      p(m1, wsz(), PUP::PUPMode::DEVICE);
      p(v1, wsz(), PUP::PUPMode::DEVICE);
      p(m2, wsz(), PUP::PUPMode::DEVICE);
      p(v2, wsz(), PUP::PUPMode::DEVICE);
    }
    // What else is live depends on where in the step the move happens. The
    // runtime holds a migration until every device receive addressed to this
    // element has been consumed and every send it issued has completed, so:
    //   phase 0: the slabs consumed so far, which compute will read;
    //   phase 1: the outputs the kernels (now complete) produced, which
    //            sendOutputs will send from the destination;
    //   phase 2: nothing -- the outputs have gone.
    if (phase == 0) {
      for (int s = 0; s < n_disp; s++)
        if (n_src[s] > 0)
          p(xSeg(my_step) + s * segBytes(), (size_t)n_src[s] * d_model * actEsz(),
              PUP::PUPMode::DEVICE);
    } else if (phase == 1) {
      for (int s = 0; s < n_disp; s++)
        if (n_src[s] > 0)
          p(y_seg + s * segBytes(), (size_t)n_src[s] * d_model * actEsz(),
              PUP::PUPMode::DEVICE);
    }
    if (p.isPacking())
      TRACE(1, "expert %d.%d pack end %.3f ms", expert(), rank(), (tnow() - t0) * 1e3);
    else if (p.isUnpacking())
      TRACE(1, "expert %d.%d unpack step %d phase %d %.3f ms", expert(), rank(), my_step,
          phase, (tnow() - t0) * 1e3);
  }

  void beginStep() {
    my_step++;
    recv_count = 0;
    phase = 0;
    n_tot = 0;
    outputs_sent = false;
    resumed_by_lb = false;
    for (int r = 0; r < K_MAX_COPIES; r++) { peer_n[r] = 0; peer_pe[r] = -1; peer_gpu[r] = 0.0; peer_ret[r] = false; }
    n_expect = servedCount();
    std::fill(n_src.begin(), n_src.end(), -1);
  }

  // Post entry method: a dispatcher's slab lands in its own segment. An empty
  // message still lands one row there, which nothing reads.
  void receiveTokens(int ref, int src, int n, long long ts, int spe, int& len, char*& buf,
      CkDeviceBufferPost* devicePost) {
    if (flight_log) { env_t.resize(3 * n_disp); env_t[(ref % 3) * n_disp + src] = moeNowNs(); }
    if (ref < my_step || ref > my_step + (send_ahead ? 2 : 1))
      CkAbort("Expert %d.%d on PE %d: slab for step %d arrived during step %d\n",
          expert(), rank(), CkMyPe(), ref, my_step);
    buf = xSeg(ref) + (size_t)src * segBytes();
    devicePost[0].hapi_stream = commStream();
    TRACE(3, "expert %d.%d step %d slab src %d n=%d posted", expert(), rank(), ref, src,
        n);
  }

  // A note on one of the token channels: find the channel it names and let it
  // take the note. A landed put goes on to the step loop as slabIn, matched by
  // its step; anything else (a resolve, an announce) the channel handles.
  void slabArrived(CkDataMsg* m) {
    CkDeviceChannelEvent ev;
    memcpy(&ev, m->getData(), sizeof(ev));
    int idx = -1;
    for (int i = 0; i < (int)tok_ch.size(); i++)
      if (ch_open[i] && tok_ch[i].id() == ev.channel) { idx = i; break; }
    if (idx < 0)
      CkAbort("Expert %d.%d on PE %d: a note for channel %llx, which is not one of its own\n",
          expert(), rank(), CkMyPe(), (unsigned long long)ev.channel);
    const CkDeviceChannelEvent* e = tok_ch[idx].event(m);
    delete m;
    if (e == NULL) return;
    const int ref = (int)e->tag;
    if (ref % tokParities() != idx / n_disp)
      CkAbort("Expert %d.%d on PE %d: step %d's slab arrived on the channel of parity %d\n",
          expert(), rank(), CkMyPe(), ref, idx / n_disp);
    slab_held[idx] = 1;
    TRACE(2, "expert %d.%d step %d slab for step %d from dispatcher %d landed, %zu bytes",
        expert(), rank(), my_step, ref, idx % n_disp, e->bytes);
    self().slabIn(ref, idx % n_disp, (int)(e->bytes / (actEsz() * d_model)));
  }

  void ckJustMigrated() {
    CBase_Expert::ckJustMigrated();
    if (!arrived_by_pup) return;   // a handoff within the process: the channels are intact
    arrived_by_pup = false;
    int n = 0;
    for (size_t i = 0; i < tok_ch.size(); i++) {
      if (!ch_open[i]) continue;
      tok_ch[i].reopen(xSeg((int)(i / n_disp)) + (i % n_disp) * segBytes());
      n++;
    }
    TRACE(2, "expert %d.%d step %d phase %d reopened %d channels", expert(), rank(), my_step,
        phase, n);
  }

  void noteTokens(int src, int n, long long ts, int spe) {
    TRACE(2, "expert %d.%d step %d consumes slab from dispatcher %d n=%d", expert(), rank(),
        my_step, src, n);
    if (!serves(src))
      CkAbort("Expert %d.%d on PE %d: step %d slab from dispatcher %d, which its other copy serves\n",
          expert(), rank(), CkMyPe(), my_step, src);
    if (tok_channels) slab_held[(my_step % tokParities()) * n_disp + src] = 0;
    if (flight_log && !tok_channels)
      g_flights.push_back({'T', my_step, spe, CkMyPe(), expert(), (long)std::max(n, 1) * d_model * (long)actEsz(),
                           ts, env_t[(my_step % 3) * n_disp + src], moeNowNs()});
    n_src[src] = n;
    TRACE(3, "expert %d.%d step %d slab src %d n=%d consumed", expert(), rank(), my_step,
        src, n);
  }

  // The load-balancing join point; see runStep in moe.ci. Every path ends in
  // exactly one resumed() for this step: sent here directly, or from
  // ResumeFromSync when the runtime releases the element.
  void lbPoint() {
    // Declare the load this step will cost before joining: every slab is in,
    // so the token count is exact, and the rate comes from the steps before.
    sampleTiming();
    n_tot = 0;
    for (int s = 0; s < n_disp; s++) n_tot += std::max(n_src[s], 0);
    // The job-wide rate (MoeCtx::global_spt), so every expert on every GPU
    // declares tokens on one scale; this PE's pooled figure until Main has
    // broadcast one, and the expert's own until this PE has measured.
    const MoeCtx* c = ctx();
    double s_tok = (c->global_spt > 0.0) ? c->global_spt
                 : (c->spt_tok > 0.0) ? c->spt_gpu / c->spt_tok : spt;
    // Diagnostic: a fixed per-token cost, identical on every PE. Every expert
    // here does the same work per token, so this is the exact load ratio; it
    // separates a balancer that cannot use a good signal from a bad signal.
    static const bool flat = getenv("CHARM_MOE_FLATLOAD") != NULL;
    if (flat) s_tok = 1e-5;
    // Declared over the balancing interval, not the step. The balancer holds
    // this against host time it accumulated since the previous balancing
    // step and against that interval's wall time, so a per-step figure is
    // short by the interval length: measured 12 Sep 2026, it read as 17% of
    // the interval when the device was 68-77% busy (nvidia-smi). This step's
    // tokens stand for the interval; every expert scales by the same factor,
    // so the balance is unchanged and only the units are right.
    //
    // A declaration now takes precedence over the CUPTI attribution for the
    // interval (LBObj::gpuDeclared), so under -I, which asks for the measured
    // loads, nothing is declared.
    if (!use_instrument) setObjGPUTime(s_tok * n_tot * stepsSinceLastLB(my_step));
    // This copy's place in its expert: the balancer may ask for a second copy
    // of an expert hotter than a GPU's share, or take one back (ShedLB).
    // ... priced with what one partial's exchange costs per step (the probe at
    // start-up: Dispatcher::probe), over the same interval as the load.
    const Dispatcher* db = disp_proxy.ckLocalBranch();
    const double xsteps = serve_only ? 0.0 : (double)stepsSinceLastLB(my_step);
    setLBReplica(expert(), rank(), nc, rep_max, replicaExtraBytes(),
        db->xchg_node_s * xsteps, db->xchg_remote_s * xsteps);
    if (isLBStep(my_step))
      TRACE(1, "expert %d.%d step %d declares %.6f s: %d tokens x %.4e s/token x %d steps; job rate %.4e; PE pool "
          "%.4f s / %.0f tokens; own last sample %.4f s for %d tokens", expert(), rank(), my_step,
          s_tok * n_tot * stepsSinceLastLB(my_step),
          n_tot, s_tok, stepsSinceLastLB(my_step), c->global_spt, c->spt_gpu, c->spt_tok, gpu_last, n_last);

    if (isLBStep(my_step) && !lb_waiting) {
      if (!async_lb) {
        // Stops here; ResumeFromSync means the strategy and the migrations
        // are over.
        TRACE(1, "expert %d.%d step %d atsync", expert(), rank(), my_step);
        awaiting_resume = true;
        AtSync();
        return;
      }
      // Split barrier: join the step and keep computing while the strategy
      // runs and other experts migrate.
      TRACE(1, "expert %d.%d step %d atsyncstart", expert(), rank(), my_step);
      AtSyncStart();
      lb_waiting = true;
      lb_start_step = my_step;
      self().resumed();
      return;
    }
    if (lb_waiting && my_step >= lb_start_step + lb_wait_lag) {
      // The other half of the split: park until the step is collected.
      // Resumes inline if it already is.
      lb_waiting = false;
      TRACE(1, "expert %d.%d step %d atsyncwait", expert(), rank(), my_step);
      awaiting_resume = true;
      AtSyncWait();
      return;
    }
    self().resumed();
  }

  void ResumeFromSync() {
    if (!awaiting_resume) {
      // A copy inserted while the balancer's step was resuming on this PE
      // gets the resume too; it asked for nothing.
      TRACE(1, "expert %d.%d step %d ignores a resume it did not ask for", expert(), rank(), my_step);
      return;
    }
    awaiting_resume = false;
    resumed_by_lb = true;
    TRACE(1, "expert %d.%d step %d resume", expert(), rank(), my_step);
    self().resumed();
  }

  // ---- the balancer's replication decisions (CkMigratable) ----
  // Recorded here and acted on in the step the balancer releases this copy
  // in (resumed_by_lb), after that step's update: the dispatchers learn the
  // change from that step's outputs and route the next step accordingly.
  void ckClone(int toPe, int rank_) {
    if (rank_ != nc || nc >= rep_max)
      CkAbort("Expert %d.%d on PE %d: asked for copy %d on PE %d while %d of %d copies exist\n",
          expert(), rank(), CkMyPe(), rank_, toPe, nc, rep_max);
    clone_to_pe = toPe;
    clone_rank = rank_;
    clone_decided = true;
    decided_rank = rank_;
    TRACE(1, "expert %d.%d step %d: clone to PE %d as copy %d decided (this copy sends the state)",
        expert(), rank(), my_step, toPe, rank_);
  }
  void ckCloneNotice(int toPe, int rank_) {
    if (rank_ != nc)
      CkAbort("Expert %d.%d on PE %d: told of copy %d while %d copies exist\n", expert(), rank(),
          CkMyPe(), rank_, nc);
    clone_decided = true;
    decided_rank = rank_;
    TRACE(1, "expert %d.%d step %d: copy %d on PE %d decided (notice)", expert(), rank(), my_step, rank_, toPe);
  }
  void ckRetire() {
    if (!replicated || rank() != nc - 1)
      CkAbort("Expert %d.%d on PE %d: asked to retire but it is not the highest of %d copies\n",
          expert(), rank(), CkMyPe(), nc);
    retire_pending = true;
    retire_decided = true;
    decided_rank = rank();
    TRACE(1, "expert %d.%d step %d: retire decided", expert(), rank(), my_step);
  }
  void ckRetireNotice(int rank_) {
    if (rank_ != nc - 1)
      CkAbort("Expert %d.%d on PE %d: told copy %d retires while %d copies exist\n", expert(), rank(),
          CkMyPe(), rank_, nc);
    retire_decided = true;
    decided_rank = rank_;
    TRACE(1, "expert %d.%d step %d: copy %d retires (notice)", expert(), rank(), my_step, rank_);
  }

  // Fold the last recorded step's device time into the per-token rate. Its
  // kernels are complete whenever this runs after that step's outputs were
  // sent. Step 1 is skipped: its time is cuBLAS loading its kernels, and one
  // such sample made every expert's first estimate the same 90 ms whatever
  // its token count.
  void sampleTiming() {
    if (!ev_pending || hapiEventQuery(ev_end) != hapiSuccess) return;
    float ms = 0.0f;
    hapiCheck(hapiEventElapsedTime(&ms, ev_start, ev_end));
    ev_pending = false;
    gpu_last = ms * 1e-3;
    if (n_last > 0 && step_last > 1) {
      // Pool the sample into the PE's figure rather than keeping a per-expert
      // one; see MoeCtx. The decay advances once per step, so the order in
      // which a step's experts report does not weight them differently.
      MoeCtx* c = ctx();
      float s_ms = 0.0f, e_ms = 0.0f;
      hapiCheck(hapiEventElapsedTime(&s_ms, c->ref_ev, ev_start));
      hapiCheck(hapiEventElapsedTime(&e_ms, c->ref_ev, ev_end));
      if (step_last > c->spt_step) {
        c->spt_gpu *= 0.75;
        c->spt_tok *= 0.75;
        c->spt_step = step_last;
      }
      c->spt_gpu += 1e-3 * addBusy(c, s_ms, e_ms);
      c->spt_tok += n_last;
      spt = c->spt_gpu / c->spt_tok;
    }
  }

  // Adds [s, e] (ms on the device clock) to the PE's busy set and returns the
  // part no earlier span -- of any step -- covered. One set across steps: the
  // experts' steps overlap in time under lookahead, and per-step sets counted
  // the same device time once per step. The set is kept disjoint; intervals
  // older than the horizon are dropped.
  static float addBusy(MoeCtx* c, float s, float e) {
    float fresh = e - s;
    for (int i = 0; i < c->n_iv; i++)
      fresh -= std::max(0.0f, std::min(e, c->iv_e[i]) - std::max(s, c->iv_s[i]));
    // Merge: the overlapping intervals fold into [s, e]; the rest stay unless
    // they are older than the horizon.
    int k = 0;
    for (int i = 0; i < c->n_iv; i++) {
      if (c->iv_e[i] < e - MOE_BUSY_HORIZON_MS) continue;
      if (c->iv_e[i] < s || c->iv_s[i] > e) { c->iv_s[k] = c->iv_s[i]; c->iv_e[k] = c->iv_e[i]; k++; }
      else { s = std::min(s, c->iv_s[i]); e = std::max(e, c->iv_e[i]); }
    }
    if (k < MOE_BUSY_IVS) { c->iv_s[k] = s; c->iv_e[k] = e; k++; }
    c->n_iv = k;
    return std::max(fresh, 0.0f);
  }

  // Tokens this copy holds for partition p this step.
  int partitionTokens(int p) const {
    int n = 0;
    for (int s = 0; s < n_disp; s++)
      if (partitionOf(s) == p) n += std::max(n_src[s], 0);
    return n;
  }

  // One run: the slabs of partition p's dispatchers, in dispatcher order,
  // through the chunked step into the accumulators dW1/dW2 (the run's first
  // chunk overwrites them). The outputs go back into each dispatcher's
  // segment and, with stream_outputs, each dispatcher's leave as soon as its
  // tokens are done. Returns the chunks issued.
  // One chunked run: the forward alone when serving, else the forward,
  // backward and gradient into dW1/dW2.
  void chunksOf(MoeGpu* g, const void* x, void* y, int n, float* dW1, float* dW2, bool zeroFirst) {
    if (serve_only)
      moeExpertForward(g, wRead1(), wRead2(), x, y, n, d_model, d_ff, chunk_tokens);
    else
      moeExpertChunks(g, wRead1(), wRead2(), x, y, n, d_model, d_ff, chunk_tokens, dW1, dW2,
          zeroFirst);
  }

  int runPartition(MoeGpu* g, int p, float* dW1, float* dW2) {
    const int n_run = partitionTokens(p);
    if (n_run <= 0) return 0;
    int chunks = 0;
    const size_t esz = actEsz();
    char* fx = (char*)g->fx;
    char* fy = (char*)g->fy;
    if (fuse_sources) {
      // One pass over the partition's tokens instead of one per source.
      // The gradient GEMMs of a chunk read and rewrite both accumulators
      // whatever the chunk holds, so a run's cost is set by the number of
      // chunks, and unfused that is at least one per (source, expert) pair:
      // a typical expert gets a few hundred tokens from each dispatcher.
      // Concatenating in dispatcher order keeps the chunk boundaries a
      // function of the routing alone, so the reduction order is still
      // placement-independent.
      size_t off = 0;
      for (int s = 0; s < n_disp; s++) {
        if (partitionOf(s) != p || n_src[s] <= 0) continue;
        const size_t n = (size_t)n_src[s] * d_model;
        hapiCheck(hapiMemcpyAsync(fx + off * esz, xSeg(my_step) + s * segBytes(),
            esz * n, hapiMemcpyDeviceToDevice, g->stream));
        off += n;
      }
      chunks = (n_run + chunk_tokens - 1) / chunk_tokens;
      if (stream_outputs) {
        // The same chunks, issued up to the one that holds each dispatcher's
        // last token; that dispatcher's outputs are final there, so they go
        // back into its segment and its send leaves while the rest of the
        // run computes. A chunk range that starts at a multiple of the chunk
        // size is the same sequence of kernels as one call over all n_run,
        // so the results are bit for bit those of the single call. Sending
        // all of them after the last chunk put every dispatcher's next step
        // behind the busiest expert's whole step plus the slowest of its
        // eight 14 MB sends at once (large MoE, 2 nodes: the busiest GPU idle
        // ~70 ms of a 190 ms step, job 23175260).
        int done = 0, tok = 0;
        for (int s = 0; s < n_disp; s++) {
          if (partitionOf(s) != p) continue;
          const int ns = std::max(n_src[s], 0);
          if (ns > 0) {
            const int end = tok + ns;
            const int upto = (end >= n_run) ? n_run
                : std::min(n_run, (end + chunk_tokens - 1) / chunk_tokens * chunk_tokens);
            if (upto > done) {
              chunksOf(g, fx + (size_t)done * d_model * esz, fy + (size_t)done * d_model * esz,
                  upto - done, dW1, dW2, done == 0);
              done = upto;
            }
            hapiCheck(hapiMemcpyAsync(y_seg + s * segBytes(),
                fy + (size_t)tok * d_model * esz, esz * (size_t)ns * d_model,
                hapiMemcpyDeviceToDevice, g->stream));
            tok = end;
          }
          hapiCheck(hapiEventRecord(ev_seg[s], g->stream));
        }
      } else {
        chunksOf(g, fx, fy, n_run, dW1, dW2, true);
        // Back into the per-source segments, so the sends and both pup phases
        // are unchanged. Queued before ev_end, which orders a pack behind it.
        off = 0;
        for (int s = 0; s < n_disp; s++) {
          if (partitionOf(s) != p || n_src[s] <= 0) continue;
          const size_t n = (size_t)n_src[s] * d_model;
          hapiCheck(hapiMemcpyAsync(y_seg + s * segBytes(), fy + off * esz,
              esz * n, hapiMemcpyDeviceToDevice, g->stream));
          off += n;
        }
      }
    } else {
      bool first = true;
      for (int s = 0; s < n_disp; s++) {
        if (partitionOf(s) != p || n_src[s] <= 0) continue;
        chunks += (n_src[s] + chunk_tokens - 1) / chunk_tokens;
        chunksOf(g, xSeg(my_step) + s * segBytes(), y_seg + s * segBytes(), n_src[s], dW1, dW2,
            first);
        first = false;
      }
    }
    return chunks;
  }

  // One node of the gradient's tree: partitions p0 and p0 + 1 (an aligned
  // pair; a block of one partition when k = 4) into the accumulator pair
  // (A1, A2), with (S1, S2) as the second leaf's scratch, their sum folded
  // into (A1, A2). o1/o2: the node, or NULL when neither partition had
  // tokens. A copy with a block of one partition (k = 4) folds nothing.
  int treeNode(MoeGpu* g, int p0, float* A1, float* A2, float* S1, float* S2, float*& o1, float*& o2) {
    const bool pair = (p0 + 1 < K_PART) && (blockHi(rank(), ncopies()) - blockLo(rank(), ncopies()) >= 2 || !replicated);
    const int na = partitionTokens(p0), nb = pair ? partitionTokens(p0 + 1) : 0;
    int chunks = 0;
    o1 = o2 = NULL;
    if (na > 0) {
      chunks += runPartition(g, p0, A1, A2);
      o1 = A1; o2 = A2;
    }
    if (nb > 0) {
      float* l1 = na > 0 ? S1 : A1;
      float* l2 = na > 0 ? S2 : A2;
      chunks += runPartition(g, p0 + 1, l1, l2);
      if (na > 0) { moeAddInto(g, A1, S1, wsz()); moeAddInto(g, A2, S2, wsz()); }
      o1 = A1; o2 = A2;
    }
    return chunks;
  }

  // The step's update from the two tree nodes (NULL: no tokens), then the
  // weight checksum when it is due.
  void applyUpdate(MoeGpu* g, const float* A1, const float* A2, const float* B1,
      const float* B2, int n_all) {
    if (n_all > 0) {
      const float inv_n = 1.0f / n_all;
      if (use_adam)
        moeAdamApply(g, W1, m1, v1, W2, m2, v2, W1b, W2b, A1, A2, B1, B2, d_model, d_ff,
            (float)learning_rate, inv_n, ++adam_t);
      else
        moeApplySGD(g, W1, W2, W1b, W2b, A1, A2, B1, B2, d_model, d_ff,
            (float)learning_rate * inv_n);
    }
    if (chk_due) {
      hapiCheck(hapiMemsetAsync(d_chk, 0, sizeof(double), g->stream));
      moeSumSqAdd(W1, wsz(), g, d_chk, g->stream);
      moeSumSqAdd(W2, wsz(), g, d_chk, g->stream);
      hapiCheck(hapiMemcpyAsync(h_chk, d_chk, sizeof(double),
          hapiMemcpyDeviceToHost, g->stream));
      hapiCheck(hapiEventRecord(ev_chk, g->stream));
    }
  }

  void compute() {
    phase = 1;
    n_tot = 0;
    for (int s = 0; s < n_disp; s++) n_tot += std::max(n_src[s], 0);
    pickLane();
    MoeGpu* g = lane();

    sampleTiming();
    if (exchange_free_due && !grad_in_flight) {
      freeExchange();
      exchange_free_due = false;
    }

    // The copies serving the next step. A clone decision takes effect at the
    // first step inside the balancing window after it arrived, after this
    // step's update (executeClone, from endStep): under the split barrier
    // that is up to lag steps before the round's resume, and the new copy
    // registers into the next round (its PE's barrier for this one has
    // fired, or the strategy could not have run). A retire takes effect in
    // the step the balancer released this copy in, at its end.
    // Every copy of the group knows the round's decision (ckClone / ckRetire
    // on the copy concerned, the notices on the others) before the round
    // releases it, so all announce the same count in this step's outputs and
    // act at its end: the new copy starts, or the retired one ends, at the
    // next step.
    nc_next = nc;
    if (resumed_by_lb && clone_decided) nc_next = nc + 1;
    if (resumed_by_lb && retire_decided) {
      nc_next = nc - 1;
      if (rank() == decided_rank) retiring = true;
    }
    mult_next = nc_next;

    // Only the primary copy reports the weight checksum; every copy has the
    // bits, and at trace level 2 each prints its own, so they can be compared.
    chk_due = checksum_freq > 0 && (my_step % checksum_freq == 0) && (rank() == 0 || traceLevel() >= 2);
    TRACE(2, "expert %d.%d step %d compute n=%d%s", expert(), rank(), my_step, n_tot,
        replicated ? " (one of two copies)" : "");
    const double ti0 = tnow();
    int chunks = 0;
    if (use_bf16 && wb_stale) {
      // The master weights are new on this device (init, a migration, a
      // clone's state, all complete before this entry runs): remake the bf16
      // copies on the lane, ahead of the GEMMs that read them. Every update
      // from here on rewrites the copies in its own pass.
      moeCastBf16(W1, W1b, wsz(), g->stream);
      moeCastBf16(W2, W2b, wsz(), g->stream);
      wb_stale = false;
    }
    static const bool slab_probe = getenv("CHARM_MOE_SLABPROBE") != NULL;
    if (slab_probe && !use_bf16 && my_step <= 2 && send_ahead) {
      for (int sd = 0; sd < n_disp; sd++) {
        if (n_src[sd] <= 0) continue;
        double sums[2];
        for (int q = 0; q < 2; q++) {
          hapiCheck(hapiMemsetAsync(d_chk, 0, sizeof(double), g->stream));
          moeSumSqAdd((const float*)(x_seg + (size_t)q * segBytes() * n_disp + (size_t)sd * segBytes()),
              (size_t)n_src[sd] * d_model, g, d_chk, g->stream);
          hapiCheck(hapiMemcpyAsync(h_chk, d_chk, sizeof(double), hapiMemcpyDeviceToHost, g->stream));
          hapiCheck(hapiStreamSynchronize(g->stream));
          sums[q] = *h_chk;
        }
        const uintptr_t a1 = (uintptr_t)(x_seg + segBytes() * n_disp + (size_t)sd * segBytes());
        const uintptr_t unit = (uintptr_t)256 << 20;
        CkPrintf("[SLAB] expert %d step %d disp %d n=%d parity0 sumsq %.17g parity1 sumsq %.17g (reads parity %d) "
                 "parity1 seg at %#lx, %zu MiB to the next 256 MiB boundary, %zu MiB long\n",
            expert(), my_step, sd, n_src[sd], sums[0], sums[1], my_step & 1, (unsigned long)a1,
            (size_t)((unit - (a1 % unit)) >> 20), (size_t)(((size_t)n_src[sd] * d_model * actEsz()) >> 20));
      }
    }
    hapiCheck(hapiEventRecord(ev_start, g->stream));
    if (serve_only) {
      // Serving: the forward of every partition here, or of this copy's
      // block of them; no gradient, no update, no exchange. The weight
      // checksum is what it was. (A copy's dispatchers are its block's, so
      // the other partitions hold no tokens.)
      for (int p = 0; p < K_PART; p++)
        if (partitionTokens(p) > 0) chunks += runPartition(g, p, NULL, NULL);
      applyUpdate(g, NULL, NULL, NULL, NULL, 0);
    } else if (!replicated) {
      // Both partitions here, each into its own lane accumulator, and the
      // update from the two: the same operands and the same add as two
      // copies exchanging partials, so the bits agree either way.
      // The fixed tree over the four partitions: node (P0 + P1) into pair 0
      // (pair 1 as the second leaf's scratch), node (P2 + P3) into pair 1
      // (pair 2 as scratch); the root add is the SGD kernel's.
      float *a1, *a2, *b1, *b2;
      chunks += treeNode(g, 0, g->acc1[0], g->acc2[0], g->acc1[1], g->acc2[1], a1, a2);
      chunks += treeNode(g, 2, g->acc1[1], g->acc2[1], g->acc1[2], g->acc2[2], b1, b2);
      applyUpdate(g, a1, a2, b1, b2, n_tot);
    } else {
      // This copy's block of partitions, folded into its own accumulator; the
      // update follows the exchange of block sums (applyReplicated).
      float *o1, *o2;
      chunks += treeNode(g, blockLo(rank(), ncopies()), dWown1, dWown2, dWownS1, dWownS2, o1, o2);
      hapiCheck(hapiEventRecord(ev_grad, g->stream));
    }
    TRACE(2, "expert %d.%d step %d issued %d chunks in %.1f ms", expert(), rank(),
        my_step, chunks, (tnow() - ti0) * 1e3);
    hapiCheck(hapiEventRecord(ev_end, g->stream));
    if (n_tot > 0) {
      ev_pending = true;
      n_last = n_tot;
      step_last = my_step;
    }
    if (n_tot > 0 || chk_due) {
      CkCallback* cb = new CkCallback(CkIndex_Expert::computeDone(), self());
      hapiAddCallback(g->stream, cb);
    } else {
      self().computeDone();
    }
    // Each send is tagged with the send stream after a wait on its own
    // dispatcher's event, so the runtime takes that point as "data ready":
    // across nodes it holds the envelope until then, on the node the receiver
    // polls the flag. Nothing waits on the host.
    if (n_tot > 0 && fuse_sources && stream_outputs) {
      phase = 2;   // a migration is held until every send has completed
      for (int s = 0; s < n_disp; s++) {
        if (!serves(s)) continue;
        hapiCheck(hapiStreamWaitEvent(send_stream, ev_seg[s], 0));
        sendOutput(s);
      }
      outputs_sent = true;
      TRACE(2, "expert %d.%d step %d outputs queued behind their chunks", expert(), rank(), my_step);
    }
    if (replicated) {
      if (serve_only) sendPeerStats();
      else sendGradPartial();
    }
  }

  // Serving: the other copy gets this step's figures alone (tokens, PE,
  // device time, whether this copy retires); nothing is in flight after.
  void sendPeerStats() {
    for (int r = 0; r < nc; r++)
      if (r != rank()) peerAt(r).peerStats(my_step, rank(), n_tot, CkMyPe(), gpu_last, retiring ? 1 : 0);
  }

  // This copy's partial to the other copy, behind its own kernels on the
  // device; dWown is rewritten only after gradSent (runStep). Sent whole even
  // when this copy saw no tokens -- n_mine 0 tells the other copy to add
  // nothing -- so the message shape never changes.
  void sendGradPartial() {
    hapiCheck(hapiStreamWaitEvent(send_stream, ev_grad, 0));
    grad_in_flight = true;
    grad_bufs_done = 0;
    grad_bufs_expected = 2 * (nc - 1);
    const CkCallback done(CkIndex_Expert::gradBufDone(), self());
    for (int r = 0; r < nc; r++) {
      if (r == rank()) continue;
      peerAt(r).gradPartial(my_step, rank(), n_tot, CkMyPe(), gpu_last, retiring ? 1 : 0, (int)wsz(),
          CkDeviceBuffer(dWown1, done, send_stream), CkDeviceBuffer(dWown2, done, send_stream));
    }
    TRACE(2, "expert %d.%d step %d block sum of %d tokens sent to %d other cop%s%s", expert(), rank(),
        my_step, n_tot, nc - 1, nc - 1 == 1 ? "y" : "ies", retiring ? " (retiring)" : "");
  }
  void gradBufDone() {
    if (++grad_bufs_done < grad_bufs_expected) return;
    if (retiring) {
      // No next step will wait for gradSent; the teardown does instead.
      grad_in_flight = false;
      maybeFinishRetire();
    } else {
      self().gradSent();
    }
  }

  // Post entry method: the other copy's partial lands in this step parity's
  // landing buffers on the comm stream.
  void gradPartial(int ref, int from_rank, int n_mine, int pe, double gpu_s, int retiring_, int& n,
      float*& g1, float*& g2, CkDeviceBufferPost* devicePost) {
    // A new copy: the other copies' block sums for its first step can land before its state
    // (their step ends while the state is still in flight from another PE; on Frontier the
    // partial beat a 384 MB state). The landing buffers do not depend on the copy count, so
    // they are made here; the count is set when the state is in (startAsCopy), and the SDAG
    // loop takes the partial when its step comes. Nothing else can arrive early: a copy's
    // next block sum waits for this copy's.
    if (!started) {
      if (W1 == NULL) { allocate(); createEvents(); }
      allocateExchange();
    }
    if ((started && !replicated) || from_rank == rank() || from_rank >= rep_max ||
        dWpeer1[slotOf(from_rank)][ref & 1] == NULL)
      CkAbort("Expert %d.%d on PE %d: a block sum from copy %d for step %d but this copy has %d copies\n",
          expert(), rank(), CkMyPe(), from_rank, ref, nc);
    if (started && (ref < my_step || ref > my_step + 1))
      CkAbort("Expert %d.%d on PE %d: block sum for step %d arrived during step %d\n",
          expert(), rank(), CkMyPe(), ref, my_step);
    g1 = dWpeer1[slotOf(from_rank)][ref & 1];
    g2 = dWpeer2[slotOf(from_rank)][ref & 1];
    devicePost[0].hapi_stream = commStream();
    devicePost[1].hapi_stream = commStream();
    TRACE(3, "expert %d.%d step %d partial for step %d posted", expert(), rank(), my_step, ref);
  }

  void noteGrad(int from_rank, int n_mine, int pe, double gpu_s, int retiring_) {
    peer_n[from_rank] = n_mine;
    peer_pe[from_rank] = pe;
    peer_gpu[from_rank] = gpu_s;
    peer_ret[from_rank] = retiring_ != 0;
    TRACE(2, "expert %d.%d step %d block sum of %d tokens from copy %d in%s", expert(), rank(), my_step,
        n_mine, from_rank, peer_ret[from_rank] ? " (it retires)" : "");
  }

  // Copy r's block sum this step: its own, or the landed one; NULL when it saw no tokens.
  float* blockSum1(int r) { return r == rank() ? (n_tot > 0 ? dWown1 : NULL) : (peer_n[r] > 0 ? dWpeer1[slotOf(r)][my_step & 1] : NULL); }
  float* blockSum2(int r) { return r == rank() ? (n_tot > 0 ? dWown2 : NULL) : (peer_n[r] > 0 ? dWpeer2[slotOf(r)][my_step & 1] : NULL); }
  // One node of the tree, partitions pA and pA + 1, from the block sums: the
  // one block that holds both, or the two blocks' sums added. The add goes
  // into a landed buffer, never this copy's own (a send may still read it);
  // the order of the two operands does not matter, fp32 addition is
  // commutative, and the unreplicated expert adds the same two leaves.
  void treeNodeFromBlocks(MoeGpu* g, int pA, float*& o1, float*& o2) {
    const int rA = blockOfPartition(pA, nc), rB = blockOfPartition(pA + 1, nc);
    float *a1 = blockSum1(rA), *a2 = blockSum2(rA);
    if (rA == rB) { o1 = a1; o2 = a2; return; }
    float *b1 = blockSum1(rB), *b2 = blockSum2(rB);
    if (!a1) { o1 = b1; o2 = b2; return; }
    if (!b1) { o1 = a1; o2 = a2; return; }
    float *d1 = a1, *d2 = a2, *s1 = b1, *s2 = b2;
    if (rA == rank()) { d1 = b1; d2 = b2; s1 = a1; s2 = a2; }
    moeAddInto(g, d1, s1, wsz());
    moeAddInto(g, d2, s2, wsz());
    o1 = d1; o2 = d2;
  }

  // The update of a replicated copy: the two tree nodes assembled from its
  // own block sum and the landed ones, then the same SGD as every copy's.
  void applyReplicated() {
    MoeGpu* g = lane();
    float *a1, *a2, *b1, *b2;
    treeNodeFromBlocks(g, 0, a1, a2);
    treeNodeFromBlocks(g, 2, b1, b2);
    applyUpdate(g, a1, a2, b1, b2, (int)(n_tot + peerTokens()));
    hapiCheck(hapiEventRecord(ev_end, g->stream));   // the clone's state send orders behind the update
    CkCallback* cb = new CkCallback(CkIndex_Expert::applyDone(), self());
    hapiAddCallback(g->stream, cb);
  }

  void sendOutput(int s) {
    const int n = std::max(n_src[s], 0);
    const int len = (int)(std::max(n, 1) * d_model * actEsz());   // bytes
    disp_proxy[CkNodeFirst(s)].receiveOutput(my_step, expert(), n, moeNowNs(), CkMyPe(), mult_next, len,
        (outstanding_sends++,
         CkDeviceBuffer(y_seg + s * segBytes(),
             CkCallback(CkIndex_Expert::sendDone(), self()),
             send_stream)));
  }

  void sendOutputs() {
    releaseLane();   // the step's kernels are complete (computeDone)
    if (tok_channels) {
      // The kernels that read this step's slab are done: the dispatchers may
      // put into it again (the step after next, or the next without send_ahead).
      // Only the channels that were put into this step: the other copy's
      // dispatchers never sent here.
      const int q = my_step % tokParities();
      for (int d = 0; d < n_disp; d++)
        if (n_src[d] >= 0) tok_ch[q * n_disp + d].release();
    }
    if (!outputs_sent) {
      phase = 2;
      const double ts0 = tnow();
      if (n_tot > 0) hapiCheck(hapiStreamWaitEvent(send_stream, ev_end, 0));
      for (int s = 0; s < n_disp; s++)
        if (serves(s)) sendOutput(s);
      TRACE(2, "expert %d.%d step %d outputs sent", expert(), rank(), my_step);
      TRACE(2, "expert %d.%d step %d issued %d sends in %.1f ms", expert(), rank(),
          my_step, servedCount(), (tnow() - ts0) * 1e3);
    }
    gateDrain();
  }

  // The next step's kernels rewrite the output segments, and under async LB a
  // migration's pup frees them; neither may happen over a live transport read.
  void gateDrain() {
    if (outstanding_sends == 0) self().sendsDrained();
    else drain_pending = true;
  }

  void sendDone() {
    outstanding_sends--;
    if (drain_pending && outstanding_sends == 0) {
      drain_pending = false;
      self().sendsDrained();
    }
  }

  // ---- making a copy ----
  // After this step's update: insert (e, 1) where the balancer said, send it
  // the weights (and moments) behind the update on the device, and serve
  // only partition 0 from the next step. The weights are read by the send
  // until cloneSent, which the next step's compute waits for.
  void executeClone() {
    const int toPe = clone_to_pe, r = clone_rank;
    clone_to_pe = -1;
    clone_rank = -1;
    TRACE(1, "expert %d.%d step %d: copy %d inserted on PE %d, state sent", expert(), rank(), my_step,
        r, toPe);
    expert_proxy(expert(), r).insert(toPe);
    hapiCheck(hapiStreamWaitEvent(send_stream, ev_end, 0));
    clone_in_flight = true;
    clone_bufs_done = 0;
    clone_bufs_expected = use_adam ? 6 : 2;
    const CkCallback done(CkIndex_Expert::cloneBufDone(), self());
    expert_proxy(expert(), r).takeState(my_step + 1, adam_t, spt, nc_next, (int)wsz(),
        CkDeviceBuffer(W1, done, send_stream), CkDeviceBuffer(W2, done, send_stream));
    if (use_adam)
      expert_proxy(expert(), r).takeMoments((int)wsz(),
          CkDeviceBuffer(m1, done, send_stream), CkDeviceBuffer(v1, done, send_stream),
          CkDeviceBuffer(m2, done, send_stream), CkDeviceBuffer(v2, done, send_stream));
  }
  void cloneBufDone() {
    if (++clone_bufs_done == clone_bufs_expected) self().cloneSent();
  }

  // The new copy. Post entry method: its buffers are allocated here, so the
  // state lands straight in them.
  void takeState(int step, int adam_t_, double spt_, int ncopies_, int& n, float*& w1, float*& w2,
      CkDeviceBufferPost* devicePost) {
    if (W1 == NULL) { allocate(); createEvents(); }
    w1 = W1;
    w2 = W2;
    devicePost[0].hapi_stream = commStream();
    devicePost[1].hapi_stream = commStream();
    devicePost[0].buffer_free = devicePost[1].buffer_free = true;
  }
  void takeState(int step, int adam_t_, double spt_, int ncopies_, int n, float* w1, float* w2) {
    my_step = step - 1;   // beginStep makes it `step`
    adam_t = adam_t_;
    spt = spt_;
    nc_next = ncopies_;
    state_in = true;
    TRACE(1, "expert %d.%d: state in; first step %d", expert(), rank(), step);
    startAsCopy();
  }
  void takeMoments(int& n, float*& a, float*& b, float*& c, float*& d, CkDeviceBufferPost* devicePost) {
    if (W1 == NULL) { allocate(); createEvents(); }
    a = m1; b = v1; c = m2; d = v2;
    for (int i = 0; i < 4; i++) {
      devicePost[i].hapi_stream = commStream();
      devicePost[i].buffer_free = true;
    }
  }
  void takeMoments(int n, float* a, float* b, float* c, float* d) {
    moments_in = true;
    startAsCopy();
  }
  void startAsCopy() {
    if (!state_in || (use_adam && !moments_in)) return;
    started = true;
    setCopies(nc_next);
    openChannels();
    self().runStep();
  }

  // ---- retiring ----
  // The dispatchers this copy served drop their channels at the start of the
  // next step and say so; once the last step is over and every send is done,
  // the channels close and the copy goes.
  void channelsDropped() {
    chans_dropped++;
    TRACE(2, "expert %d.%d: %d of %d dispatchers dropped their channels", expert(), rank(),
        chans_dropped, servedCount());
    maybeFinishRetire();
  }
  void maybeFinishRetire() {
    if (!retiring || !step_ended || grad_in_flight || chans_dropped < servedCount()) return;
    TRACE(1, "expert %d.%d step %d: retired", expert(), rank(), my_step);
    for (size_t i = 0; i < tok_ch.size(); i++)
      if (ch_open[i]) tok_ch[i].close();
    self().die();   // off this entry method: a message to itself
  }
  void die() { ckDestroy(); }

  void endStep() {
    if (!retiring) {
      // One report per expert per step, from the primary copy: both copies'
      // tokens and device time (the other's rode in with its partial), and
      // the weight checksum.
      if (chk_due) TRACE(2, "expert %d.%d step %d w2 %.17g", expert(), rank(), my_step, *h_chk);
      if (rank() == 0) {
        const double w2 = chk_due ? *h_chk : 0.0;
        const MoeCtx* c = ctx();
        int pes[K_MAX_COPIES]; long ns[K_MAX_COPIES]; double gs[K_MAX_COPIES];
        int np = 0;
        pes[np] = CkMyPe(); ns[np] = n_tot; gs[np] = gpu_last; np++;
        for (int r = 1; r < nc; r++) { pes[np] = peer_pe[r]; ns[np] = peer_n[r]; gs[np] = peer_gpu[r]; np++; }
        main_proxy.expertStep(expert(), my_step, (long)n_tot + peerTokens(), w2, np, pes, ns, gs,
            c->spt_gpu, c->spt_tok);
      }
      if (nc_next != nc) {
        // The round's decision takes effect: a copy more (this copy sends the
        // state when the balancer named it) or one fewer (the highest rank,
        // which is not this copy here). Channels for a grown block go out
        // now; a copy left alone frees its exchange buffers once this step's
        // block sum has been read by the transport (gradSent).
        if (nc_next > nc && clone_to_pe >= 0) executeClone();
        TRACE(1, "expert %d.%d step %d: %d copies from step %d", expert(), rank(), my_step, nc_next, my_step + 1);
        const int k_old = nc;
        setCopies(nc_next);
        refreshChannels(k_old, nc);
        if (nc == 1) exchange_free_due = true;
        clone_decided = retire_decided = false;
        decided_rank = -1;
        clone_to_pe = clone_rank = -1;
      }
    }
    if (retiring) {
      step_ended = true;
      maybeFinishRetire();
    } else if (my_step < warmup_steps + n_steps) {
      self().runStep();
    } else {
      if (print_place)
        CkPrintf("Expert %d.%d on PE %d: %d tokens in the last step\n", expert(), rank(),
            CkMyPe(), n_tot);
      if (flight_log) moeFlightDump();
      if (rank() == 0) main_proxy.expertDone(expert());
    }
  }
};

#include "moe.def.h"

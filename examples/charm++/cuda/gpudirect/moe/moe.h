#ifndef __CUDA_GPUDIRECT_MOE_H_
#define __CUDA_GPUDIRECT_MOE_H_

#include "hapi_portable.h"   // the vendor table: cuda_runtime.h or hip_runtime.h
#include <cstddef>
#include <cstdint>

// bf16 as its bit pattern. The vendors' bf16 types differ (cuda_bf16.h,
// hip_bfloat16.h); the bits do not, and the kernels convert by hand (moe.cu
// bf2f / f2bf, round to nearest even). The BLAS takes the type as an enum.
typedef uint16_t moe_bf16;

// One compute lane: a stream, a cuBLAS handle bound to it with a fixed
// workspace (so a GEMM of a given shape is bitwise reproducible wherever it
// runs), and the activation scratch the chunked expert step needs. An expert
// computes on lane (index mod n_lanes) of whatever PE it is on.
struct MoeGpu {
  hapiStream_t stream;
  void* blas;             // the BLAS handle (hapi_blas.h), opaque to the Charm++ side
  // -B: the GEMM operands (tokens, activations, the weights' bf16 copies) are
  // bf16 and the GEMMs accumulate in fp32 (hapiBlasGemmEx); the gradient
  // accumulators, the master weights and the checksums stay fp32. Without it
  // everything is fp32 and the GEMMs are the plain sgemm.
  bool bf16;
  void* h;                // [chunk x d_ff]   relu(x W1) for the current chunk, fp32 or bf16
  void* dh;               // [chunk x d_ff]   dL/dh, overwritten by dL/dz
  // Gradient accumulators for an unreplicated expert's two dispatcher
  // partitions (acc1[p] is W1-shaped, acc2[p] W2-shaped): the step's gradient
  // is acc[0] + acc[1], the same two operands a pair of replicas exchange, so
  // the sum is bit for bit the replicated one. A replicated expert owns its
  // accumulators instead (Expert::dWown / dWpeer). Always fp32.
  float* acc1[3];
  float* acc2[3];
  // Default (-U turns it off): one expert's tokens gathered from its per-source segments into a
  // single run, so the step is one chunked pass instead of one per source.
  // Lane scratch: experts sharing a lane are serialized by its stream.
  void* fx;               // [n_disp x cap_src x d_model], fp32 or bf16
  void* fy;
  void* workspace;        // cuBLAS workspace
  size_t workspace_bytes;
  double* partials;       // MOE_RED_BLOCKS partial sums for the checksums
};

#define MOE_MAX_LANES 8

// Per-PE GPU context, owned by the Dispatcher group; experts fetch it through
// ckLocalBranch() on every use and never cache it, since the pointer differs
// on every PE they migrate to.
//
// Communication and compute are on different streams on purpose. A landing
// copy the runtime issues for a receive waits, on the device, for the sender's
// stream to reach the point where the data was produced. If that copy sat on
// the same stream as this PE's GEMMs, every GEMM queued behind it would wait
// for a remote PE's compute -- measured as a 25% longer compute phase once
// the runtime stopped serializing receives on the host. So all landings (the
// receive posts name this stream) and the dispatcher's own gather/combine go
// on `comm`, and experts compute on the lanes. No cross-stream events are
// needed: a slab is consumed only after its landing completed, and the
// runtime orders a send behind the lane that produced its data.
#define MOE_BUSY_IVS 64
#define MOE_BUSY_HORIZON_MS 5000.0f   // busy intervals older than this are dropped

struct MoeCtx {
  MoeGpu comm;                  // stream + partials only; no cuBLAS, no scratch
  MoeGpu lanes[MOE_MAX_LANES];
  int n_lanes;
  // Device-busy seconds on this PE's GPU and the tokens they covered, pooled
  // over its experts with a per-step decay. Busy time is the union of the
  // experts' kernel spans on the device clock (against ref_ev), one set of
  // intervals across steps: a span adds only the time no earlier span of any
  // step covered, so the busy time of a window can never exceed its wall time.
  // (Per-step sets double-counted steps that overlap in time under lookahead,
  // and a GPU's seconds per token then read 2x another's by crowding alone;
  // the balancer moved most of its experts away, job 23383169.) The RATE the
  // experts declare with is not this PE's: with identical devices every expert
  // does the same work per token, so Main pools every PE's busy time and tokens
  // into one job-wide seconds per token (global_spt, broadcast each step), and
  // a declared load is tokens on one scale for every GPU. This PE's own figure
  // stands in until the first broadcast, and is what a heterogeneous machine
  // would want per device class.
  double spt_gpu;               // decayed sum of device-busy seconds
  double spt_tok;               // decayed sum of the tokens they covered
  int spt_step;                 // the step the decay last advanced for
  double global_spt;            // job-wide seconds per token from Main; 0 until known
  hapiEvent_t ref_ev;           // recorded once on comm, before any expert work
  int n_iv;                     // busy intervals, disjoint, in ms on the device clock
  float iv_s[MOE_BUSY_IVS], iv_e[MOE_BUSY_IVS];
  // Tokens issued on each lane and not yet complete (Expert::pickLane).
  long lane_pending[MOE_MAX_LANES];
};

#define MOE_RED_BLOCKS 256
#define MOE_RED_THREADS 256
#define MOE_MAX_TOPK 8

// The attention sublayer of -w (moe.cu): weights, activations and scratch of
// one dispatcher's tokens. Operands in the run's dtype; scores and gradient
// accumulators fp32.
struct MoeAttn {
  int T, d, H, S, nseq;
  bool bf16;
  void *Wqkv, *Wo;                 // [d x 3d], [d x d]
  void *qkv, *qh, *kh, *vh, *oh;   // [T x 3d]; head-major [nseq*H x S x dh] each
  void *o, *h;                     // [T x d]: attention output, and the sublayer output (the MoE's input)
  float* scores;                   // [nseq*H x S x S] fp32
  void* p;                         // [nseq*H x S x S] probabilities, then dz
  void *doh, *dqh, *dkh, *dvh;     // head-major gradients
  void* dqkv;                      // [T x 3d]
  float *gWqkv, *gWo;              // fp32 gradients
  void* fused;                     // the cuDNN fused-attention state (moe.cu), NULL on the unfused path
  float* stats;                    // fused path: the softmax statistics [nseq*H x S] the backward reads
};
void moeAttnInit(MoeGpu* g, MoeAttn* a, int Ttok, int d, int H, int S, uint64_t seed);
void moeAttnStep(MoeGpu* g, MoeAttn* a, const void* x, bool train, float lr_over_n);   // h = Attn(x); train: SGD on its own loss
const char* moeAttnKernels(bool bf16);   // which attention kernels this build runs under -B / fp32

// moe.cu. Buffers typed void* are fp32 or bf16 as the `bf16` argument (or
// g->bf16) says; the element size is 4 or 2 bytes.
int  moeBlasCreate(MoeGpu* g, bool tf32);      // 0 on success
void moeBlasDestroy(MoeGpu* g);
void moeInitUniform(float* p, size_t n, uint64_t seed, float bound,
    hapiStream_t s);
// dst[i] = bf16(src[i]), round to nearest even.
void moeCastBf16(const float* src, moe_bf16* dst, size_t n, hapiStream_t s);
void moeGather(const void* x, const int* idx, void* out, int n_slots,
    int dm, bool bf16, hapiStream_t s);
// y (fp32) = w * sum of the k rows of recv (fp32 or bf16) each token's slots name.
void moeCombine(const void* recv, const int* pos, float* y, int n_tok,
    int k, int dm, float w, bool bf16, hapiStream_t s);
// One run of an expert's step (the tokens of one dispatcher partition in
// dispatcher order): forward, the self-similar backward, and the gradient
// GEMMs accumulated into dW1/dW2 (fp32) with alpha 1. zeroFirst: the run's
// first chunk overwrites the accumulators (beta 0) instead of adding to them.
// W1/W2, x and y are fp32, or under g->bf16 the weights' bf16 copies and bf16
// tokens and outputs.
void moeExpertChunks(MoeGpu* g, const void* W1, const void* W2, const void* x,
    void* y, int n, int dm, int dff, int chunk, float* dW1, float* dW2, bool zeroFirst);
// Serving (-G): the forward alone, h = relu(x W1), y = h W2, per chunk; no
// gradient, so nothing accumulates and nothing is updated.
void moeExpertForward(MoeGpu* g, const void* W1, const void* W2, const void* x,
    void* y, int n, int dm, int dff, int chunk);
// The step's update, once, from the two partitions' accumulators (either may
// be NULL when its partition sent no tokens): SGD W -= (lr/n) (A + B), or Adam
// on the mean gradient (A + B) / n. Identical arithmetic whether A and B were
// computed here or one of them arrived from a replica. W1b/W2b (NULL without
// -B): the bf16 copies, rewritten from the updated master weights in the same
// pass.
void moeAddInto(MoeGpu* g, float* dst, const float* src, size_t n);   // dst += src, fp32, on g->stream
void moeApplySGD(MoeGpu* g, float* W1, float* W2, moe_bf16* W1b, moe_bf16* W2b,
    const float* A1, const float* A2, const float* B1, const float* B2, int dm, int dff,
    float lr_over_n);
void moeAdamApply(MoeGpu* g, float* W1, float* m1, float* v1, float* W2,
    float* m2, float* v2, moe_bf16* W1b, moe_bf16* W2b, const float* A1, const float* A2,
    const float* B1, const float* B2, int dm, int dff, float lr, float inv_n, int t);
// *d_out += sum(p[i]^2), in double, in a fixed order.
void moeSumSqAdd(const float* p, size_t n, MoeGpu* g, double* d_out,
    hapiStream_t s);

#endif // __CUDA_GPUDIRECT_MOE_H_

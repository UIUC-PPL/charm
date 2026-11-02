// Kokkos variant of moe.cu: the kernels as functors over the same raw pointers, launched on the lane's stream
// through the Kokkos execution-space instance wrapping it (hapi_kokkos.h), as TeamPolicy leagues of BLOCK_1D-thread
// teams with the grid-stride loops the kernels had. The GEMMs stay on cuBLAS: moe needs one handle per lane with a
// fixed workspace and math mode for its bitwise-reproducible checksums, which KokkosBlas's cuBLAS path (one shared
// handle per process, set-stream per call, no workspace control) does not give, and its native gemm is far slower.
#include "hapi.h"
#include "moe.h"
#include "hapi_kokkos.h"
#include "hapi_blas.h"
#include <cstdio>
#include <cstdlib>

#define BLOCK_1D 256

#define cublasCheck(x) do { \
    hapiBlasStatus_t st_ = (x); \
    if (st_ != HAPI_BLAS_STATUS_SUCCESS) { \
      fprintf(stderr, "BLAS error %d at %s:%d\n", (int)st_, __FILE__, \
              __LINE__); \
      abort(); \
    } \
  } while (0)

static inline int nblocks(size_t n) {
  size_t b = (n + BLOCK_1D - 1) / BLOCK_1D;
  if (b > 65535) b = 65535;
  if (b < 1) b = 1;
  return (int)b;
}

using hapikokkos::Space;
using hapikokkos::space;
using Team = Kokkos::TeamPolicy<Space>;
using Member = Team::member_type;

static inline Team teamOf(hapiStream_t s, int league, int team, size_t scratchBytes = 0) {
  Team p(space(s), league, team);
  if (scratchBytes) p.set_scratch_size(0, Kokkos::PerTeam(scratchBytes));
  return p;
}
template <class P, class F>
static inline void site(std::once_flag& once, const char* label, const P& p, const F& f) {
  hapikokkos::launch(once, [&] { Kokkos::parallel_for(label, p, f); });
}
// The grid-stride loop's start and stride, as blockIdx.x * blockDim.x + threadIdx.x and blockDim.x * gridDim.x were.
KOKKOS_INLINE_FUNCTION size_t gridStart(const Member& m) { return (size_t)m.league_rank() * m.team_size() + m.team_rank(); }
KOKKOS_INLINE_FUNCTION size_t gridStride(const Member& m) { return (size_t)m.team_size() * m.league_size(); }

// ---- cuBLAS ----

int moeBlasCreate(MoeGpu* g, bool tf32) {
  hapiBlasHandle_t h;
  if (hapiBlasCreate(&h) != HAPI_BLAS_STATUS_SUCCESS) return 1;
  cublasCheck(hapiBlasSetStream(h, g->stream));
  cublasCheck(hapiBlasSetWorkspace(h, g->workspace, g->workspace_bytes));
  cublasCheck(hapiBlasSetMathModeTf32(h, tf32));
  g->blas = (void*)h;
  return 0;
}

void moeBlasDestroy(MoeGpu* g) {
  if (g->blas) hapiBlasDestroy((hapiBlasHandle_t)g->blas);
  g->blas = NULL;
}

static void gemm_rm(hapiBlasHandle_t h, bool tA, bool tB, int m, int n, int k,
    float alpha, const float* A, int lda, const float* B, int ldb, float beta,
    float* C, int ldc) {
  cublasCheck(hapiBlasSgemm(h, tB ? HAPI_BLAS_OP_T : HAPI_BLAS_OP_N,
      tA ? HAPI_BLAS_OP_T : HAPI_BLAS_OP_N, n, m, k, &alpha, B, ldb, A, lda, &beta,
      C, ldc));
}

// The mixed-precision product of the -B path (moe.cu gemm_rm_ex): operands of type `ab`, result `ct`, fp32 sums.
static void gemm_rm_ex(hapiBlasHandle_t h, bool tA, bool tB, int m, int n, int k,
    float alpha, const void* A, int lda, const void* B, int ldb, hapiDataType_t ab,
    float beta, void* C, hapiDataType_t ct, int ldc) {
  cublasCheck(hapiBlasGemmEx(h, tB ? HAPI_BLAS_OP_T : HAPI_BLAS_OP_N,
      tA ? HAPI_BLAS_OP_T : HAPI_BLAS_OP_N, n, m, k, &alpha, B, ab, ldb, A, ab, lda,
      &beta, C, ct, ldc));
}

// ---- bf16 by hand (moe.h: moe_bf16 is the bit pattern; moe.cu bf2f / f2bf) ----

KOKKOS_FORCEINLINE_FUNCTION float bf2f(moe_bf16 h) { return Kokkos::bit_cast<float>((uint32_t)h << 16); }
KOKKOS_FORCEINLINE_FUNCTION moe_bf16 f2bf(float f) {
  uint32_t u = Kokkos::bit_cast<uint32_t>(f);
  if ((u & 0x7fffffffu) > 0x7f800000u) return (moe_bf16)((u >> 16) | 0x40u);   // a NaN stays a NaN
  u += 0x7fffu + ((u >> 16) & 1u);   // round to nearest even
  return (moe_bf16)(u >> 16);
}
KOKKOS_FORCEINLINE_FUNCTION float toF(float v) { return v; }
KOKKOS_FORCEINLINE_FUNCTION float toF(moe_bf16 v) { return bf2f(v); }
template <class T> KOKKOS_FORCEINLINE_FUNCTION T fromF(float v);
template <> KOKKOS_FORCEINLINE_FUNCTION float fromF<float>(float v) { return v; }
template <> KOKKOS_FORCEINLINE_FUNCTION moe_bf16 fromF<moe_bf16>(float v) { return f2bf(v); }

// ---- hashing ----

KOKKOS_FORCEINLINE_FUNCTION unsigned long long splitmix64(unsigned long long x) {
  x += 0x9E3779B97F4A7C15ULL;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
  return x ^ (x >> 31);
}

KOKKOS_FORCEINLINE_FUNCTION float u01(unsigned long long h) {
  return ((float)(unsigned int)(h >> 40) + 0.5f) * (1.0f / 16777216.0f);
}

// ---- kernels ----

struct InitUniformKernel {
  float* p; size_t n; unsigned long long seed; float bound;
  KOKKOS_INLINE_FUNCTION void operator()(const Member& m) const {
    const size_t stride = gridStride(m);
    for (size_t i = gridStart(m); i < n; i += stride) {
      unsigned long long h = splitmix64(seed ^ (i * 0x9E3779B97F4A7C15ULL));
      h = splitmix64(h + i);
      p[i] = bound * (2.0f * u01(h) - 1.0f);
    }
  }
};

struct CastBf16Kernel {
  const float* src; moe_bf16* dst; size_t n;
  KOKKOS_INLINE_FUNCTION void operator()(const Member& m) const {
    const size_t stride = gridStride(m);
    for (size_t i = gridStart(m); i < n; i += stride) dst[i] = f2bf(src[i]);
  }
};

template <class T>
struct GatherKernel {
  const T* x; const int* idx; T* out; size_t total; int dm;
  KOKKOS_INLINE_FUNCTION void operator()(const Member& m) const {
    const size_t stride = gridStride(m);
    for (size_t e = gridStart(m); e < total; e += stride) {
      size_t slot = e / dm;
      int c = (int)(e - slot * dm);
      out[e] = x[(size_t)idx[slot] * dm + c];
    }
  }
};

template <class T>
struct CombineKernel {
  const T* recv; const int* pos; float* y; size_t total; int k; int dm; float w;
  KOKKOS_INLINE_FUNCTION void operator()(const Member& m) const {
    const size_t stride = gridStride(m);
    for (size_t e = gridStart(m); e < total; e += stride) {
      size_t t = e / dm;
      int c = (int)(e - t * dm);
      float acc = 0.0f;
      for (int j = 0; j < k; j++)
        acc += toF(recv[(size_t)pos[t * k + j] * dm + c]);
      y[e] = w * acc;
    }
  }
};

template <class T>
struct ReluKernel {
  T* h; size_t n;
  KOKKOS_INLINE_FUNCTION void operator()(const Member& m) const {
    const size_t stride = gridStride(m);
    for (size_t i = gridStart(m); i < n; i += stride)
      if (!(toF(h[i]) > 0.0f)) h[i] = fromF<T>(0.0f);
  }
};

template <class T>
struct ReluBackwardKernel {
  T* dh; const T* h; size_t n;
  KOKKOS_INLINE_FUNCTION void operator()(const Member& m) const {
    const size_t stride = gridStride(m);
    for (size_t i = gridStart(m); i < n; i += stride)
      if (!(toF(h[i]) > 0.0f)) dh[i] = fromF<T>(0.0f);
  }
};

// The step's gradient at i (moe.cu gradAt): the two partitions' accumulators
// added in one fixed order (a missing one adds nothing), scaled by 1/n.
KOKKOS_INLINE_FUNCTION float gradAt(const float* A, const float* B, size_t i, float inv_n) {
  float g = A ? A[i] : 0.0f;
  if (B) g += B[i];
  return inv_n * g;
}

// W -= lr_over_n (A + B); Wb (when given) = bf16(W), the copy the GEMMs read
struct ApplySgdKernel {
  float* W; moe_bf16* Wb; const float* A; const float* B; size_t n; float lr_over_n;
  KOKKOS_INLINE_FUNCTION void operator()(const Member& m) const {
    const size_t stride = gridStride(m);
    for (size_t i = gridStart(m); i < n; i += stride) {
      const float w = W[i] - gradAt(A, B, i, lr_over_n);
      W[i] = w;
      if (Wb) Wb[i] = f2bf(w);
    }
  }
};

struct AdamKernel {
  float* W; moe_bf16* Wb; float* mm; float* v; const float* A; const float* B; float inv_n; size_t n;
  float lr, b1, b2, eps, c1, c2;
  KOKKOS_INLINE_FUNCTION void operator()(const Member& m) const {
    const size_t stride = gridStride(m);
    for (size_t i = gridStart(m); i < n; i += stride) {
      float gi = gradAt(A, B, i, inv_n);
      float mi = b1 * mm[i] + (1.0f - b1) * gi;
      float vi = b2 * v[i] + (1.0f - b2) * gi * gi;
      mm[i] = mi;
      v[i] = vi;
      const float w = W[i] - lr * (mi * c1) / (sqrtf(vi * c2) + eps);
      W[i] = w;
      if (Wb) Wb[i] = f2bf(w);
    }
  }
};

// Deterministic sum of squares (moe.cu): the fixed grid, strided subsets, fixed tree per team, partials in order.
struct SumSqKernel {
  const float* p; size_t n; double* partials;
  KOKKOS_INLINE_FUNCTION void operator()(const Member& m) const {
    double* sh = (double*)m.team_shmem().get_shmem(sizeof(double) * MOE_RED_THREADS);
    const int t = m.team_rank();
    double acc = 0.0;
    const size_t stride = gridStride(m);
    for (size_t i = gridStart(m); i < n; i += stride) {
      double v = p[i];
      acc += v * v;
    }
    sh[t] = acc;
    m.team_barrier();
    for (int s = m.team_size() / 2; s > 0; s >>= 1) {
      if (t < s) sh[t] += sh[t + s];
      m.team_barrier();
    }
    if (t == 0) partials[m.league_rank()] = sh[0];
  }
};

struct SumPartialsKernel {
  const double* partials; int n; double* out;
  KOKKOS_INLINE_FUNCTION void operator()(const Member& m) const {
    if (m.team_rank() == 0 && m.league_rank() == 0) {
      double s = 0.0;
      for (int i = 0; i < n; i++) s += partials[i];
      *out += s;
    }
  }
};

// ---- wrappers ----

void moeInitUniform(float* p, size_t n, uint64_t seed, float bound,
    hapiStream_t s) {
  static std::once_flag once;
  site(once, "moe::initUniform", teamOf(s, nblocks(n), BLOCK_1D),
       InitUniformKernel{p, n, (unsigned long long)seed, bound});
  hapiCheck(hapiPeekAtLastError());
}

void moeCastBf16(const float* src, moe_bf16* dst, size_t n, hapiStream_t s) {
  static std::once_flag once;
  site(once, "moe::castBf16", teamOf(s, nblocks(n), BLOCK_1D), CastBf16Kernel{src, dst, n});
  hapiCheck(hapiPeekAtLastError());
}

void moeGather(const void* x, const int* idx, void* out, int n_slots,
    int dm, bool bf16, hapiStream_t s) {
  if (n_slots <= 0) return;
  size_t total = (size_t)n_slots * dm;
  static std::once_flag o_f, o_b;
  if (bf16)
    site(o_b, "moe::gather", teamOf(s, nblocks(total), BLOCK_1D),
         GatherKernel<moe_bf16>{(const moe_bf16*)x, idx, (moe_bf16*)out, total, dm});
  else
    site(o_f, "moe::gather", teamOf(s, nblocks(total), BLOCK_1D),
         GatherKernel<float>{(const float*)x, idx, (float*)out, total, dm});
  hapiCheck(hapiPeekAtLastError());
}

void moeCombine(const void* recv, const int* pos, float* y, int n_tok,
    int k, int dm, float w, bool bf16, hapiStream_t s) {
  size_t total = (size_t)n_tok * dm;
  static std::once_flag o_f, o_b;
  if (bf16)
    site(o_b, "moe::combine", teamOf(s, nblocks(total), BLOCK_1D),
         CombineKernel<moe_bf16>{(const moe_bf16*)recv, pos, y, total, k, dm, w});
  else
    site(o_f, "moe::combine", teamOf(s, nblocks(total), BLOCK_1D),
         CombineKernel<float>{(const float*)recv, pos, y, total, k, dm, w});
  hapiCheck(hapiPeekAtLastError());
}

// The relu and its backward on the lane, fp32 or bf16 scratch (one launch site per type).
static void relu(MoeGpu* g, size_t nh) {
  static std::once_flag o_f, o_b;
  if (g->bf16) site(o_b, "moe::relu", teamOf(g->stream, nblocks(nh), BLOCK_1D), ReluKernel<moe_bf16>{(moe_bf16*)g->h, nh});
  else site(o_f, "moe::relu", teamOf(g->stream, nblocks(nh), BLOCK_1D), ReluKernel<float>{(float*)g->h, nh});
}
static void reluBackward(MoeGpu* g, size_t nh) {
  static std::once_flag o_f, o_b;
  if (g->bf16)
    site(o_b, "moe::reluBackward", teamOf(g->stream, nblocks(nh), BLOCK_1D),
         ReluBackwardKernel<moe_bf16>{(moe_bf16*)g->dh, (const moe_bf16*)g->h, nh});
  else
    site(o_f, "moe::reluBackward", teamOf(g->stream, nblocks(nh), BLOCK_1D),
         ReluBackwardKernel<float>{(float*)g->dh, (const float*)g->h, nh});
}

// One run of the expert's step (moe.cu moeExpertChunks): the gradient GEMMs
// accumulate with alpha 1; the run's first chunk overwrites when zeroFirst.
// Under g->bf16 the operands are bf16 and the GEMMs sum in fp32 (gemm_rm_ex).
void moeExpertChunks(MoeGpu* g, const void* W1, const void* W2, const void* x,
    void* y, int n, int dm, int dff, int chunk, float* dW1, float* dW2,
    bool zeroFirst) {
  hapiBlasHandle_t h = (hapiBlasHandle_t)g->blas;
  const size_t esz = g->bf16 ? sizeof(moe_bf16) : sizeof(float);
  for (int c0 = 0; c0 < n; c0 += chunk) {
    const int nc = (n - c0 < chunk) ? (n - c0) : chunk;
    const char* xc = (const char*)x + (size_t)c0 * dm * esz;
    char* yc = (char*)y + (size_t)c0 * dm * esz;
    const size_t nh = (size_t)nc * dff;
    const float beta = (zeroFirst && c0 == 0) ? 0.0f : 1.0f;
    if (!g->bf16) {
      const float* xf = (const float*)xc;
      float* yf = (float*)yc;
      float* hf = (float*)g->h;
      float* dhf = (float*)g->dh;
      gemm_rm(h, false, false, nc, dff, dm, 1.0f, xf, dm, (const float*)W1, dff, 0.0f, hf, dff);
      relu(g, nh);
      gemm_rm(h, false, false, nc, dm, dff, 1.0f, hf, dff, (const float*)W2, dm, 0.0f, yf, dm);
      gemm_rm(h, false, true, nc, dff, dm, 1.0f, yf, dm, (const float*)W2, dm, 0.0f, dhf, dff);
      reluBackward(g, nh);
      gemm_rm(h, true, false, dff, dm, nc, 1.0f, hf, dff, yf, dm, beta, dW2, dm);
      gemm_rm(h, true, false, dm, dff, nc, 1.0f, xf, dm, dhf, dff, beta, dW1, dff);
    } else {
      moe_bf16* hb = (moe_bf16*)g->h;
      moe_bf16* dhb = (moe_bf16*)g->dh;
      gemm_rm_ex(h, false, false, nc, dff, dm, 1.0f, xc, dm, W1, dff, HAPI_R_16BF, 0.0f, hb, HAPI_R_16BF, dff);
      relu(g, nh);
      gemm_rm_ex(h, false, false, nc, dm, dff, 1.0f, hb, dff, W2, dm, HAPI_R_16BF, 0.0f, yc, HAPI_R_16BF, dm);
      gemm_rm_ex(h, false, true, nc, dff, dm, 1.0f, yc, dm, W2, dm, HAPI_R_16BF, 0.0f, dhb, HAPI_R_16BF, dff);
      reluBackward(g, nh);
      gemm_rm_ex(h, true, false, dff, dm, nc, 1.0f, hb, dff, yc, dm, HAPI_R_16BF, beta, dW2, HAPI_R_32F, dm);
      gemm_rm_ex(h, true, false, dm, dff, nc, 1.0f, xc, dm, dhb, dff, HAPI_R_16BF, beta, dW1, HAPI_R_32F, dff);
    }
  }
  hapiCheck(hapiPeekAtLastError());
}

// Serving (moe.cu moeExpertForward): the forward alone.
void moeExpertForward(MoeGpu* g, const void* W1, const void* W2, const void* x,
    void* y, int n, int dm, int dff, int chunk) {
  hapiBlasHandle_t h = (hapiBlasHandle_t)g->blas;
  const size_t esz = g->bf16 ? sizeof(moe_bf16) : sizeof(float);
  for (int c0 = 0; c0 < n; c0 += chunk) {
    const int nc = (n - c0 < chunk) ? (n - c0) : chunk;
    const char* xc = (const char*)x + (size_t)c0 * dm * esz;
    char* yc = (char*)y + (size_t)c0 * dm * esz;
    const size_t nh = (size_t)nc * dff;
    if (!g->bf16) {
      float* hf = (float*)g->h;
      gemm_rm(h, false, false, nc, dff, dm, 1.0f, (const float*)xc, dm, (const float*)W1, dff, 0.0f, hf, dff);
      relu(g, nh);
      gemm_rm(h, false, false, nc, dm, dff, 1.0f, hf, dff, (const float*)W2, dm, 0.0f, (float*)yc, dm);
    } else {
      moe_bf16* hb = (moe_bf16*)g->h;
      gemm_rm_ex(h, false, false, nc, dff, dm, 1.0f, xc, dm, W1, dff, HAPI_R_16BF, 0.0f, hb, HAPI_R_16BF, dff);
      relu(g, nh);
      gemm_rm_ex(h, false, false, nc, dm, dff, 1.0f, hb, dff, W2, dm, HAPI_R_16BF, 0.0f, yc, HAPI_R_16BF, dm);
    }
  }
  hapiCheck(hapiPeekAtLastError());
}

void moeApplySGD(MoeGpu* g, float* W1, float* W2, moe_bf16* W1b, moe_bf16* W2b,
    const float* A1, const float* A2, const float* B1, const float* B2, int dm, int dff,
    float lr_over_n) {
  const size_t n = (size_t)dm * dff;
  static std::once_flag o1, o2;
  site(o1, "moe::applySgd", teamOf(g->stream, nblocks(n), BLOCK_1D), ApplySgdKernel{W1, W1b, A1, B1, n, lr_over_n});
  site(o2, "moe::applySgd", teamOf(g->stream, nblocks(n), BLOCK_1D), ApplySgdKernel{W2, W2b, A2, B2, n, lr_over_n});
  hapiCheck(hapiPeekAtLastError());
}

void moeAdamApply(MoeGpu* g, float* W1, float* m1, float* v1, float* W2,
    float* m2, float* v2, moe_bf16* W1b, moe_bf16* W2b, const float* A1, const float* A2,
    const float* B1, const float* B2, int dm, int dff, float lr, float inv_n, int t) {
  const float b1 = 0.9f, b2 = 0.999f, eps = 1e-8f;
  const float c1 = 1.0f / (1.0f - powf(b1, (float)t));
  const float c2 = 1.0f / (1.0f - powf(b2, (float)t));
  const size_t n = (size_t)dm * dff;
  static std::once_flag o1, o2;
  site(o1, "moe::adam", teamOf(g->stream, nblocks(n), BLOCK_1D),
       AdamKernel{W1, W1b, m1, v1, A1, B1, inv_n, n, lr, b1, b2, eps, c1, c2});
  site(o2, "moe::adam", teamOf(g->stream, nblocks(n), BLOCK_1D),
       AdamKernel{W2, W2b, m2, v2, A2, B2, inv_n, n, lr, b1, b2, eps, c1, c2});
  hapiCheck(hapiPeekAtLastError());
}

void moeSumSqAdd(const float* p, size_t n, MoeGpu* g, double* d_out,
    hapiStream_t s) {
  static std::once_flag o1, o2;
  site(o1, "moe::sumSq", teamOf(s, MOE_RED_BLOCKS, MOE_RED_THREADS, sizeof(double) * MOE_RED_THREADS),
       SumSqKernel{p, n, g->partials});
  site(o2, "moe::sumPartials", teamOf(s, 1, 32), SumPartialsKernel{g->partials, MOE_RED_BLOCKS, d_out});
  hapiCheck(hapiPeekAtLastError());
}

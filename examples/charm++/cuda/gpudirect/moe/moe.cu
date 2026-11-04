#include "hapi.h"
#include "moe.h"
#include "../hapi_blas.h"
#include <cstdio>
#include <cstdlib>
// The block variant's attention under -B runs on cuDNN's fused attention graphs (the flash-attention
// kernels PyTorch's scaled_dot_product_attention dispatches to): the scores never touch memory.
// MOE_CUDNN_ATTN=0 (the Makefile) builds the unfused kernels instead; the fp32 path is always unfused.
// The HIP build has the same through aotriton (MOE_AOTRITON_ATTN, below).
#ifndef MOE_CUDNN_ATTN
#define MOE_CUDNN_ATTN 0
#endif
#if MOE_CUDNN_ATTN
#include <cudnn.h>
#include <cudnn_frontend.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>
namespace fe = cudnn_frontend;
#endif
// On HIP the same fused attention comes from aotriton, the library PyTorch's scaled_dot_product_attention
// dispatches to on ROCm (its flash kernels): MOE_AOTRITON_ATTN=1 (the Makefile; the library and the gfx90a
// kernel images come from the torch wheel). Either way the fp32 path is unfused.
#ifndef MOE_AOTRITON_ATTN
#define MOE_AOTRITON_ATTN 0
#endif
#if MOE_AOTRITON_ATTN
#define AOTRITON_V2_API_FLASH_ATTN_H   // the deprecated v2 declarations are not wanted (the v3 API is used)
#include <aotriton/flash.h>
#include <aotriton/runtime.h>
#include <aotriton/util.h>
#include <aotriton/dtypes.h>
#include <cmath>
#include <cstring>
#include <vector>
#endif
#define MOE_FUSED_ATTN (MOE_CUDNN_ATTN || MOE_AOTRITON_ATTN)

#define BLOCK_1D 256

#define cublasCheck(x) do { \
    hapiBlasStatus_t st_ = (x); \
    if (st_ != hapiBlasStatusSuccess) { \
      fprintf(stderr, "cuBLAS error %d at %s:%d\n", (int)st_, __FILE__, \
              __LINE__); \
      abort(); \
    } \
  } while (0)

// The grid is capped, so every kernel launched with it loops over the grid:
// one thread per element stops at 65535 * 256 elements.
static inline int nblocks(size_t n) {
  size_t b = (n + BLOCK_1D - 1) / BLOCK_1D;
  if (b > 65535) b = 65535;
  if (b < 1) b = 1;
  return (int)b;
}

// ---- cuBLAS ----

int moeBlasCreate(MoeGpu* g, bool tf32) {
  hapiBlasHandle_t h;
  if (hapiBlasCreate(&h) != hapiBlasStatusSuccess) return 1;
  cublasCheck(hapiBlasSetStream(h, g->stream));
  // A fixed workspace: cuBLAS picks its algorithm from the workspace it has,
  // and the checksum comparison across placements needs every run of a given
  // shape to make the same choice.
  cublasCheck(hapiBlasSetWorkspace(h, g->workspace, g->workspace_bytes));
  cublasCheck(hapiBlasSetMathModeTf32(h, tf32));
  g->blas = (void*)h;
  return 0;
}

void moeBlasDestroy(MoeGpu* g) {
  if (g->blas) hapiBlasDestroy((hapiBlasHandle_t)g->blas);
  g->blas = NULL;
}

// Row-major C[m x n] = alpha * op(A)[m x k] * op(B)[k x n] + beta * C.
// cuBLAS is column-major; the row-major product is the column-major product
// of the transposes in the other order, so the operands swap places and keep
// their own transpose flags. The leading dimension of a row-major operand is
// its column count as stored.
static void gemm_rm(hapiBlasHandle_t h, bool tA, bool tB, int m, int n, int k,
    float alpha, const float* A, int lda, const float* B, int ldb, float beta,
    float* C, int ldc) {
  cublasCheck(hapiBlasSgemm(h, tB ? HAPI_BLAS_OP_T : HAPI_BLAS_OP_N,
      tA ? HAPI_BLAS_OP_T : HAPI_BLAS_OP_N, n, m, k, &alpha, B, ldb, A, lda, &beta,
      C, ldc));
}

// The same product with the operand type `ab` (bf16 here) and the result type
// `ct`, accumulated in fp32: the GEMMs of the -B path. The fp32 path keeps the
// plain sgemm above, so its bits are what they were.
static void gemm_rm_ex(hapiBlasHandle_t h, bool tA, bool tB, int m, int n, int k,
    float alpha, const void* A, int lda, const void* B, int ldb, hapiDataType_t ab,
    float beta, void* C, hapiDataType_t ct, int ldc) {
  cublasCheck(hapiBlasGemmEx(h, tB ? HAPI_BLAS_OP_T : HAPI_BLAS_OP_N,
      tA ? HAPI_BLAS_OP_T : HAPI_BLAS_OP_N, n, m, k, &alpha, B, ab, ldb, A, ab, lda,
      &beta, C, ct, ldc));
}

// ---- bf16 by hand (moe.h: moe_bf16 is the bit pattern) ----

__device__ __forceinline__ float bf2f(moe_bf16 h) {
  return __uint_as_float(((unsigned)h) << 16);
}
// Round to nearest even, as the vendors' conversions do; a NaN stays a NaN.
__device__ __forceinline__ moe_bf16 f2bf(float f) {
  unsigned u = __float_as_uint(f);
  if ((u & 0x7fffffffu) > 0x7f800000u) return (moe_bf16)((u >> 16) | 0x40u);
  u += 0x7fffu + ((u >> 16) & 1u);
  return (moe_bf16)(u >> 16);
}
__device__ __forceinline__ float toF(float v) { return v; }
__device__ __forceinline__ float toF(moe_bf16 v) { return bf2f(v); }
template <class T> __device__ __forceinline__ T fromF(float v);
template <> __device__ __forceinline__ float fromF<float>(float v) { return v; }
template <> __device__ __forceinline__ moe_bf16 fromF<moe_bf16>(float v) { return f2bf(v); }

// ---- hashing ----

__device__ inline unsigned long long splitmix64(unsigned long long x) {
  x += 0x9E3779B97F4A7C15ULL;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
  return x ^ (x >> 31);
}

// Uniform in (0,1)
__device__ inline float u01(unsigned long long h) {
  return ((float)(unsigned int)(h >> 40) + 0.5f) * (1.0f / 16777216.0f);
}

// ---- kernels ----

__global__ void initUniformKernel(float* p, size_t n, unsigned long long seed,
    float bound) {
  size_t stride = (size_t)blockDim.x * gridDim.x;
  for (size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x; i < n;
       i += stride) {
    unsigned long long h = splitmix64(seed ^ (i * 0x9E3779B97F4A7C15ULL));
    h = splitmix64(h + i);
    p[i] = bound * (2.0f * u01(h) - 1.0f);
  }
}

__global__ void castBf16Kernel(const float* src, moe_bf16* dst, size_t n) {
  size_t stride = (size_t)blockDim.x * gridDim.x;
  for (size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x; i < n;
       i += stride)
    dst[i] = f2bf(src[i]);
}

// out[i] = x[idx[i]], one row of dm elements per slot
template <class T>
__global__ void gatherKernel(const T* x, const int* idx, T* out,
    size_t total, int dm) {
  size_t stride = (size_t)blockDim.x * gridDim.x;
  for (size_t e = (size_t)blockIdx.x * blockDim.x + threadIdx.x; e < total;
       e += stride) {
    size_t slot = e / dm;
    int c = (int)(e - slot * dm);
    out[e] = x[(size_t)idx[slot] * dm + c];
  }
}

// y[t] = w * sum_j recv[pos[t*k+j]], j in a fixed order: no atomics, so the
// result does not depend on which expert answered first. The sum is fp32
// whatever recv holds.
template <class T>
__global__ void combineKernel(const T* recv, const int* pos, float* y,
    size_t total, int k, int dm, float w) {
  size_t stride = (size_t)blockDim.x * gridDim.x;
  for (size_t e = (size_t)blockIdx.x * blockDim.x + threadIdx.x; e < total;
       e += stride) {
    size_t t = e / dm;
    int c = (int)(e - t * dm);
    float acc = 0.0f;
    for (int j = 0; j < k; j++)
      acc += toF(recv[(size_t)pos[t * k + j] * dm + c]);
    y[e] = w * acc;
  }
}

template <class T>
__global__ void reluKernel(T* h, size_t n) {
  size_t stride = (size_t)blockDim.x * gridDim.x;
  for (size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x; i < n;
       i += stride)
    if (!(toF(h[i]) > 0.0f)) h[i] = fromF<T>(0.0f);
}

// dz = dh * relu'(z); relu'(z) = (h > 0) since h = relu(z)
template <class T>
__global__ void reluBackwardKernel(T* dh, const T* h, size_t n) {
  size_t stride = (size_t)blockDim.x * gridDim.x;
  for (size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x; i < n;
       i += stride)
    if (!(toF(h[i]) > 0.0f)) dh[i] = fromF<T>(0.0f);
}

// The step's gradient at i: the two partitions' accumulators added in one
// fixed order (a missing partition contributes nothing: x + 0 == x), scaled
// by 1/n. One kernel for every expert, replicated or not, so the bits agree.
__device__ inline float gradAt(const float* A, const float* B, size_t i, float inv_n) {
  float g = A ? A[i] : 0.0f;
  if (B) g += B[i];
  return inv_n * g;
}

// W -= lr_over_n (A + B); Wb (when given) = bf16(W), the copy the GEMMs read
__global__ void applySgdKernel(float* W, moe_bf16* Wb, const float* A, const float* B,
    size_t n, float lr_over_n) {
  size_t stride = (size_t)blockDim.x * gridDim.x;
  for (size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x; i < n;
       i += stride) {
    const float w = W[i] - gradAt(A, B, i, lr_over_n);
    W[i] = w;
    if (Wb) Wb[i] = f2bf(w);
  }
}

__global__ void adamKernel(float* W, moe_bf16* Wb, float* m, float* v, const float* A,
    const float* B, float inv_n, size_t n, float lr, float b1, float b2, float eps,
    float c1, float c2) {
  size_t stride = (size_t)blockDim.x * gridDim.x;
  for (size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x; i < n;
       i += stride) {
    float gi = gradAt(A, B, i, inv_n);
    float mi = b1 * m[i] + (1.0f - b1) * gi;
    float vi = b2 * v[i] + (1.0f - b2) * gi * gi;
    m[i] = mi;
    v[i] = vi;
    const float w = W[i] - lr * (mi * c1) / (sqrtf(vi * c2) + eps);
    W[i] = w;
    if (Wb) Wb[i] = f2bf(w);
  }
}

// Deterministic sum of squares: a fixed grid, each thread walks a fixed
// strided subset in order, each block folds its threads in a fixed tree, and a
// single thread adds the block partials in order. Doubles throughout. An
// atomic reduction would reorder between runs and blur the low bits, which is
// exactly what a placement-to-placement comparison must not do.
__global__ void sumSqKernel(const float* p, size_t n, double* partials) {
  __shared__ double sh[MOE_RED_THREADS];
  double acc = 0.0;
  size_t stride = (size_t)blockDim.x * gridDim.x;
  for (size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x; i < n;
       i += stride) {
    double v = p[i];
    acc += v * v;
  }
  sh[threadIdx.x] = acc;
  __syncthreads();
  for (int s = blockDim.x / 2; s > 0; s >>= 1) {
    if (threadIdx.x < s) sh[threadIdx.x] += sh[threadIdx.x + s];
    __syncthreads();
  }
  if (threadIdx.x == 0) partials[blockIdx.x] = sh[0];
}

__global__ void sumPartialsKernel(const double* partials, int n,
    double* out) {
  if (threadIdx.x == 0 && blockIdx.x == 0) {
    double s = 0.0;
    for (int i = 0; i < n; i++) s += partials[i];
    *out += s;
  }
}

// ---- wrappers ----

void moeInitUniform(float* p, size_t n, uint64_t seed, float bound,
    hapiStream_t s) {
  initUniformKernel<<<nblocks(n), BLOCK_1D, 0, s>>>(p, n,
      (unsigned long long)seed, bound);
  hapiCheck(hapiPeekAtLastError());
}

void moeCastBf16(const float* src, moe_bf16* dst, size_t n, hapiStream_t s) {
  castBf16Kernel<<<nblocks(n), BLOCK_1D, 0, s>>>(src, dst, n);
  hapiCheck(hapiPeekAtLastError());
}

void moeGather(const void* x, const int* idx, void* out, int n_slots,
    int dm, bool bf16, hapiStream_t s) {
  if (n_slots <= 0) return;
  size_t total = (size_t)n_slots * dm;
  if (bf16)
    gatherKernel<moe_bf16><<<nblocks(total), BLOCK_1D, 0, s>>>((const moe_bf16*)x, idx,
        (moe_bf16*)out, total, dm);
  else
    gatherKernel<float><<<nblocks(total), BLOCK_1D, 0, s>>>((const float*)x, idx,
        (float*)out, total, dm);
  hapiCheck(hapiPeekAtLastError());
}

void moeCombine(const void* recv, const int* pos, float* y, int n_tok,
    int k, int dm, float w, bool bf16, hapiStream_t s) {
  size_t total = (size_t)n_tok * dm;
  if (bf16)
    combineKernel<moe_bf16><<<nblocks(total), BLOCK_1D, 0, s>>>((const moe_bf16*)recv, pos,
        y, total, k, dm, w);
  else
    combineKernel<float><<<nblocks(total), BLOCK_1D, 0, s>>>((const float*)recv, pos, y,
        total, k, dm, w);
  hapiCheck(hapiPeekAtLastError());
}

// One run of the expert's step: the tokens of one dispatcher partition, in
// chunks so the activation scratch is bounded. Per chunk:
//   h  = relu(x W1)          y = h W2            (forward)
//   dy = y                                        (loss = 1/2 |y|^2)
//   dh = dy W2^T             dz = dh * relu'(z)   (backward)
//   dW2 += h^T dy            dW1 += x^T dz        (gradient, alpha 1)
// The weights are read only: the update is applied once per step from the
// accumulated gradient (moeApplySGD / moeAdamApply), scaled there by 1/n so
// the loss is a mean over the expert's tokens. That is what lets two replicas
// each run one partition against the same weights and add their partials.
// Chunk boundaries fall at fixed token offsets of a run whose order the
// dispatchers fixed, so the reduction order never depends on placement.
// zeroFirst: this call's first chunk writes the accumulators with beta 0 (the
// run's first chunk); later chunks and later calls of the same run add.
// Under g->bf16 the operands (W1, W2, x, h, y, dh) are bf16 and every GEMM
// accumulates in fp32; h, y and dh are rounded to bf16 on the way out, the
// gradient accumulators are written in fp32.
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
      gemm_rm(h, false, false, nc, dff, dm, 1.0f, xf, dm, (const float*)W1, dff, 0.0f, hf,
          dff);
      reluKernel<float><<<nblocks(nh), BLOCK_1D, 0, g->stream>>>(hf, nh);
      gemm_rm(h, false, false, nc, dm, dff, 1.0f, hf, dff, (const float*)W2, dm, 0.0f, yf,
          dm);

      gemm_rm(h, false, true, nc, dff, dm, 1.0f, yf, dm, (const float*)W2, dm, 0.0f, dhf,
          dff);
      reluBackwardKernel<float><<<nblocks(nh), BLOCK_1D, 0, g->stream>>>(dhf, hf, nh);
      gemm_rm(h, true, false, dff, dm, nc, 1.0f, hf, dff, yf, dm, beta, dW2, dm);
      gemm_rm(h, true, false, dm, dff, nc, 1.0f, xf, dm, dhf, dff, beta, dW1, dff);
    } else {
      moe_bf16* hb = (moe_bf16*)g->h;
      moe_bf16* dhb = (moe_bf16*)g->dh;
      gemm_rm_ex(h, false, false, nc, dff, dm, 1.0f, xc, dm, W1, dff, HAPI_R_16BF, 0.0f, hb,
          HAPI_R_16BF, dff);
      reluKernel<moe_bf16><<<nblocks(nh), BLOCK_1D, 0, g->stream>>>(hb, nh);
      gemm_rm_ex(h, false, false, nc, dm, dff, 1.0f, hb, dff, W2, dm, HAPI_R_16BF, 0.0f, yc,
          HAPI_R_16BF, dm);

      gemm_rm_ex(h, false, true, nc, dff, dm, 1.0f, yc, dm, W2, dm, HAPI_R_16BF, 0.0f, dhb,
          HAPI_R_16BF, dff);
      reluBackwardKernel<moe_bf16><<<nblocks(nh), BLOCK_1D, 0, g->stream>>>(dhb, hb, nh);
      gemm_rm_ex(h, true, false, dff, dm, nc, 1.0f, hb, dff, yc, dm, HAPI_R_16BF, beta, dW2,
          HAPI_R_32F, dm);
      gemm_rm_ex(h, true, false, dm, dff, nc, 1.0f, xc, dm, dhb, dff, HAPI_R_16BF, beta, dW1,
          HAPI_R_32F, dff);
    }
  }
  hapiCheck(hapiPeekAtLastError());
}

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
      gemm_rm(h, false, false, nc, dff, dm, 1.0f, (const float*)xc, dm, (const float*)W1, dff,
          0.0f, hf, dff);
      reluKernel<float><<<nblocks(nh), BLOCK_1D, 0, g->stream>>>(hf, nh);
      gemm_rm(h, false, false, nc, dm, dff, 1.0f, hf, dff, (const float*)W2, dm, 0.0f,
          (float*)yc, dm);
    } else {
      moe_bf16* hb = (moe_bf16*)g->h;
      gemm_rm_ex(h, false, false, nc, dff, dm, 1.0f, xc, dm, W1, dff, HAPI_R_16BF, 0.0f, hb,
          HAPI_R_16BF, dff);
      reluKernel<moe_bf16><<<nblocks(nh), BLOCK_1D, 0, g->stream>>>(hb, nh);
      gemm_rm_ex(h, false, false, nc, dm, dff, 1.0f, hb, dff, W2, dm, HAPI_R_16BF, 0.0f, yc,
          HAPI_R_16BF, dm);
    }
  }
  hapiCheck(hapiPeekAtLastError());
}

__global__ void addIntoKernel(float* dst, const float* src, size_t n) {
  const size_t stride = (size_t)blockDim.x * gridDim.x;
  for (size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x; i < n; i += stride)
    dst[i] += src[i];
}
// One node of the gradient's fixed summation tree (moe.C: partitions): the
// two leaves' order does not matter, fp32 addition is commutative.
void moeAddInto(MoeGpu* g, float* dst, const float* src, size_t n) {
  addIntoKernel<<<nblocks(n), BLOCK_1D, 0, g->stream>>>(dst, src, n);
  hapiCheck(hapiPeekAtLastError());
}

void moeApplySGD(MoeGpu* g, float* W1, float* W2, moe_bf16* W1b, moe_bf16* W2b,
    const float* A1, const float* A2, const float* B1, const float* B2, int dm, int dff,
    float lr_over_n) {
  const size_t n = (size_t)dm * dff;
  applySgdKernel<<<nblocks(n), BLOCK_1D, 0, g->stream>>>(W1, W1b, A1, B1, n, lr_over_n);
  applySgdKernel<<<nblocks(n), BLOCK_1D, 0, g->stream>>>(W2, W2b, A2, B2, n, lr_over_n);
  hapiCheck(hapiPeekAtLastError());
}

void moeAdamApply(MoeGpu* g, float* W1, float* m1, float* v1, float* W2,
    float* m2, float* v2, moe_bf16* W1b, moe_bf16* W2b, const float* A1, const float* A2,
    const float* B1, const float* B2, int dm, int dff, float lr, float inv_n, int t) {
  const float b1 = 0.9f, b2 = 0.999f, eps = 1e-8f;
  const float c1 = 1.0f / (1.0f - powf(b1, (float)t));
  const float c2 = 1.0f / (1.0f - powf(b2, (float)t));
  const size_t n = (size_t)dm * dff;
  adamKernel<<<nblocks(n), BLOCK_1D, 0, g->stream>>>(W1, W1b, m1, v1, A1, B1, inv_n, n,
      lr, b1, b2, eps, c1, c2);
  adamKernel<<<nblocks(n), BLOCK_1D, 0, g->stream>>>(W2, W2b, m2, v2, A2, B2, inv_n, n,
      lr, b1, b2, eps, c1, c2);
  hapiCheck(hapiPeekAtLastError());
}

void moeSumSqAdd(const float* p, size_t n, MoeGpu* g, double* d_out,
    hapiStream_t s) {
  sumSqKernel<<<MOE_RED_BLOCKS, MOE_RED_THREADS, 0, s>>>(p, n, g->partials);
  sumPartialsKernel<<<1, 32, 0, s>>>(g->partials, MOE_RED_BLOCKS, d_out);
  hapiCheck(hapiPeekAtLastError());
}

// ---- the attention sublayer (-w) ----
// A transformer block's other sublayer, run by each dispatcher on its own
// tokens before they are gathered: q, k, v and o projections (d x d, no bias)
// and causal scaled-dot-product attention with H heads over sequences of S
// tokens. It is trained on its own loss 1/2 |h|^2 with SGD and the MoE
// sublayer takes h as its input, so the MoE layer's contract (weight
// gradients, no input gradient) is unchanged and the attention work, local
// to the GPU, is identical in every implementation (ref/moe_ref.py's
// Attention is the same computation in PyTorch). Operands follow the run's
// dtype (bf16 under -B); scores and the gradient accumulators are fp32.

static void gemm_rm_batched_ex(hapiBlasHandle_t h, bool tA, bool tB, int m, int n, int k,
    float alpha, const void* A, int lda, long long sA, const void* B, int ldb, long long sB,
    hapiDataType_t ab, float beta, void* C, hapiDataType_t ct, int ldc, long long sC, int batch) {
  cublasCheck(hapiBlasGemmStridedBatchedEx(h, tB ? HAPI_BLAS_OP_T : HAPI_BLAS_OP_N,
      tA ? HAPI_BLAS_OP_T : HAPI_BLAS_OP_N, n, m, k, &alpha, B, ab, ldb, sB, A, ab, lda, sA,
      &beta, C, ct, ldc, sC, batch));
}

// Column block `col` (width d) of a [T x w] row-major array -> head-major [nseq*H x S x dh].
template <class T>
__global__ void toHeadsKernel(const T* x, T* xh, int Ttok, int d, int H, int S, int col, int w) {
  const size_t n = (size_t)Ttok * d, stride = (size_t)blockDim.x * gridDim.x;
  for (size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x; i < n; i += stride) {
    const int t = (int)(i / d), c = (int)(i % d), dh = d / H;
    const int seq = t / S, pos = t % S, hh = c / dh, cc = c % dh;
    xh[(((size_t)seq * H + hh) * S + pos) * dh + cc] = x[(size_t)t * w + col + c];
  }
}

// Head-major [nseq*H x S x dh] -> [T x d] at column offset `col` of a row of width `w`.
template <class T>
__global__ void mergeHeadsKernel(const T* xh, T* out, int Ttok, int d, int H, int S, int col, int w) {
  const size_t n = (size_t)Ttok * d, stride = (size_t)blockDim.x * gridDim.x;
  for (size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x; i < n; i += stride) {
    const int t = (int)(i / d), c = (int)(i % d), dh = d / H;
    const int seq = t / S, pos = t % S, hh = c / dh, cc = c % dh;
    out[(size_t)t * w + col + c] = xh[(((size_t)seq * H + hh) * S + pos) * dh + cc];
  }
}

// Causal softmax of one row of fp32 scores (scaled by `scale`) into T probabilities.
template <class T>
__global__ void causalSoftmaxKernel(const float* s, T* p, int S, float scale) {
  const size_t row = blockIdx.x;          // (batch, i)
  const int i = (int)(row % S);
  const float* sr = s + row * S;
  T* pr = p + row * S;
  __shared__ float red[256];
  float m = -1e30f;
  for (int j = threadIdx.x; j <= i; j += blockDim.x) m = fmaxf(m, sr[j] * scale);
  red[threadIdx.x] = m; __syncthreads();
  for (int o = blockDim.x / 2; o > 0; o >>= 1) { if (threadIdx.x < o) red[threadIdx.x] = fmaxf(red[threadIdx.x], red[threadIdx.x + o]); __syncthreads(); }
  m = red[0]; __syncthreads();
  float sum = 0.0f;
  for (int j = threadIdx.x; j <= i; j += blockDim.x) sum += __expf(sr[j] * scale - m);
  red[threadIdx.x] = sum; __syncthreads();
  for (int o = blockDim.x / 2; o > 0; o >>= 1) { if (threadIdx.x < o) red[threadIdx.x] += red[threadIdx.x + o]; __syncthreads(); }
  const float inv = 1.0f / red[0];
  for (int j = threadIdx.x; j < S; j += blockDim.x)
    pr[j] = fromF<T>(j <= i ? __expf(sr[j] * scale - m) * inv : 0.0f);
}

// Softmax backward of one row: dz = p * (ds - sum_j ds_j p_j) * scale, written over p.
template <class T>
__global__ void softmaxBackwardKernel(const float* ds, T* p, int S, float scale) {
  const size_t row = blockIdx.x;
  const float* dr = ds + row * S;
  T* pr = p + row * S;
  __shared__ float red[256];
  float acc = 0.0f;
  for (int j = threadIdx.x; j < S; j += blockDim.x) acc += dr[j] * toF(pr[j]);
  red[threadIdx.x] = acc; __syncthreads();
  for (int o = blockDim.x / 2; o > 0; o >>= 1) { if (threadIdx.x < o) red[threadIdx.x] += red[threadIdx.x + o]; __syncthreads(); }
  const float tot = red[0];
  for (int j = threadIdx.x; j < S; j += blockDim.x) {
    const float pj = toF(pr[j]);
    pr[j] = fromF<T>(pj * (dr[j] - tot) * scale);
  }
}

template <class T>
__global__ void sgdPlainKernel(T* W, const float* G, size_t n, float lr) {
  const size_t stride = (size_t)blockDim.x * gridDim.x;
  for (size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x; i < n; i += stride)
    W[i] = fromF<T>(toF(W[i]) - lr * G[i]);
}

static void* attnAlloc(size_t bytes) {
  void* p = nullptr;
  hapiCheck(hapiMalloc(&p, bytes));
  return p;
}

#if MOE_CUDNN_ATTN
// ---- cuDNN fused attention: one graph for the training forward (with the softmax statistics), one for
// the inference forward, one for the backward. Q, K and V are strided views of the packed [T x 3d] qkv
// array (token t = b S + s, column h dh + i, at column offsets 0, d, 2d), O and dO of [T x d] arrays,
// and dQ, dK, dV are written straight into the packed [T x 3d] dqkv: no head copies on either side.
struct AttnFused {
  cudnnHandle_t handle = nullptr;
  std::shared_ptr<fe::graph::Graph> fwd, inf, bwd;
  std::shared_ptr<fe::graph::Tensor_attributes> fQ, fK, fV, fO, fS;
  std::shared_ptr<fe::graph::Tensor_attributes> iQ, iK, iV, iO;
  std::shared_ptr<fe::graph::Tensor_attributes> bQ, bK, bV, bO, bdO, bS, bdQ, bdK, bdV;
  void* ws = nullptr;
  int64_t ws_bytes = 0;
};
#define feCheck(call) do { auto _e = (call); if (!_e.is_good()) { \
    fprintf(stderr, "cuDNN frontend: %s (%s:%d)\n", _e.get_message().c_str(), __FILE__, __LINE__); abort(); } } while (0)
#define cudnnCheck(call) do { cudnnStatus_t _s = (call); if (_s != CUDNN_STATUS_SUCCESS) { \
    fprintf(stderr, "cuDNN: %s (%s:%d)\n", cudnnGetErrorString(_s), __FILE__, __LINE__); abort(); } } while (0)

// A [B, H, S, dh] bf16 view of a row-major [T x w] array (row t = b S + s, column h dh + i).
static std::shared_ptr<fe::graph::Tensor_attributes> attnView(std::shared_ptr<fe::graph::Graph>& g,
    const char* name, int64_t b, int64_t h, int64_t s, int64_t dh, int64_t w) {
  return g->tensor(fe::graph::Tensor_attributes().set_name(name).set_dim({b, h, s, dh})
      .set_stride({s * w, dh, w, 1}).set_data_type(fe::DataType_t::BFLOAT16));
}
static std::shared_ptr<fe::graph::Graph> attnGraph() {
  auto g = std::make_shared<fe::graph::Graph>();
  g->set_io_data_type(fe::DataType_t::BFLOAT16).set_intermediate_data_type(fe::DataType_t::FLOAT)
      .set_compute_data_type(fe::DataType_t::FLOAT);
  return g;
}
static int64_t attnBuild(std::shared_ptr<fe::graph::Graph>& g, cudnnHandle_t h) {
  feCheck(g->validate());
  feCheck(g->build_operation_graph(h));
  feCheck(g->create_execution_plans({fe::HeurMode_t::A}));
  feCheck(g->check_support(h));
  feCheck(g->build_plans(h));
  int64_t ws = 0;
  feCheck(g->get_workspace_size(ws));
  return ws;
}
static void attnFusedInit(MoeGpu* g, MoeAttn* a) {
  AttnFused* f = new AttnFused;
  cudnnCheck(cudnnCreate(&f->handle));
  cudnnCheck(cudnnSetStream(f->handle, g->stream));
  const int64_t b = a->nseq, h = a->H, s = a->S, d = a->d, dh = d / h;
  const float scale = 1.0f / sqrtf((float)dh);
  // training forward
  f->fwd = attnGraph();
  f->fQ = attnView(f->fwd, "Q", b, h, s, dh, 3 * d);
  f->fK = attnView(f->fwd, "K", b, h, s, dh, 3 * d);
  f->fV = attnView(f->fwd, "V", b, h, s, dh, 3 * d);
  auto fo = f->fwd->sdpa(f->fQ, f->fK, f->fV, fe::graph::SDPA_attributes().set_name("attn")
      .set_generate_stats(true).set_causal_mask(true).set_attn_scale(scale));
  f->fO = fo[0]; f->fS = fo[1];
  f->fO->set_output(true).set_dim({b, h, s, dh}).set_stride({s * d, dh, d, 1});
  f->fS->set_output(true).set_data_type(fe::DataType_t::FLOAT).set_dim({b, h, s, 1}).set_stride({h * s, s, 1, 1});
  int64_t ws = attnBuild(f->fwd, f->handle);
  // inference forward: no statistics
  f->inf = attnGraph();
  f->iQ = attnView(f->inf, "Q", b, h, s, dh, 3 * d);
  f->iK = attnView(f->inf, "K", b, h, s, dh, 3 * d);
  f->iV = attnView(f->inf, "V", b, h, s, dh, 3 * d);
  auto io = f->inf->sdpa(f->iQ, f->iK, f->iV, fe::graph::SDPA_attributes().set_name("attn_inf")
      .set_generate_stats(false).set_causal_mask(true).set_attn_scale(scale));
  f->iO = io[0];
  f->iO->set_output(true).set_dim({b, h, s, dh}).set_stride({s * d, dh, d, 1});
  ws = std::max(ws, attnBuild(f->inf, f->handle));
  // backward: dQ, dK, dV from Q, K, V, O, dO and the statistics
  f->bwd = attnGraph();
  f->bQ = attnView(f->bwd, "Q", b, h, s, dh, 3 * d);
  f->bK = attnView(f->bwd, "K", b, h, s, dh, 3 * d);
  f->bV = attnView(f->bwd, "V", b, h, s, dh, 3 * d);
  f->bO = attnView(f->bwd, "O", b, h, s, dh, d);
  f->bdO = attnView(f->bwd, "dO", b, h, s, dh, d);
  f->bS = f->bwd->tensor(fe::graph::Tensor_attributes().set_name("stats").set_dim({b, h, s, 1})
      .set_stride({h * s, s, 1, 1}).set_data_type(fe::DataType_t::FLOAT));
  auto bo = f->bwd->sdpa_backward(f->bQ, f->bK, f->bV, f->bO, f->bdO, f->bS,
      fe::graph::SDPA_backward_attributes().set_name("attn_bwd").set_causal_mask(true).set_attn_scale(scale));
  f->bdQ = bo[0]; f->bdK = bo[1]; f->bdV = bo[2];
  f->bdQ->set_output(true).set_dim({b, h, s, dh}).set_stride({s * 3 * d, dh, 3 * d, 1});
  f->bdK->set_output(true).set_dim({b, h, s, dh}).set_stride({s * 3 * d, dh, 3 * d, 1});
  f->bdV->set_output(true).set_dim({b, h, s, dh}).set_stride({s * 3 * d, dh, 3 * d, 1});
  ws = std::max(ws, attnBuild(f->bwd, f->handle));
  f->ws_bytes = ws;
  f->ws = ws > 0 ? attnAlloc((size_t)ws) : nullptr;
  a->fused = f;
}
// o = Attn(qkv) (and the statistics when training).
static void attnFusedForward(MoeAttn* a, bool train) {
  AttnFused* f = (AttnFused*)a->fused;
  moe_bf16* qkv = (moe_bf16*)a->qkv;
  const int d = a->d;
  if (train) {
    std::unordered_map<std::shared_ptr<fe::graph::Tensor_attributes>, void*> pack = {
        {f->fQ, qkv}, {f->fK, qkv + d}, {f->fV, qkv + 2 * d}, {f->fO, a->o}, {f->fS, a->stats}};
    feCheck(f->fwd->execute(f->handle, pack, f->ws));
  } else {
    std::unordered_map<std::shared_ptr<fe::graph::Tensor_attributes>, void*> pack = {
        {f->iQ, qkv}, {f->iK, qkv + d}, {f->iV, qkv + 2 * d}, {f->iO, a->o}};
    feCheck(f->inf->execute(f->handle, pack, f->ws));
  }
}
// dqkv = the gradients over q, k, v from dO (in a->doh), with o and the statistics of the forward.
static void attnFusedBackward(MoeAttn* a) {
  AttnFused* f = (AttnFused*)a->fused;
  moe_bf16* qkv = (moe_bf16*)a->qkv;
  moe_bf16* dqkv = (moe_bf16*)a->dqkv;
  const int d = a->d;
  std::unordered_map<std::shared_ptr<fe::graph::Tensor_attributes>, void*> pack = {
      {f->bQ, qkv}, {f->bK, qkv + d}, {f->bV, qkv + 2 * d}, {f->bO, a->o}, {f->bdO, a->doh}, {f->bS, a->stats},
      {f->bdQ, dqkv}, {f->bdK, dqkv + d}, {f->bdV, dqkv + 2 * d}};
  feCheck(f->bwd->execute(f->handle, pack, f->ws));
}
static void attnFusedFree(MoeAttn* a) {
  AttnFused* f = (AttnFused*)a->fused;
  if (f->ws) hapiCheck(hapiFree(f->ws));
  cudnnCheck(cudnnDestroy(f->handle));
  delete f;
}
#endif

#if MOE_AOTRITON_ATTN
// ---- aotriton fused attention (the v3 API, as PyTorch 2.13 calls it): the forward writes O and, when
// training, the softmax statistics L [B H x S]; the backward takes Q, K, V, O, dO and L and writes dQ, dK, dV,
// with a delta row per query and an fp32 dQ accumulator as its workspaces. Q, K, V and dQ, dK, dV are the
// same strided views of the packed [T x 3d] arrays as on the cuDNN path: no head copies on either side.
struct AttnFused {
  hipStream_t stream = nullptr;
  int32_t* counter = nullptr;    // the causal forward's persistent atomic counter, zeroed before each call
  uint64_t* philox = nullptr;    // the seed and offset the dropout-free kernels never read
  float* delta = nullptr;        // [B H x S] fp32, the backward's D
  float* dq_acc = nullptr;       // [B, H, S, dh] fp32, the backward's dQ accumulator, zeroed before each call
  size_t dq_acc_bytes = 0;
};
namespace aof = aotriton::v3::flash;
#define aoCheck(call) do { hipError_t _e = (call); if (_e != hipSuccess) { \
    fprintf(stderr, "aotriton: %s (%s:%d)\n", hipGetErrorString(_e), __FILE__, __LINE__); abort(); } } while (0)
// A [B, H, S, dh] bf16 view of a row-major [T x w] array (row t = b S + s, column h dh + i).
static aotriton::TensorView<4> aoView(const void* base, int64_t b, int64_t h, int64_t s, int64_t dh, int64_t w) {
  return aotriton::TensorView<4>((intptr_t)base, {(uint64_t)b, (uint64_t)h, (uint64_t)s, (uint64_t)dh},
      {(uint64_t)(s * w), (uint64_t)dh, (uint64_t)w, (uint64_t)1}, aotriton::DType::kBFloat16);
}
static aotriton::TensorView<2> aoRows(const float* base, int64_t rows, int64_t s) {
  return aotriton::TensorView<2>((intptr_t)base, {(uint64_t)rows, (uint64_t)s}, {(uint64_t)s, (uint64_t)1},
      aotriton::DType::kFloat32);
}
static void attnFusedInit(MoeGpu* g, MoeAttn* a) {
  AttnFused* f = new AttnFused;
  f->stream = g->stream;
  const size_t rows = (size_t)a->nseq * a->H;
  f->counter = (int32_t*)attnAlloc(sizeof(int32_t));
  f->philox = (uint64_t*)attnAlloc(2 * sizeof(uint64_t));
  aoCheck(hipMemset(f->philox, 0, 2 * sizeof(uint64_t)));
  f->delta = (float*)attnAlloc(sizeof(float) * rows * a->S);
  f->dq_acc_bytes = sizeof(float) * (size_t)a->T * a->d;
  f->dq_acc = (float*)attnAlloc(f->dq_acc_bytes);
  a->fused = f;
}
static void attnFusedFree(MoeAttn* a) {
  AttnFused* f = (AttnFused*)a->fused;
  hapiCheck(hapiFree(f->counter)); hapiCheck(hapiFree(f->philox));
  hapiCheck(hapiFree(f->delta)); hapiCheck(hapiFree(f->dq_acc));
  delete f;
}
template <class P>
static void aoCommon(P& p, const AttnFused* f, int64_t s) {
  p.B = aotriton::TensorView<4>::get_null_tensor(aotriton::DType::kBFloat16);
  p.Max_seqlen_q = (int32_t)s; p.Max_seqlen_k = (int32_t)s;
  p.dropout_p = 0.0f;
  p.philox_seed_ptr = aotriton::TensorView<0>((intptr_t)f->philox, aotriton::DType::kUInt64);
  p.philox_offset1 = aotriton::TensorView<0>((intptr_t)(f->philox + 1), aotriton::DType::kUInt64);
  p.philox_offset2 = 0;
  p.causal_type = aof::CausalType::WindowedAttention;   // causal, as PyTorch passes is_causal
  p.varlen_type = aof::VarlenType::None;
  p.window_left = aof::WindowValue::BottomRightAligned;
  p.window_right = aof::WindowValue::BottomRightAligned;
}
// o = Attn(qkv) (and the statistics when training).
static void attnFusedForward(MoeAttn* a, bool train) {
  AttnFused* f = (AttnFused*)a->fused;
  const moe_bf16* qkv = (const moe_bf16*)a->qkv;
  const int64_t b = a->nseq, h = a->H, s = a->S, d = a->d, dh = d / h;
  aoCheck(hipMemsetAsync(f->counter, 0, sizeof(int32_t), f->stream));
  aof::attn_fwd_params p;
  p.Q = aoView(qkv, b, h, s, dh, 3 * d);
  p.K = aoView(qkv + d, b, h, s, dh, 3 * d);
  p.V = aoView(qkv + 2 * d, b, h, s, dh, 3 * d);
  p.Sm_scale = 1.0f / sqrtf((float)dh);
  p.L = train ? aoRows(a->stats, b * h, s) : aotriton::TensorView<2>::get_null_tensor(aotriton::DType::kFloat32);
  p.Out = aoView(a->o, b, h, s, dh, d);
  p.philox_seed_output = aotriton::TensorView<0>::get_null_tensor(aotriton::DType::kUInt64);
  p.philox_offset_output = aotriton::TensorView<0>::get_null_tensor(aotriton::DType::kUInt64);
  p.encoded_softmax = aotriton::TensorView<4>::get_null_tensor(aotriton::DType::kBFloat16);
  p.persistent_atomic_counter = aotriton::TensorView<0>((intptr_t)f->counter, aotriton::DType::kInt32);
  aoCommon(p, f, s);
  aoCheck(aof::attn_fwd(p, aof::attn_fwd_params::kVersion, aotriton::Stream(f->stream)));
}
// dqkv = the gradients over q, k, v from dO (in a->doh), with o and the statistics of the forward.
static void attnFusedBackward(MoeAttn* a) {
  AttnFused* f = (AttnFused*)a->fused;
  const moe_bf16* qkv = (const moe_bf16*)a->qkv;
  moe_bf16* dqkv = (moe_bf16*)a->dqkv;
  const int64_t b = a->nseq, h = a->H, s = a->S, d = a->d, dh = d / h;
  aoCheck(hipMemsetAsync(f->dq_acc, 0, f->dq_acc_bytes, f->stream));
  aof::attn_bwd_params p;
  p.Q = aoView(qkv, b, h, s, dh, 3 * d);
  p.K = aoView(qkv + d, b, h, s, dh, 3 * d);
  p.V = aoView(qkv + 2 * d, b, h, s, dh, 3 * d);
  p.Sm_scale = 1.0f / sqrtf((float)dh);
  p.Out = aoView(a->o, b, h, s, dh, d);
  p.DO = aoView(a->doh, b, h, s, dh, d);
  p.DQ = aoView(dqkv, b, h, s, dh, 3 * d);
  p.DK = aoView(dqkv + d, b, h, s, dh, 3 * d);
  p.DV = aoView(dqkv + 2 * d, b, h, s, dh, 3 * d);
  p.DB = aotriton::TensorView<4>::get_null_tensor(aotriton::DType::kBFloat16);
  p.L = aoRows(a->stats, b * h, s);
  p.D.eager = aoRows(f->delta, b * h, s);
  p.DQ_ACC.eager = aotriton::TensorView<4>((intptr_t)f->dq_acc, {(uint64_t)b, (uint64_t)h, (uint64_t)s, (uint64_t)dh},
      {(uint64_t)(h * s * dh), (uint64_t)(s * dh), (uint64_t)dh, (uint64_t)1}, aotriton::DType::kFloat32);
  aoCommon(p, f, s);
  aoCheck(aof::attn_bwd(p, aof::attn_bwd_params::kVersion, aotriton::Stream(f->stream)));
}
#endif

const char* moeAttnKernels(bool bf16) {
#if MOE_CUDNN_ATTN
  return bf16 ? "cuDNN fused attention (the flash kernels)" : "unfused cuBLAS and softmax kernels (fp32)";
#elif MOE_AOTRITON_ATTN
  return bf16 ? "aotriton fused attention (the flash kernels PyTorch runs on ROCm)" : "unfused hipBLAS and softmax kernels (fp32)";
#else
  (void)bf16;
  return "unfused cuBLAS and softmax kernels";
#endif
}

template <class T>
static void attnStepT(MoeGpu* g, MoeAttn* a, const void* x, bool train, float lr);

// want_fused: the fused path (cuDNN or aotriton) when this build has one and the operands are bf16; else the
// unfused kernels.
static void attnInit(MoeGpu* g, MoeAttn* a, int Ttok, int d, int H, int S, uint64_t seed, bool want_fused) {
  a->T = Ttok; a->d = d; a->H = H; a->S = S; a->nseq = Ttok / S; a->bf16 = g->bf16;
  const size_t esz = g->bf16 ? sizeof(moe_bf16) : sizeof(float);
  const size_t B = (size_t)a->nseq * H;
  a->fused = nullptr; a->stats = nullptr;
  a->qh = a->kh = a->vh = a->oh = a->dqh = a->dkh = a->dvh = a->p = nullptr; a->scores = nullptr;
  a->Wqkv = attnAlloc(esz * d * 3 * d);
  a->Wo = attnAlloc(esz * d * d);
  a->qkv = attnAlloc(esz * Ttok * 3 * d);
  a->o = attnAlloc(esz * Ttok * d);
  a->h = attnAlloc(esz * Ttok * d);
  a->doh = attnAlloc(esz * Ttok * d);
  a->dqkv = attnAlloc(esz * Ttok * 3 * d);
#if MOE_FUSED_ATTN
  if (g->bf16 && want_fused) {
    a->stats = (float*)attnAlloc(sizeof(float) * B * S);
    attnFusedInit(g, a);
  }
#else
  (void)want_fused;
#endif
  if (!a->fused) {
    a->qh = attnAlloc(esz * Ttok * d); a->kh = attnAlloc(esz * Ttok * d); a->vh = attnAlloc(esz * Ttok * d);
    a->oh = attnAlloc(esz * Ttok * d);
    a->scores = (float*)attnAlloc(sizeof(float) * B * S * S);
    a->p = attnAlloc(esz * B * S * S);
    a->dqh = attnAlloc(esz * Ttok * d); a->dkh = attnAlloc(esz * Ttok * d); a->dvh = attnAlloc(esz * Ttok * d);
  }
  a->gWqkv = (float*)attnAlloc(sizeof(float) * d * 3 * d);
  a->gWo = (float*)attnAlloc(sizeof(float) * d * d);
  // Weights uniform in +-1/sqrt(d), the PyTorch harness's bound.
  const float bound = 1.0f / sqrtf((float)d);
  float* tmp = (float*)attnAlloc(sizeof(float) * d * 3 * d);
  moeInitUniform(tmp, (size_t)d * 3 * d, seed, bound, g->stream);
  if (g->bf16) moeCastBf16(tmp, (moe_bf16*)a->Wqkv, (size_t)d * 3 * d, g->stream);
  else hapiCheck(hapiMemcpyAsync(a->Wqkv, tmp, sizeof(float) * d * 3 * d, hapiMemcpyDeviceToDevice, g->stream));
  moeInitUniform(tmp, (size_t)d * d, seed + 1, bound, g->stream);
  if (g->bf16) moeCastBf16(tmp, (moe_bf16*)a->Wo, (size_t)d * d, g->stream);
  else hapiCheck(hapiMemcpyAsync(a->Wo, tmp, sizeof(float) * d * d, hapiMemcpyDeviceToDevice, g->stream));
  hapiCheck(hapiStreamSynchronize(g->stream));
  hapiCheck(hapiFree(tmp));
}

static void attnFree(MoeAttn* a) {
  void* bufs[] = {a->Wqkv, a->Wo, a->qkv, a->qh, a->kh, a->vh, a->oh, a->o, a->h, a->scores, a->p, a->doh,
                  a->dqh, a->dkh, a->dvh, a->dqkv, a->gWqkv, a->gWo, a->stats};
  for (void* b : bufs) if (b) hapiCheck(hapiFree(b));
#if MOE_FUSED_ATTN
  if (a->fused) attnFusedFree(a);
#endif
  memset(a, 0, sizeof(*a));
}

#if MOE_FUSED_ATTN
static double relL2(const float* x, const float* ref, size_t n) {
  double dd = 0.0, rr = 0.0;
  for (size_t i = 0; i < n; i++) { const double e = (double)x[i] - ref[i]; dd += e * e; rr += (double)ref[i] * ref[i]; }
  return rr > 0.0 ? sqrt(dd / rr) : sqrt(dd);
}
static void toHost(std::vector<float>& out, const void* dev, size_t n, bool bf16) {
  out.resize(n);
  if (!bf16) { hapiCheck(hapiMemcpy(out.data(), dev, n * sizeof(float), hapiMemcpyDeviceToHost)); return; }
  std::vector<moe_bf16> b(n);
  hapiCheck(hapiMemcpy(b.data(), dev, n * sizeof(moe_bf16), hapiMemcpyDeviceToHost));
  for (size_t i = 0; i < n; i++) { const unsigned u = ((unsigned)b[i]) << 16; memcpy(&out[i], &u, 4); }
}
// Once, at startup: the fused path's forward output and weight gradients against the unfused kernels'
// on the same weights and a random input (one training step at learning rate 0). The two differ by
// the fused kernel's internal rounding, a relative error around 1e-3 in bf16; past 5e-2 the run aborts.
static void attnSelfCheck(MoeGpu* g, MoeAttn* a, uint64_t seed) {
  MoeAttn u;
  attnInit(g, &u, a->T, a->d, a->H, a->S, seed, false);
  const size_t n = (size_t)a->T * a->d, nq = (size_t)a->d * 3 * a->d, no = (size_t)a->d * a->d;
  float* xf = (float*)attnAlloc(sizeof(float) * n);
  moe_bf16* xb = (moe_bf16*)attnAlloc(sizeof(moe_bf16) * n);
  moeInitUniform(xf, n, seed + 7, sqrtf(3.0f), g->stream);
  moeCastBf16(xf, xb, n, g->stream);
  attnStepT<moe_bf16>(g, a, xb, true, 0.0f);
  attnStepT<moe_bf16>(g, &u, xb, true, 0.0f);
  hapiCheck(hapiStreamSynchronize(g->stream));
  std::vector<float> ha, hu, qa, qu, oa, ou;
  toHost(ha, a->h, n, true); toHost(hu, u.h, n, true);
  toHost(qa, a->gWqkv, nq, false); toHost(qu, u.gWqkv, nq, false);
  toHost(oa, a->gWo, no, false); toHost(ou, u.gWo, no, false);
  const double eh = relL2(ha.data(), hu.data(), n), eq = relL2(qa.data(), qu.data(), nq), eo = relL2(oa.data(), ou.data(), no);
  printf("Attention self-check, fused against unfused (one step, lr 0): relative L2 error h %.2e, dWqkv %.2e, dWo %.2e\n",
      eh, eq, eo);
  if (!(eh < 5e-2 && eq < 5e-2 && eo < 5e-2)) {
    fprintf(stderr, "Attention self-check failed: the fused kernels disagree with the unfused ones\n");
    abort();
  }
  hapiCheck(hapiFree(xf));
  hapiCheck(hapiFree(xb));
  attnFree(&u);
}
#endif

void moeAttnInit(MoeGpu* g, MoeAttn* a, int Ttok, int d, int H, int S, uint64_t seed) {
  attnInit(g, a, Ttok, d, H, S, seed, true);
  printf("Attention: %s\n", moeAttnKernels(g->bf16));
#if MOE_FUSED_ATTN
  if (a->fused) attnSelfCheck(g, a, seed);
#endif
}

template <class T>
static void attnStepT(MoeGpu* g, MoeAttn* a, const void* x, bool train, float lr) {
  hapiBlasHandle_t hb = (hapiBlasHandle_t)g->blas;
  const hapiDataType_t ab = g->bf16 ? HAPI_R_16BF : HAPI_R_32F;
  const int Ttok = a->T, d = a->d, H = a->H, S = a->S, dh = d / H;
  const int B = a->nseq * H;
  const size_t nTd = (size_t)Ttok * d;
  const long long sQ = (long long)S * dh, sP = (long long)S * S;
  const float scale = 1.0f / sqrtf((float)dh);
  // forward: qkv = x Wqkv, o = Attn(q, k, v), h = o Wo
  gemm_rm_ex(hb, false, false, Ttok, 3 * d, d, 1.0f, x, d, a->Wqkv, 3 * d, ab, 0.0f, a->qkv, ab, 3 * d);
#if MOE_FUSED_ATTN
  if (a->fused) {
    attnFusedForward(a, train);
  } else
#endif
  {
    toHeadsKernel<T><<<nblocks(nTd), BLOCK_1D, 0, g->stream>>>((const T*)a->qkv, (T*)a->qh, Ttok, d, H, S, 0, 3 * d);
    toHeadsKernel<T><<<nblocks(nTd), BLOCK_1D, 0, g->stream>>>((const T*)a->qkv, (T*)a->kh, Ttok, d, H, S, d, 3 * d);
    toHeadsKernel<T><<<nblocks(nTd), BLOCK_1D, 0, g->stream>>>((const T*)a->qkv, (T*)a->vh, Ttok, d, H, S, 2 * d, 3 * d);
    gemm_rm_batched_ex(hb, false, true, S, S, dh, 1.0f, a->qh, dh, sQ, a->kh, dh, sQ, ab, 0.0f, a->scores, HAPI_R_32F, S, sP, B);
    causalSoftmaxKernel<T><<<(unsigned)((size_t)B * S), 256, 0, g->stream>>>(a->scores, (T*)a->p, S, scale);
    gemm_rm_batched_ex(hb, false, false, S, dh, S, 1.0f, a->p, S, sP, a->vh, dh, sQ, ab, 0.0f, a->oh, ab, dh, sQ, B);
    mergeHeadsKernel<T><<<nblocks(nTd), BLOCK_1D, 0, g->stream>>>((const T*)a->oh, (T*)a->o, Ttok, d, H, S, 0, d);
  }
  gemm_rm_ex(hb, false, false, Ttok, d, d, 1.0f, a->o, d, a->Wo, d, ab, 0.0f, a->h, ab, d);
  hapiCheck(hapiPeekAtLastError());
  if (!train) return;
  // backward of 1/2 |h|^2: dh = h
  gemm_rm_ex(hb, true, false, d, d, Ttok, 1.0f, a->o, d, a->h, d, ab, 0.0f, a->gWo, HAPI_R_32F, d);
#if MOE_FUSED_ATTN
  if (a->fused) {
    // do = h Wo^T into doh (o stays: the backward reads it), then dqkv straight from the graph
    gemm_rm_ex(hb, false, true, Ttok, d, d, 1.0f, a->h, d, a->Wo, d, ab, 0.0f, a->doh, ab, d);
    attnFusedBackward(a);
  } else
#endif
  {
    gemm_rm_ex(hb, false, true, Ttok, d, d, 1.0f, a->h, d, a->Wo, d, ab, 0.0f, a->o, ab, d);   // do, over o
    toHeadsKernel<T><<<nblocks(nTd), BLOCK_1D, 0, g->stream>>>((const T*)a->o, (T*)a->doh, Ttok, d, H, S, 0, d);
    gemm_rm_batched_ex(hb, true, false, S, dh, S, 1.0f, a->p, S, sP, a->doh, dh, sQ, ab, 0.0f, a->dvh, ab, dh, sQ, B);   // dV = P^T dO
    gemm_rm_batched_ex(hb, false, true, S, S, dh, 1.0f, a->doh, dh, sQ, a->vh, dh, sQ, ab, 0.0f, a->scores, HAPI_R_32F, S, sP, B); // dP = dO V^T
    softmaxBackwardKernel<T><<<(unsigned)((size_t)B * S), 256, 0, g->stream>>>(a->scores, (T*)a->p, S, scale);  // dz over p
    gemm_rm_batched_ex(hb, false, false, S, dh, S, 1.0f, a->p, S, sP, a->kh, dh, sQ, ab, 0.0f, a->dqh, ab, dh, sQ, B);   // dQ = dZ K
    gemm_rm_batched_ex(hb, true, false, S, dh, S, 1.0f, a->p, S, sP, a->qh, dh, sQ, ab, 0.0f, a->dkh, ab, dh, sQ, B);    // dK = dZ^T Q
    mergeHeadsKernel<T><<<nblocks(nTd), BLOCK_1D, 0, g->stream>>>((const T*)a->dqh, (T*)a->dqkv, Ttok, d, H, S, 0, 3 * d);
    mergeHeadsKernel<T><<<nblocks(nTd), BLOCK_1D, 0, g->stream>>>((const T*)a->dkh, (T*)a->dqkv, Ttok, d, H, S, d, 3 * d);
    mergeHeadsKernel<T><<<nblocks(nTd), BLOCK_1D, 0, g->stream>>>((const T*)a->dvh, (T*)a->dqkv, Ttok, d, H, S, 2 * d, 3 * d);
  }
  gemm_rm_ex(hb, true, false, d, 3 * d, Ttok, 1.0f, x, d, a->dqkv, 3 * d, ab, 0.0f, a->gWqkv, HAPI_R_32F, 3 * d);
  sgdPlainKernel<T><<<nblocks((size_t)d * 3 * d), BLOCK_1D, 0, g->stream>>>((T*)a->Wqkv, a->gWqkv, (size_t)d * 3 * d, lr);
  sgdPlainKernel<T><<<nblocks((size_t)d * d), BLOCK_1D, 0, g->stream>>>((T*)a->Wo, a->gWo, (size_t)d * d, lr);
  hapiCheck(hapiPeekAtLastError());
}

void moeAttnStep(MoeGpu* g, MoeAttn* a, const void* x, bool train, float lr_over_n) {
  if (g->bf16) attnStepT<moe_bf16>(g, a, x, train, lr_over_n);
  else attnStepT<float>(g, a, x, train, lr_over_n);
}

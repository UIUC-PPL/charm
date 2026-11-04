#!/usr/bin/env python3
"""
moe_ref.py: an expert-parallel MoE layer in PyTorch, the reference the Charm++
example in ../moe.C is compared against.

Same layer, routing, arithmetic and checksums as the example (the routing hash,
the init hash, the chunked SGD order and the loss are ports of moe.C/moe.cu),
written the way an MoE training framework writes it:

  - one process per GPU, torch.distributed over NCCL;
  - dispatch and combine are one all_to_all_single each, every token bundled
    per destination rank (not one message per expert);
  - the host runs a step ahead of the device (double-buffered slabs, the next
    step's token counts exchanged before this step's combine is issued);
  - load balancing by expert re-placement between steps, in two flavours:
      --lb sync   EPLB-style: at an LB step the placement is recomputed from
                  the step's token counts and the weights are moved with NCCL
                  point-to-point sends before the step starts (a full stop);
      --lb async  the same decision, but the weights move after the old
                  owner's compute of that step, overlapped with the rest of
                  the step, and the placement takes effect at the next step.
    --place greedy moves as few experts as needed (pairwise transfers from
    the most to the least loaded rank); --place lpt repacks all experts
    largest-first with an equal count per rank, EPLB's packing.

Every Step line matches the example's format so the same scripts parse both.
Checksums: out2 is the sum of squares of every rank's combined output, w2 the
sum of squares of every expert's W1 and W2 after the step's update, both in
double; they agree with the example's to fp32 accumulation-order noise.
"""
import argparse
import itertools
import math
import os
import sys
import time

import numpy as np
import torch
import torch.distributed as dist

# ---------------------------------------------------------------- hashing
# Ports of sm64/hmix/u01 (moe.C, host) and splitmix64/u01 (moe.cu, device).
MASK = (1 << 64) - 1
C1 = 0x9E3779B97F4A7C15
C2 = 0xBF58476D1CE4E5B9
C3 = 0x94D049BB133111EB
C4 = 0x632BE59BD9B4E019
U = np.uint64


def sm64(x):
    x = (x + C1) & MASK
    x = ((x ^ (x >> 30)) * C2) & MASK
    x = ((x ^ (x >> 27)) * C3) & MASK
    return x ^ (x >> 31)


def hmix(a, b):
    return sm64(a ^ ((b * C1 + C4) & MASK))


def sm64_np(x):
    x = x + U(C1)
    x = (x ^ (x >> U(30))) * U(C2)
    x = (x ^ (x >> U(27))) * U(C3)
    return x ^ (x >> U(31))


def hmix_np(a, b):
    return sm64_np(a ^ (b * U(C1) + U(C4)))


def u01_np(h):
    return ((h >> U(11)).astype(np.float64) + 0.5) / 9007199254740992.0


def i64(u):
    """The int64 with the same bits as the uint64 u (torch has no uint64 math)."""
    return u - (1 << 64) if u >= (1 << 63) else u


def lsr(x, s):
    """Logical right shift of an int64 tensor holding uint64 bits."""
    return (x >> s) & ((1 << (64 - s)) - 1)


def splitmix64_t(x):
    x = x + i64(C1)
    x = (x ^ lsr(x, 30)) * i64(C2)
    x = (x ^ lsr(x, 27)) * i64(C3)
    return x ^ lsr(x, 31)


def init_uniform(n, seed, bound, device, dtype=torch.float32, block=1 << 24):
    """moeInitUniform: p[i] = bound * (2 u - 1), u from two splitmix rounds of
    (seed, i). Computed in blocks so the int64 temporaries stay small."""
    out = torch.empty(n, dtype=dtype, device=device)
    for i0 in range(0, n, block):
        i = torch.arange(i0, min(n, i0 + block), dtype=torch.int64, device=device)
        h = splitmix64_t((i * i64(C1)) ^ i64(seed))
        h = splitmix64_t(h + i)
        v = lsr(h, 40).to(torch.float32)
        u = (v + 0.5) * (1.0 / 16777216.0)
        out[i0:i0 + i.numel()] = (bound * (2.0 * u - 1.0)).to(dtype)
    return out


# ---------------------------------------------------------------- routing
class Router:
    """Dispatcher::route(): a pure function of (seed, step, pe, token, slot)."""

    def __init__(self, E, T, K, zipf, drift, seed, pe):
        self.E, self.T, self.K = E, T, K
        self.zipf, self.drift, self.seed, self.pe = zipf, drift, seed, pe
        self.cur_phase = -1

    def build_phase(self, phase):
        E = self.E
        perm = list(range(E))
        for i in range(E - 1, 0, -1):
            h = hmix(hmix(self.seed, 77), phase * E + i)
            j = h % (i + 1)
            perm[i], perm[j] = perm[j], perm[i]
        cdf = []
        s = 0.0
        for r in range(E):
            s += math.pow(r + 1, -self.zipf) if self.zipf > 0.0 else 1.0
            cdf.append(s)
        self.cdf = np.array([c / s for c in cdf], dtype=np.float64)
        self.perm = np.array(perm, dtype=np.int64)

    def route(self, step):
        E, T, K = self.E, self.T, self.K
        phase = (step - 1) // self.drift if self.drift > 0 else 0
        if phase != self.cur_phase:
            self.cur_phase = phase
            self.build_phase(phase)
        base = hmix(hmix(self.seed, step), self.pe)
        with np.errstate(over="ignore"):
            ht = hmix_np(U(base), np.arange(T, dtype=U))
            chosen = np.full((T, K), -1, dtype=np.int64)
            for j in range(K):
                hj = hmix_np(ht, U(j))
                pending = np.ones(T, dtype=bool)
                for attempt in range(16):
                    idx = np.flatnonzero(pending)
                    if idx.size == 0:
                        break
                    h = hmix_np(hj[idx], U(attempt))
                    r = np.searchsorted(self.cdf, u01_np(h), side="right")
                    cand = self.perm[np.minimum(r, E - 1)]
                    ok = np.ones(idx.size, dtype=bool)
                    for q in range(j):
                        ok &= chosen[idx, q] != cand
                    chosen[idx[ok], j] = cand[ok]
                    pending[idx[ok]] = False
                for ti in np.flatnonzero(pending):
                    e = (chosen[ti, j - 1] + 1) % E
                    while e in chosen[ti, :j]:
                        e = (e + 1) % E
                    chosen[ti, j] = e
        slot_expert = chosen.reshape(-1)  # (token, slot) order
        counts = np.bincount(slot_expert, minlength=E).astype(np.int64)
        return slot_expert, counts


TRACE_MAGIC = 0x4D4F4554


class TraceRouter:
    """Routing replayed from a recorded trace (trace_routing.py): token t of
    rank pe at step s is text token ((s - 1) * P + pe) * T + t, modulo the
    trace length, the same rule as the example's -E, so every implementation
    routes the same tokens. perm[0] is the hottest expert over the text and
    p_max the largest share any expert takes of one rank's tokens in one step."""

    def __init__(self, path, E, T, K, pe, P):
        hdr = np.fromfile(path, dtype=np.int32, count=4)
        if hdr.size < 4 or hdr[0] != TRACE_MAGIC:
            sys.exit("--trace %s: not a routing trace" % path)
        if hdr[1] != E or hdr[2] != K:
            sys.exit("--trace %s: the trace has %d experts and top-%d, the run %d and top-%d" % (
                path, hdr[1], hdr[2], E, K))
        self.E, self.T, self.K, self.pe, self.P = E, T, K, pe, P
        self.n = int(hdr[3])
        self.ids = np.fromfile(path, dtype=np.int32, offset=16).reshape(self.n, K)
        cnt = np.bincount(self.ids.reshape(-1), minlength=E)
        self.perm = np.argsort(-cnt, kind="stable")
        self.share_max = cnt.max() / cnt.sum()
        nchunks = self.n // T
        chunks = self.ids[:nchunks * T].reshape(nchunks, T * K)
        self.p_max = max(np.bincount(c, minlength=E).max() for c in chunks) / float(T * K)
        self.path = path

    def build_phase(self, phase):
        pass

    def route(self, step):
        g0 = ((step - 1) * self.P + self.pe) * self.T
        idx = (g0 + np.arange(self.T)) % self.n
        slot_expert = self.ids[idx].reshape(-1).astype(np.int64)
        counts = np.bincount(slot_expert, minlength=self.E).astype(np.int64)
        return slot_expert, counts


def make_router(a, pe, P):
    """The example's router for this rank: the replayed trace (--trace) or the hashed Zipf."""
    if getattr(a, "trace", None):
        return TraceRouter(a.trace, a.experts, a.tokens, a.topk, pe, P)
    return Router(a.experts, a.tokens, a.topk, a.zipf, a.drift, a.seed, pe)


def routing_line(a, router):
    """The 'Routing:' line of the header, as the example prints it."""
    E = a.experts
    if getattr(a, "trace", None):
        return "Routing: trace %s (%d tokens; hottest expert %.1fx average over the text, %.1fx in its hottest step; replayed in text order, a chunk of %d tokens per rank and step)" % (
            router.path, router.n, router.share_max * E, router.p_max * E, a.tokens)
    H = sum(math.pow(r + 1, -a.zipf) for r in range(E)) if a.zipf > 0 else E
    return "Routing: zipf %.2f (hottest expert %.1fx average), hot set reshuffled every %d steps, seed %d" % (
        a.zipf, E / H, a.drift, a.seed)


class Attention(torch.nn.Module):
    """The block's attention sublayer, the same in every harness (--attn): q, k, v
    and o projections (d_model x d_model, no bias) and causal scaled-dot-product
    attention with `heads` heads over sequences of `seq` tokens (a rank's tokens
    are tokens/seq sequences). It is trained on its own loss 1/2 |h|^2 with SGD,
    and the MoE sublayer takes h detached, so the MoE layer's contract (weight
    gradients, no input gradient) is unchanged and the attention work, local to
    the rank, is identical in every implementation. The example's -w does the
    same with cuBLAS."""

    def __init__(self, d_model, heads, seq, dtype, device, seed):
        super().__init__()
        g = torch.Generator(device="cpu").manual_seed(seed)
        bound = 1.0 / math.sqrt(d_model)
        self.w = torch.nn.Parameter(
            (torch.rand(4, d_model, d_model, generator=g) * 2 - 1).mul_(bound).to(device=device, dtype=dtype))
        self.heads, self.seq, self.d = heads, seq, d_model

    def forward(self, x):
        T, d = x.shape
        S, H = self.seq, self.heads
        q = (x @ self.w[0]).view(T // S, S, H, d // H).transpose(1, 2)
        k = (x @ self.w[1]).view(T // S, S, H, d // H).transpose(1, 2)
        v = (x @ self.w[2]).view(T // S, S, H, d // H).transpose(1, 2)
        o = torch.nn.functional.scaled_dot_product_attention(q, k, v, is_causal=True)
        return o.transpose(1, 2).reshape(T, d) @ self.w[3]


def master_sgd_step(params, masters, lr):
    """The bf16 optimizer's step as Megatron-LM's does it (and the example's experts): the
    gradients cast to fp32, the SGD step on the fp32 master copies, the masters cast back
    into the bf16 parameters. An update below 2^-9 of a weight is rounded away by a bf16
    parameter updated in place, which is why bf16 training keeps masters."""
    with torch.no_grad():
        for p, m in zip(params, masters):
            if p.grad is None:
                continue   # outside the step's graph (Megatron's router weight under the replayed routing)
            m.add_(p.grad, alpha=-lr)   # the bf16 gradient promoted into the fp32 master
            p.copy_(m)
            p.grad = None


def attention_step(attn, opt, x, lr_scale, serving):
    """One attention sublayer step: forward, and unless serving its loss, backward and
    update. Returns the detached output, the MoE sublayer's input."""
    if serving:
        with torch.no_grad():
            return attn(x)
    h = attn(x)
    (0.5 * h.float().square().sum() * lr_scale).backward()
    opt.step()
    opt.zero_grad(set_to_none=True)
    return h.detach()


def add_attention_args(ap):
    ap.add_argument("--attn", action="store_true",
                    help="a transformer block: an attention sublayer (q/k/v/o, causal SDPA) before the MoE sublayer")
    ap.add_argument("--heads", type=int, default=32)
    ap.add_argument("--seq", type=int, default=1024, help="sequence length the rank's tokens are split into")


# ---------------------------------------------------------------- placement
def place_greedy(owner, load, P, tol=0.02, owner2=None):
    """Pairwise transfers from the most to the least loaded rank until the gap
    is within tol of the average; each move takes the expert whose load is
    closest to half the gap. Few moves, like a diffusion balancer. A
    replicated expert (owner2 >= 0) is pinned and counts half on each rank."""
    owner = owner.copy()
    load = load.copy()
    extra = np.zeros(P)
    pinned = np.zeros(len(load), dtype=bool)
    if owner2 is not None:
        for e in np.flatnonzero(owner2 >= 0):
            pinned[e] = True
            load[e] *= 0.5
            extra[owner2[e]] += load[e]
    R = np.bincount(owner, weights=load, minlength=P) + extra
    avg = load.sum() / P
    while True:
        hi, lo = int(R.argmax()), int(R.argmin())
        gap = R[hi] - R[lo]
        if gap <= tol * avg:
            break
        cands = np.flatnonzero((owner == hi) & ~pinned)
        cands = cands[load[cands] < gap]
        if cands.size == 0:
            break
        e = int(cands[np.argmin(np.abs(load[cands] - gap / 2))])
        owner[e] = lo
        R[hi] -= load[e]
        R[lo] += load[e]
    return owner


def place_lpt(owner, load, P):
    """EPLB's packing: experts largest-first onto the least loaded rank with
    room, an equal number per rank; rank labels then permuted to keep as many
    experts in place as possible."""
    E = len(load)
    cap = (E + P - 1) // P
    order = np.argsort(-load, kind="stable")
    R = np.zeros(P)
    cnt = np.zeros(P, dtype=np.int64)
    new = np.zeros(E, dtype=np.int64)
    for e in order:
        r = int(np.argmin(np.where(cnt < cap, R, np.inf)))
        new[e] = r
        R[r] += load[e]
        cnt[r] += 1
    best, best_keep = None, -1
    for perm in itertools.permutations(range(P)):
        relab = np.array(perm)[new]
        keep = int((relab == owner).sum())
        if keep > best_keep:
            best, best_keep = relab, keep
    return best


# ---------------------------------------------------------------- the layer
class Lane:
    def __init__(self, chunk, dff, dtype, device, adam, dm, fuse_rows):
        self.stream = torch.cuda.Stream()
        with torch.cuda.stream(self.stream):
            self.h = torch.empty(chunk, dff, dtype=dtype, device=device)
            self.dh = torch.empty(chunk, dff, dtype=dtype, device=device)
            # One gradient accumulator pair per source partition (even and odd
            # ranks), as the Charm++ app keeps per dispatcher partition: the
            # step's update is applied once from their sum. fp32 whatever the
            # operand dtype, as the app's accumulators are.
            self.grads = [(torch.zeros(dm, dff, dtype=torch.float32, device=device),
                           torch.zeros(dff, dm, dtype=torch.float32, device=device)) for _ in range(2)]
            self.fx = self.fy = None
            self.fuse = fuse_rows
            self.dm, self.dtype, self.device = dm, dtype, device

    def fused(self, rows):
        """Scratch for one expert's tokens, grown on demand: the worst case
        (every token of every rank on one expert) is several GB at Llama size
        and never happens."""
        if self.fx is None or self.fx.shape[0] < rows:
            n = max(rows, 2 * (0 if self.fx is None else self.fx.shape[0]))
            self.fx = torch.empty(n, self.dm, dtype=self.dtype, device=self.device)
            self.fy = torch.empty(n, self.dm, dtype=self.dtype, device=self.device)
        return self.fx[:rows], self.fy[:rows]


def expert_chunks(lane, W1, W2, x, y, chunk, grads, zero_first, serve=False):
    """moeExpertChunks: per chunk h = relu(x W1), y = h W2, dh = y W2^T,
    dz = dh * (h > 0), dW2 += h^T y, dW1 += x^T dz into grads=(dW1, dW2) with
    alpha 1 (the first chunk overwrites when zero_first). The weights are
    read only; the update is applied once per step (apply_sgd / adam_apply).
    W1/W2 are the operands the GEMMs read: the masters in fp32, their bf16
    copies under --dtype bf16, where the gradient GEMMs write fp32 from bf16
    operands (torch.mm out_dtype) into the fp32 accumulators, as the app's
    hapiBlasGemmEx does."""
    n = x.shape[0]
    bf16 = x.dtype == torch.bfloat16
    for c0 in range(0, n, chunk):
        nc = min(chunk, n - c0)
        xc = x[c0:c0 + nc]
        yc = y[c0:c0 + nc]
        h = lane.h[:nc]
        dh = lane.dh[:nc]
        torch.mm(xc, W1, out=h)
        h.relu_()
        torch.mm(h, W2, out=yc)
        if serve:
            continue
        torch.mm(yc, W2.t(), out=dh)
        dh.masked_fill_(h <= 0, 0.0)
        first = zero_first and c0 == 0
        if bf16:
            g2 = torch.mm(h.t(), yc, out_dtype=torch.float32)
            g1 = torch.mm(xc.t(), dh, out_dtype=torch.float32)
            if first:
                grads[1].copy_(g2)
                grads[0].copy_(g1)
            else:
                grads[1].add_(g2)
                grads[0].add_(g1)
        else:
            beta = 0.0 if first else 1.0
            grads[1].addmm_(h.t(), yc, beta=beta, alpha=1.0)
            grads[0].addmm_(xc.t(), dh, beta=beta, alpha=1.0)


def grad_sum(gA, gB, inv_n):
    """The step's gradient: the two partitions' accumulators (either None
    when its partition sent nothing) added in one order, scaled by 1/n."""
    if gA is None:
        g = gB.clone()
    else:
        g = gA.clone()
        if gB is not None:
            g.add_(gB)
    g.mul_(inv_n)
    return g


def apply_sgd(W, gA, gB, lr_over_n):
    W.sub_(grad_sum(gA, gB, lr_over_n))


def adam_apply(W, m, v, g, lr, t):
    b1, b2, eps = 0.9, 0.999, 1e-8
    c1 = 1.0 / (1.0 - b1 ** t)
    c2 = 1.0 / (1.0 - b2 ** t)
    m.mul_(b1).add_(g, alpha=1.0 - b1)
    v.mul_(b2).addcmul_(g, g, value=1.0 - b2)
    W.addcdiv_(m * c1, (v * c2).sqrt_().add_(eps), value=-lr)


class Expert:
    """W1/W2 are the fp32 master weights (what moves, what the checksum reads);
    W1b/W2b their bf16 copies under --dtype bf16, rewritten after every update
    and remade on arrival, as the app's."""
    __slots__ = ("W1", "W2", "W1b", "W2b", "m1", "v1", "m2", "v2", "adam_t")

    def __init__(self):
        self.W1 = self.W2 = None
        self.W1b = self.W2b = None
        self.m1 = self.v1 = self.m2 = self.v2 = None
        self.adam_t = 0

    def tensors(self, adam):
        ts = [self.W1, self.W2]
        if adam:
            ts += [self.m1, self.v1, self.m2, self.v2]
        return ts

    def read(self):
        """The operands the GEMMs read."""
        return (self.W1b, self.W2b) if self.W1b is not None else (self.W1, self.W2)

    def arrived(self, dtype):
        """The masters are in place (init or a migration): make the copies."""
        if dtype == torch.bfloat16:
            self.W1b = self.W1.to(torch.bfloat16)
            self.W2b = self.W2.to(torch.bfloat16)

    def updated(self):
        if self.W1b is not None:
            self.W1b.copy_(self.W1)
            self.W2b.copy_(self.W2)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-e", "--experts", type=int, default=64)
    ap.add_argument("-m", "--d-model", type=int, default=2048)
    ap.add_argument("-H", "--d-ff", type=int, default=8192)
    ap.add_argument("-t", "--tokens", type=int, default=8192, help="per rank per step")
    ap.add_argument("-k", "--topk", type=int, default=1)
    ap.add_argument("-i", "--steps", type=int, default=50, help="timed steps")
    ap.add_argument("-u", "--warmup", type=int, default=5)
    ap.add_argument("-z", "--zipf", type=float, default=1.0)
    ap.add_argument("--trace", help="routing trace to replay (trace_routing.py) instead of the hashed Zipf")
    ap.add_argument("-p", "--drift", type=int, default=10)
    ap.add_argument("-c", "--chunk", type=int, default=1024)
    ap.add_argument("-n", "--lanes", type=int, default=4, help="compute streams per rank")
    ap.add_argument("-L", "--lr", type=float, default=1e-4)
    ap.add_argument("-C", "--checksum", type=int, default=5, help="checksum every C steps, 0 off")
    ap.add_argument("-f", "--first-lb", type=int, default=10)
    ap.add_argument("-b", "--lb-freq", type=int, default=9999)
    ap.add_argument("-S", "--seed", type=int, default=12345)
    ap.add_argument("--lb", choices=["none", "sync", "async"], default="none")
    ap.add_argument("--place", choices=["greedy", "lpt"], default="greedy")
    ap.add_argument("-A", "--adam", action="store_true")
    ap.add_argument("-T", "--tf32", action="store_true")
    ap.add_argument("--dtype", choices=["fp32", "bf16"], default="fp32",
                    help="bf16: tokens, activations and the weights' copies the GEMMs read are bf16, "
                         "with fp32 accumulation, fp32 master weights, moments and gradient "
                         "accumulators (the app's -B)")
    ap.add_argument("--fuse", action="store_true",
                    help="one chunked pass per expert over all sources (changes chunk boundaries)")
    ap.add_argument("--replicate", action="store_true",
                    help="EPLB-style redundancy: the hottest expert of the (static) hot set has a "
                         "second copy on another rank; even ranks' tokens go to the first copy, odd "
                         "ranks' to the second; the copies all-reduce their gradients and apply the "
                         "same update. Needs -p 0.")
    ap.add_argument("-P", "--print-place", action="store_true")
    ap.add_argument("--no-pipeline", action="store_true",
                    help="synchronize the host at every step end (no run-ahead)")
    ap.add_argument("-G", "--forward-only", action="store_true",
                    help="serving: forward only, no gradient GEMMs, no update (the example's -G)")
    add_attention_args(ap)
    a = ap.parse_args()

    rank = int(os.environ.get("RANK", os.environ.get("SLURM_PROCID", 0)))
    world = int(os.environ.get("WORLD_SIZE", os.environ.get("SLURM_NTASKS", 1)))
    local = int(os.environ.get("LOCAL_RANK", os.environ.get("SLURM_LOCALID", rank)))
    if torch.cuda.device_count() == 1:
        local = 0
    device = torch.device("cuda", local)
    torch.cuda.set_device(device)
    dist.init_process_group("nccl", rank=rank, world_size=world, device_id=device)
    P = world
    E, dm, dff, T, K = a.experts, a.d_model, a.d_ff, a.tokens, a.topk
    TK = T * K
    if a.tf32:
        torch.backends.cuda.matmul.allow_tf32 = True
    dtype = torch.bfloat16 if a.dtype == "bf16" else torch.float32
    esz = 2 if dtype == torch.bfloat16 else 4
    if a.lb != "none" and a.first_lb <= a.warmup:
        sys.exit("the first LB step must come after the warmup")

    def log(*s):
        if rank == 0:
            print(*s, flush=True)

    # ---- placement and weights: expert e starts on rank e * P // E (block map)
    owner = np.array([(e * P) // E for e in range(E)], dtype=np.int64)
    # Redundancy (--replicate): owner2[e] is the rank of expert e's second
    # copy, -1 for none. Tokens from even ranks go to owner[e], from odd ranks
    # to owner2[e]; the two copies all-reduce their gradient partials and apply
    # the same update, so their weights stay identical. Neither copy moves.
    owner2 = np.full(E, -1, dtype=np.int64)
    pair_group = None
    if a.replicate:
        if a.drift != 0 and not a.trace:
            sys.exit("--replicate needs a static hot set (-p 0)")
        if P < 2:
            sys.exit("--replicate needs at least two ranks")
        r0 = make_router(a, 0, P)
        r0.build_phase(0)
        hot = int(r0.perm[0])
        owner2[hot] = (int(owner[hot]) + P // 2) % P
        pair_group = dist.new_group([int(owner[hot]), int(owner2[hot])])
        log("Replication: expert %d has copies on ranks %d (even sources) and %d (odd sources)" % (
            hot, owner[hot], owner2[hot]))

    def serves(src, e):
        """Whether this rank computes expert e's tokens from rank src."""
        if owner2[e] >= 0 and src % 2 == 1:
            return owner2[e] == rank
        return owner[e] == rank

    def owners_for(src):
        """Effective owner per expert for the tokens rank src sends."""
        if src % 2 == 1:
            return np.where(owner2 >= 0, owner2, owner)
        return owner

    experts = {}
    for e in range(E):
        if owner[e] == rank or owner2[e] == rank:
            x = Expert()
            x.W1 = init_uniform(dm * dff, hmix(hmix(a.seed, 1), e), 1.0 / math.sqrt(dm),
                                device, torch.float32).view(dm, dff)
            x.W2 = init_uniform(dm * dff, hmix(hmix(a.seed, 2), e), 1.0 / math.sqrt(dff),
                                device, torch.float32).view(dff, dm)
            if a.adam:
                x.m1, x.v1 = torch.zeros_like(x.W1), torch.zeros_like(x.W1)
                x.m2, x.v2 = torch.zeros_like(x.W2), torch.zeros_like(x.W2)
            x.arrived(dtype)
            experts[e] = x
    x_in = init_uniform(T * dm, hmix(hmix(a.seed, 1000), rank), math.sqrt(3.0),
                        device, dtype).view(T, dm)
    attn = opt_a = None
    if a.attn:
        attn = Attention(dm, a.heads, a.seq, dtype, device, hmix(a.seed, 2000))
        opt_a = torch.optim.SGD(attn.parameters(), lr=a.lr / T)
        x_tok = x_in

    main = torch.cuda.current_stream()
    # A rank can receive every token of every rank in the worst case.
    cap_rows = P * TK
    lanes = [Lane(a.chunk, dff, dtype, device, a.adam, dm, a.fuse) for _ in range(a.lanes)]
    mig_stream = torch.cuda.Stream()
    # Slabs, double-buffered so the host can issue step s+1 while s runs.
    send_buf = [torch.empty(TK, dm, dtype=dtype, device=device) for _ in range(2)]
    recv_buf = [torch.empty(cap_rows, dm, dtype=dtype, device=device) for _ in range(2)]
    out_buf = [torch.empty(cap_rows, dm, dtype=dtype, device=device) for _ in range(2)]
    back_buf = [torch.empty(TK, dm, dtype=dtype, device=device) for _ in range(2)]
    y = torch.empty(T, dm, dtype=torch.float32, device=device)   # the combine sums in fp32
    router = make_router(a, rank, P)

    n_total = a.warmup + a.steps
    wbytes = 4 * dm * dff * 2 * (3 if a.adam else 1) + (esz * dm * dff * 2 if esz != 4 else 0)
    H = sum(math.pow(r + 1, -a.zipf) for r in range(E)) if a.zipf > 0 else E
    p_max = 1.0 / H
    log("\n[PyTorch expert-parallel MoE reference]")
    log("Experts: %d, d_model %d, d_ff %d, %s, %s%s" % (
        E, dm, dff, "Adam" if a.adam else "SGD", a.dtype, " TF32" if a.tf32 else ""))
    log("Tokens: %d per rank per step on %d ranks, top-%d, chunk %d, %d compute lanes%s%s" % (
        T, P, K, a.chunk, a.lanes, ", fused sources" if a.fuse else "",
        ", no run-ahead" if a.no_pipeline else ""))
    log(routing_line(a, router))
    log("Per expert: %.1f MB weights; torch %s, NCCL %s, NCCL_P2P_LEVEL %s" % (
        wbytes / 1e6, torch.__version__, ".".join(map(str, torch.cuda.nccl.version())),
        os.environ.get("NCCL_P2P_LEVEL", "default")))
    log("Steps: %d (+%d warmup), LB: %s%s, first LB: %d, LB frequency: %d%s%s" % (
        a.steps, a.warmup, a.lb, "" if a.lb == "none" else " (" + a.place + ")",
        a.first_lb, a.lb_freq, "; forward only (serving)" if a.forward_only else "",
        "; attention sublayer (%d heads, sequences of %d) before the MoE" % (a.heads, a.seq) if a.attn else ""))

    def is_lb_step(s):
        return a.lb != "none" and (s == a.first_lb or (a.lb_freq > 0 and s > a.first_lb
                                                       and s % a.lb_freq == 0))

    def migrate(moves, stream):
        """Issue NCCL point-to-point moves of expert weights on `stream`.
        Returns (requests, [(e, new tensors)] for the experts arriving here)."""
        ops = []
        arrivals = []
        for (e, src, dst) in moves:
            if src == rank:
                for t in experts[e].tensors(a.adam):
                    ops.append(dist.P2POp(dist.isend, t, dst))
            elif dst == rank:
                x = Expert()
                x.W1 = torch.empty(dm, dff, dtype=torch.float32, device=device)
                x.W2 = torch.empty(dff, dm, dtype=torch.float32, device=device)
                if a.adam:
                    x.m1, x.v1 = torch.empty_like(x.W1), torch.empty_like(x.W1)
                    x.m2, x.v2 = torch.empty_like(x.W2), torch.empty_like(x.W2)
                for t in x.tensors(a.adam):
                    ops.append(dist.P2POp(dist.irecv, t, src))
                arrivals.append((e, x))
        reqs = []
        if ops:
            with torch.cuda.stream(stream):
                reqs = dist.batch_isend_irecv(ops)
        return reqs, arrivals

    def exchange_counts(s):
        """Route step s and start the all_gather of every rank's counts."""
        t0 = time.perf_counter()
        slot_expert, counts = router.route(s)
        cnt_t = torch.from_numpy(counts).to(device)
        all_cnt = torch.empty(P, E, dtype=torch.int64, device=device)
        dist.all_gather_into_tensor(all_cnt, cnt_t)
        return slot_expert, counts, all_cnt, (time.perf_counter() - t0) * 1e3

    def layout(slot_expert, counts, owner):
        """Slab layout for one step: positions sorted by (owner, expert,
        token, slot); the per-rank split sizes."""
        key = owner[slot_expert] * E + slot_expert
        order = np.argsort(key, kind="stable")
        send_idx = order // K
        recv_pos = np.empty(TK, dtype=np.int64)
        recv_pos[order] = np.arange(TK)
        in_splits = np.bincount(owner, weights=counts, minlength=P).astype(np.int64)
        return send_idx, recv_pos, in_splits

    # ---- per-step records (event-timed on the main stream)
    ev = [[torch.cuda.Event(enable_timing=True) for _ in range(6)] for _ in range(n_total + 1)]
    step_tok = np.zeros((n_total + 1, 3))   # tokens: max per rank, max per expert, this rank
    n_moved = np.zeros(n_total + 1, dtype=np.int64)
    host_ms = np.zeros((n_total + 1, 2))    # routing, wait for the counts
    chk_out = {}
    chk_w = {}
    pending_reqs = []       # async migration in flight (waited at the next step)
    pending_owner = None    # placement to apply at the next step
    pending_moves = []
    total_moved = 0
    a2a_bytes = 0.0

    # ---- the pipeline: the counts of step s+1 are exchanged before the
    # combine of step s is issued, so the host prepares s+1 while s runs.
    nxt = exchange_counts(1)
    for s in range(1, n_total + 1):
        b = s % 2
        slot_expert, counts, all_cnt, host_ms[s, 0] = nxt

        # migrations decided last step land now
        if pending_reqs:
            for r in pending_reqs:
                r.wait()
            pending_reqs = []
        if pending_owner is not None:
            for (e, src, dst, arr) in pending_moves:
                if src == rank:
                    del experts[e]
                elif dst == rank:
                    arr.arrived(dtype)
                    experts[e] = arr
            owner = pending_owner
            pending_owner, pending_moves = None, []

        t0 = time.perf_counter()
        C = all_cnt.cpu().numpy()      # the host waits for the all_gather here
        host_ms[s, 1] = (time.perf_counter() - t0) * 1e3
        load = C.sum(axis=0).astype(np.float64)

        moves = []
        if is_lb_step(s):
            new_owner = place_greedy(owner, load, P, owner2=owner2) if a.place == "greedy" \
                else place_lpt(owner, load, P)
            moves = [(e, int(owner[e]), int(new_owner[e])) for e in range(E)
                     if owner[e] != new_owner[e]]
            n_moved[s] = len(moves)
            total_moved += len(moves)

        ev[s][0].record(main)
        if moves and a.lb == "sync":
            reqs, arrivals = migrate(moves, main)
            for r in reqs:
                r.wait()
            for (e, src, dst) in moves:
                if src == rank:
                    del experts[e]
            for (e, x) in arrivals:
                x.arrived(dtype)
                experts[e] = x
            owner = new_owner
        ev[s][1].record(main)

        send_idx, recv_pos, in_splits = layout(slot_expert, counts, owners_for(rank))
        mine = [e for e in range(E) if owner[e] == rank or owner2[e] == rank]
        out_splits = np.array([int(sum(C[src][e] for e in mine if serves(src, e)))
                               for src in range(P)], dtype=np.int64)
        tot = int(out_splits.sum())
        my_tok = np.bincount(owner, weights=load, minlength=P)
        step_tok[s] = (my_tok.max(), load.max(), tot)
        if s > a.warmup:
            a2a_bytes += (tot + TK) * dm * esz

        # the attention sublayer (--attn) runs here, on the step's tokens, before they are gathered
        if attn is not None:
            x_in = attention_step(attn, opt_a, x_tok, 1.0, a.forward_only)
        # dispatch: gather rows in slab order, one all_to_all
        idx_t = torch.from_numpy(send_idx).to(device)
        torch.index_select(x_in, 0, idx_t, out=send_buf[b])
        work = dist.all_to_all_single(recv_buf[b][:tot], send_buf[b],
                                      output_split_sizes=out_splits.tolist(),
                                      input_split_sizes=in_splits.tolist(), async_op=True)
        work.wait()
        ev[s][2].record(main)

        # compute: segment offsets of (source, expert) in the receive slab
        seg = {}
        off = 0
        for src in range(P):
            for e in mine:
                if not serves(src, e):
                    continue
                seg[(src, e)] = off
                off += int(C[src][e])
        moving = set(e for (e, src, dst) in moves) if a.lb == "async" else set()
        order_e = sorted(mine, key=lambda e: (0 if e in moving else 1, e))
        for lane in lanes:
            lane.stream.wait_stream(main)
        mig_events = []
        for li, e in enumerate(order_e):
            lane = lanes[li % a.lanes]
            x = experts[e]
            n_tot = int(load[e])
            with torch.cuda.stream(lane.stream):
                if n_tot > 0:
                    # Two runs, one per source partition (even and odd ranks),
                    # each into its own accumulators; the update once from both.
                    # This is the app's partition structure (moe.C), so the
                    # bits agree whether or not it replicated the expert.
                    have = [False, False]
                    for part in range(2):
                        srcs = [src for src in range(P) if src % 2 == part and C[src][e] > 0
                                and serves(src, e)]
                        if not srcs:
                            continue
                        have[part] = True
                        g = lane.grads[part]
                        if a.fuse:
                            segs = [(seg[(src, e)], int(C[src][e])) for src in srcs]
                            n_run = sum(n for (_, n) in segs)
                            xs, ys = lane.fused(n_run)
                            o = 0
                            for (so, n) in segs:
                                xs[o:o + n].copy_(recv_buf[b][so:so + n])
                                o += n
                            expert_chunks(lane, *x.read(), xs, ys, a.chunk, g, True, a.forward_only)
                            o = 0
                            for (so, n) in segs:
                                out_buf[b][so:so + n].copy_(ys[o:o + n])
                                o += n
                        else:
                            first = True
                            for src in srcs:
                                n = int(C[src][e])
                                so = seg[(src, e)]
                                expert_chunks(lane, *x.read(), recv_buf[b][so:so + n],
                                              out_buf[b][so:so + n], a.chunk, g, first, a.forward_only)
                                first = False
                    if a.forward_only:
                        x.updated()
                        have = [False, False]
                    if owner2[e] >= 0 and not a.forward_only:
                        # The copies' partials, summed between them (NCCL on the
                        # pair's communicator, on this lane's stream): each copy
                        # holds one partition; an empty one contributes zeros.
                        part = 0 if owner[e] == rank else 1
                        g = lane.grads[part]
                        if not have[part]:
                            g[0].zero_()
                            g[1].zero_()
                        dist.all_reduce(g[0], group=pair_group)
                        dist.all_reduce(g[1], group=pair_group)
                        have = [part == 0, part == 1]
                    gA = lane.grads[0] if have[0] else (None, None)
                    gB = lane.grads[1] if have[1] else (None, None)
                    inv_n = 1.0 / n_tot
                    if a.forward_only:
                        pass
                    elif a.adam:
                        x.adam_t += 1
                        adam_apply(x.W1, x.m1, x.v1, grad_sum(gA[0], gB[0], inv_n), a.lr, x.adam_t)
                        adam_apply(x.W2, x.m2, x.v2, grad_sum(gA[1], gB[1], inv_n), a.lr, x.adam_t)
                    else:
                        apply_sgd(x.W1, gA[0], gB[0], a.lr * inv_n)
                        apply_sgd(x.W2, gA[1], gB[1], a.lr * inv_n)
                    if not a.forward_only:
                        x.updated()
                if e in moving:
                    evm = torch.cuda.Event()
                    evm.record(lane.stream)
                    mig_events.append(evm)
        # next step's routing and counts, queued ahead of this step's combine
        if s < n_total:
            nxt = exchange_counts(s + 1)
        if moves and a.lb == "async":
            # weights leave after their last update, overlapped with the rest
            for evm in mig_events:
                mig_stream.wait_event(evm)
            pending_reqs, arrivals = migrate(moves, mig_stream)
            arr_by_e = dict(arrivals)
            pending_moves = [(e, src, dst, arr_by_e.get(e)) for (e, src, dst) in moves]
            pending_owner = new_owner
        for lane in lanes:
            main.wait_stream(lane.stream)
        ev[s][3].record(main)

        # combine: the outputs go back the way the tokens came
        work = dist.all_to_all_single(back_buf[b], out_buf[b][:tot],
                                      output_split_sizes=in_splits.tolist(),
                                      input_split_sizes=out_splits.tolist(), async_op=True)
        work.wait()
        ev[s][4].record(main)
        pos_t = torch.from_numpy(recv_pos).to(device)
        if K == 1:
            y.copy_(back_buf[b].index_select(0, pos_t))
        else:
            torch.sum(back_buf[b].index_select(0, pos_t).view(T, K, dm).float(), dim=1, out=y)
            y.mul_(1.0 / K)
        if a.checksum > 0 and s % a.checksum == 0:
            chk_out[s] = y.double().square().sum()
            wsum = torch.zeros((), dtype=torch.float64, device=device)
            for e in mine:
                if owner[e] != rank:
                    continue   # the second copy holds the same weights
                wsum += experts[e].W1.double().square().sum() + experts[e].W2.double().square().sum()
            chk_w[s] = wsum
        ev[s][5].record(main)
        if s == a.warmup or a.no_pipeline:
            torch.cuda.synchronize()

    torch.cuda.synchronize()
    ev_end = ev[n_total][5]
    if pending_reqs:
        for r in pending_reqs:
            r.wait()
        torch.cuda.synchronize()

    # ---- results: event timings from every rank, checksums summed
    ph = np.zeros((n_total + 1, 6))
    for s in range(1, n_total + 1):
        ph[s, 0] = ev[s][0].elapsed_time(ev[s][1])   # sync migration
        ph[s, 1] = ev[s][1].elapsed_time(ev[s][2])   # dispatch all_to_all
        ph[s, 2] = ev[s][2].elapsed_time(ev[s][3])   # expert compute
        ph[s, 3] = ev[s][3].elapsed_time(ev[s][4])   # combine all_to_all
        ph[s, 4] = ev[s][4].elapsed_time(ev[s][5])   # combine kernel, checksum
        ph[s, 5] = ev[s][0].elapsed_time(ev[s + 1][0]) if s < n_total else \
            ev[s][0].elapsed_time(ev_end)             # step, as the main stream saw it
    t_start = ev[a.warmup][5] if a.warmup > 0 else ev[1][0]
    total_s = t_start.elapsed_time(ev_end) / 1e3
    ph_t = torch.from_numpy(ph).to(device)
    all_ph = [torch.empty_like(ph_t) for _ in range(P)]
    dist.all_gather(all_ph, ph_t)
    all_ph = torch.stack(all_ph).cpu().numpy()      # [rank, step, phase]
    chk = {}
    if chk_out:
        ks = sorted(chk_out)
        cs = torch.stack([chk_out[k] for k in ks] + [chk_w[k] for k in ks])
        dist.all_reduce(cs)
        cs = cs.cpu().numpy()
        chk = {k: (cs[i], cs[len(ks) + i]) for i, k in enumerate(ks)}
    agg = torch.tensor([total_s, a2a_bytes], dtype=torch.float64, device=device)
    dist.all_reduce(agg, op=dist.ReduceOp.MAX)
    total_s, a2a_bytes = agg.cpu().tolist()

    if rank == 0:
        avg_tok = TK
        avg_exp = TK * P / E
        for s in range(a.warmup + 1, n_total + 1):
            comp = all_ph[:, s, 2]
            line = "Step %d: %.3f ms/step, tokens/PE max/avg %.2f, gpu/PE max %.1f ms max/avg %.2f, expert max/avg %.2f" % (
                s, all_ph[:, s, 5].max(), step_tok[s, 0] / avg_tok, comp.max(),
                comp.max() / comp.mean(), step_tok[s, 1] / avg_exp)
            if n_moved[s]:
                line += ", moved %d" % n_moved[s]
            if s in chk:
                line += ", out2 %.15e, w2 %.15e" % chk[s]
            print(line)
        timed = slice(a.warmup + 1, n_total + 1)
        names = ["migration", "dispatch a2a", "compute", "combine a2a", "combine+chk", "step"]
        means = [all_ph[:, timed, i].mean(axis=1).max() for i in range(5)]
        print("\nPhases, mean over the timed steps (ms). An all_to_all also waits for the")
        print("slowest peer, so a rank's idle time shows up in its exchange phases.")
        print("  %-14s %s" % ("rank", " ".join("%7d" % r for r in range(P))))
        for i, n in enumerate(names):
            print("  %-14s %s" % (n, " ".join("%7.1f" % v for v in all_ph[:, timed, i].mean(axis=1))))
        print("Dispatch: %.1f MB in+out per rank per step (busiest rank), %.1f GB/s" % (
            a2a_bytes / a.steps / 1e6, a2a_bytes / a.steps / 1e9 / (means[1] / 1e3)))
        print("Host per step: routing %.1f ms, wait for counts %.1f ms; experts moved: %d" % (
            host_ms[timed, 0].mean(), host_ms[timed, 1].mean(), total_moved))
        if a.print_place:
            print("Placement: " + " ".join("%d:%d" % (e, owner[e]) for e in range(E)))
        print("\nTotal time: %.3f s\nAverage step time: %.3f ms" % (total_s, total_s / a.steps * 1e3))
    dist.barrier()
    dist.destroy_process_group()


if __name__ == "__main__":
    main()

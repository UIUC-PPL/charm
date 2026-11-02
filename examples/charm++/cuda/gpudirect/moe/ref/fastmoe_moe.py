#!/usr/bin/env python
"""One MoE layer in FastMoE with FasterMoE's smart schedule and expert
shadowing (He et al., PPoPP'22, merged into FastMoE 1.0), driven by the same
hashed Zipf routing as the Charm++ example, the PyTorch reference and the
Megatron-Core harness, so the token skew per step is identical. Expert
parallel, E/P experts per rank, static placement. With shadowing on,
FasterMoE's performance model picks the hot experts before every step,
broadcasts their weights to every rank, which then computes its own tokens for
them, and reduces their gradients back to the owner after the backward.
Forward, backward and SGD step per iteration with the self-similar loss
1/2 |y|^2. Throughput only: weights are FastMoE's own init, so the checksums
are not those of the example.

  FMOE_FASTER_SCHEDULE_ENABLE=1 FMOE_FASTER_SHADOW_ENABLE=1 srun ... refwrap.sh python fastmoe_moe.py \\
      -e 8 -m 4096 -H 14336 -t 8192 -k 2 -z 1.0 -p 0 -i 40 -u 3 [--bf16] [--forward-only]

The FasterMoE switches are FastMoE's own environment variables, read when fmoe
is imported: FMOE_FASTER_SCHEDULE_ENABLE, FMOE_FASTER_SHADOW_ENABLE,
FMOE_FASTER_GROUP_SIZE (default 4), and the shadow policy's machine model
FMOE_FASTER_GLBPLC_NETBW (bytes/s), FMOE_FASTER_GLBPLC_GPUTP (flop/s),
FMOE_FASTER_GLBPLC_ALPHA (d_ff/d_model, set here from -H/-m when unset) and
FMOE_FASTER_GLBPLC_DMODEL (set by FastMoE from the input).
"""
import argparse
import gc
import math
import os
import sys
import time

import numpy as np
import torch
import torch.distributed as dist

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from moe_ref import make_router, routing_line, init_uniform, hmix, Attention, attention_step, add_attention_args  # noqa: E402


def parse():
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
    ap.add_argument("-L", "--lr", type=float, default=1e-4)
    ap.add_argument("-S", "--seed", type=int, default=12345)
    ap.add_argument("--bf16", action="store_true", help="bf16 parameters and activations")
    ap.add_argument("--forward-only", action="store_true",
                    help="serving: the forward under no_grad, no loss, no backward, no update")
    add_attention_args(ap)
    return ap.parse_args()


a = parse()
if "FMOE_FASTER_GLBPLC_ALPHA" not in os.environ:
    os.environ["FMOE_FASTER_GLBPLC_ALPHA"] = str(a.d_ff / a.d_model)

from fmoe.layers import FMoE, fmoe_faster_schedule  # noqa: E402
from fmoe.gates.base_gate import BaseGate  # noqa: E402
from fmoe.linear import FMoELinear  # noqa: E402
from fmoe.fastermoe.config import switch_from_env  # noqa: E402
from fmoe.fastermoe.shadow_policy import get_shadow_policy  # noqa: E402
import fmoe.fastermoe.schedule as fsched  # noqa: E402


class HashedGate(BaseGate):
    """The example's routing in place of a learned gate: expert of (token, slot)
    = hashed Zipf draw of (seed, step, rank, token, slot); score 1/k."""

    hashed = None   # Router of the example, set after construction
    step = 0

    def __init__(self, d_model, num_expert, world_size, top_k=2):
        super().__init__(num_expert, world_size)
        self.top_k = top_k

    def forward(self, inp):
        T, K = inp.shape[0], self.top_k
        slot_expert, _ = self.hashed.route(self.step)   # (token, slot) order
        idx = torch.from_numpy(slot_expert.reshape(T, K)).to(inp.device).contiguous()
        score = torch.full((T, K), 1.0 / K, dtype=inp.dtype, device=inp.device)
        self.set_loss(torch.zeros(1, device=inp.device))
        return idx, score


class Expert(torch.nn.Module):
    """W1 (d_model x d_ff), ReLU, W2 (d_ff x d_model), no bias: the example's
    expert, on FastMoE's expert-linear module (one expert per module, as the
    smart schedule and shadowing require)."""

    def __init__(self, d_model, d_ff):
        super().__init__()
        self.htoh4 = FMoELinear(1, d_model, d_ff, bias=False)
        self.h4toh = FMoELinear(1, d_ff, d_model, bias=False)

    def forward(self, inp, fwd_expert_count):
        return self.h4toh(torch.relu(self.htoh4(inp, fwd_expert_count)), fwd_expert_count)


def main():
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
    assert E % P == 0, "experts must divide over the ranks"
    dtype = torch.bfloat16 if a.bf16 else torch.float32

    def log(*s):
        if rank == 0:
            print(*s, flush=True)

    torch.manual_seed(a.seed)
    layer = FMoE(num_expert=E // P, d_model=dm, world_size=P, top_k=K,
                 gate=HashedGate, expert=lambda d: Expert(d, dff)).to(device=device, dtype=dtype)
    layer.train()
    layer.gate.hashed = make_router(a, rank, P)
    params = [p for p in layer.experts.parameters() if p.requires_grad]
    opt = torch.optim.SGD(params, lr=a.lr / T)   # per-token mean, as the example's 1/n

    shadow_on = fmoe_faster_schedule and switch_from_env("FMOE_FASTER_SHADOW_ENABLE")
    shadows = []   # per step: the global expert ids the policy shadowed
    if fmoe_faster_schedule:
        base_policy = get_shadow_policy(d_model=dm)

        def counting_policy(lec, gec, n_expert, world_size, dev):
            res = base_policy(lec, gec, n_expert, world_size, dev)
            shadows.append(torch.nonzero(res).flatten().tolist())
            return res
        fsched.policy_fn = counting_policy

    x_in = init_uniform(T * dm, hmix(hmix(a.seed, 1000), rank), math.sqrt(3.0),
                        device, dtype).view(T, dm)
    attn = opt_a = None
    if a.attn:
        attn = Attention(dm, a.heads, a.seq, dtype, device, hmix(a.seed, 2000))
        opt_a = torch.optim.SGD(attn.parameters(), lr=a.lr / T)
        x_tok = x_in
        x_in = torch.empty_like(x_tok)
    if not a.forward_only:
        # the layer input carries the autograd graph: FasterMoE's fused forward records the experts'
        # sub-graphs itself and connects them to the output only through the input, and its backward
        # returns the input gradient in the same exchange regardless, as a transformer layer needs
        x_in.requires_grad_(True)
    n_total = a.warmup + a.steps
    H = sum(math.pow(r + 1, -a.zipf) for r in range(E)) if a.zipf > 0 else E
    nparams = sum(p.numel() for p in layer.experts.parameters())
    try:
        from importlib.metadata import version
        fm_version = version("fastmoe")
    except Exception:
        fm_version = "?"
    log("\n[FastMoE %s MoE layer: expert parallel, smart schedule %s, expert shadowing %s%s]" % (
        fm_version, "on" if fmoe_faster_schedule else "off", "on" if shadow_on else "off",
        ", group size %s" % os.environ.get("FMOE_FASTER_GROUP_SIZE", "4 (default)") if fmoe_faster_schedule else ""))
    log("Experts: %d, d_model %d, d_ff %d, %s, %d local, %.1f MB expert parameters per rank" % (
        E, dm, dff, "bf16" if a.bf16 else "fp32", E // P,
        nparams * (2 if a.bf16 else 4) / 1e6))
    log("Tokens: %d per rank per step on %d ranks, top-%d" % (T, P, K))
    log(routing_line(a, layer.gate.hashed))
    log("Steps: %d (+%d warmup); static placement (expert e on rank e*P/E)%s%s%s\n" % (
        a.steps, a.warmup, ", shadows re-chosen every step" if shadow_on else ", no balancing",
        "; forward only (serving)" if a.forward_only else "",
        "; attention sublayer (%d heads, sequences of %d) before the MoE" % (a.heads, a.seq) if a.attn else ""))
    if shadow_on:
        log("Policy: NETBW %s B/s, GPUTP %s flop/s, ALPHA %s, DMODEL %s" % tuple(
            os.environ.get("FMOE_FASTER_GLBPLC_" + k, "default") for k in ("NETBW", "GPUTP", "ALPHA", "DMODEL")))

    ev = [[torch.cuda.Event(enable_timing=True) for _ in range(3)] for _ in range(n_total + 2)]
    loss_hist = torch.zeros(n_total + 1, dtype=torch.float64, device=device)   # rank 0's loss per step, read at the end
    loss = torch.zeros((), device=device)
    for s in range(1, n_total + 1):
        layer.gate.step = s
        ev[s][0].record()
        if attn is not None:
            x_in = attention_step(attn, opt_a, x_tok, 1.0, a.forward_only)
            if not a.forward_only:
                x_in.requires_grad_(True)
        if a.forward_only:
            if fmoe_faster_schedule:
                # FasterMoE's fused forward records the experts' sub-graphs whatever the grad mode, and
                # without a backward to consume them the hot rank's memory grew every step and ran out
                # (44 GB) at the Mixtral shape; the graph is released by collecting the cycle each step
                out = layer(x_in)
                del out
                gc.collect()
                out = None
            else:
                with torch.no_grad():
                    out = layer(x_in)
            ev[s][1].record()
            if out is not None:
                loss = out.float().square().sum()   # a read of the output, as a server would make
        else:
            out = layer(x_in)
            ev[s][1].record()
            loss = 0.5 * out.float().square().sum()
            loss_hist[s] = loss.detach()
            loss.backward()
            opt.step()
            opt.zero_grad(set_to_none=True)
            x_in.grad = None
        ev[s][2].record()
        if s == a.warmup:
            torch.cuda.synchronize()
            t_start = time.perf_counter()
    torch.cuda.synchronize()
    total_s = time.perf_counter() - t_start
    if not torch.isfinite(loss):
        log("loss is not finite")
    if not a.forward_only:
        bad = [i for i, e in enumerate(layer.experts) if not all(torch.isfinite(p).all().item() for p in e.parameters())]
        if bad:
            print("Rank %d: non-finite weights in local expert(s) %s (global %s)" % (
                rank, bad, [rank * (E // P) + i for i in bad]), flush=True)
    if rank == 0 and not a.forward_only:
        lh = loss_hist.cpu().numpy()
        bad = [s for s in range(1, n_total + 1) if not np.isfinite(lh[s])]
        print("Loss (rank 0): step 1 %.6g, step %d %.6g, step %d %.6g%s" % (
            lh[1], a.warmup + 1, lh[a.warmup + 1], n_total, lh[n_total],
            "; first non-finite at step %d (step %d: %.6g)" % (bad[0], bad[0] - 1, lh[bad[0] - 1]) if bad else ""))

    fwd = np.array([ev[s][0].elapsed_time(ev[s][1]) for s in range(1, n_total + 1)])
    step = np.array([ev[s][0].elapsed_time(ev[s + 1][0]) if s < n_total else
                     ev[s][0].elapsed_time(ev[s][2]) for s in range(1, n_total + 1)])
    t = torch.tensor(np.stack([fwd, step]), dtype=torch.float64, device=device)
    allt = [torch.empty_like(t) for _ in range(P)]
    dist.all_gather(allt, t)
    allt = torch.stack(allt).cpu().numpy()   # [rank, 2, step-1]
    if rank == 0:
        for s in range(a.warmup + 1, n_total + 1):
            sh = ""
            if shadow_on and s - 1 < len(shadows):
                sh = ", shadowed %s" % (shadows[s - 1] if shadows[s - 1] else "none")
            print("Step %d: %.3f ms/step, forward %.1f ms slowest rank, %.1f ms fastest%s" % (
                s, allt[:, 1, s - 1].max(), allt[:, 0, s - 1].max(), allt[:, 0, s - 1].min(), sh))
        if shadow_on:
            timed = shadows[a.warmup:n_total]
            print("Shadowed experts: %.1f per step over the timed steps" % (
                sum(len(x) for x in timed) / max(1, len(timed))))
        print("\nTotal time: %.3f s\nAverage step time: %.3f ms" % (total_s, total_s / a.steps * 1e3))
    dist.barrier()
    dist.destroy_process_group()


if __name__ == "__main__":
    main()

#!/usr/bin/env python
"""One MoE layer in Megatron-Core (expert parallelism, all-to-all token
dispatcher), driven by the same hashed Zipf routing as the Charm++ example and
the PyTorch reference, so the token skew per step is identical. Static expert
placement, as Megatron-Core trains: no re-placement, no replication; a
forward, backward and SGD step per iteration with the self-similar loss
1/2 |y|^2. Throughput only: weights are Megatron's own, so the checksums are
not those of the example.

  srun ... refwrap.sh python megatron_moe.py -e 64 -m 2048 -H 8192 -t 8192 -z 1.3 -p 0 -i 60 -u 3 [--bf16] [--grouped]
"""
import argparse
import math
import os
import sys
import time

import numpy as np
import torch
import torch.distributed as dist

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from moe_ref import make_router, routing_line, init_uniform, hmix, Attention, attention_step, add_attention_args, master_sgd_step  # noqa: E402

from megatron.core import parallel_state  # noqa: E402
from megatron.core.tensor_parallel import model_parallel_cuda_manual_seed  # noqa: E402
from megatron.core.transformer.transformer_config import TransformerConfig  # noqa: E402
from megatron.core.models.gpt.gpt_layer_specs import get_gpt_layer_local_spec, get_gpt_layer_with_transformer_engine_spec  # noqa: E402
from megatron.core.transformer.moe.router import TopKRouter  # noqa: E402


class HashedRouter(TopKRouter):
    """Megatron's router with the example's routing in place of top-k on the
    gating logits: expert of (token, slot) = hashed Zipf draw of (seed, step,
    rank, token, slot). probs 1/k at the chosen experts, 0 elsewhere."""

    hashed = None   # Router of the example, set after construction
    step = 0

    def routing(self, logits, padding_mask=None, input_ids=None, packed_seq_params=None):
        E = self.config.num_moe_experts
        K = self.topk
        logits = logits.view(-1, E)
        T = logits.shape[0]
        slot_expert, counts = self.hashed.route(self.step)   # (token, slot) order
        idx = torch.from_numpy(slot_expert.reshape(T, K)).to(logits.device)
        routing_map = torch.zeros(T, E, dtype=torch.bool, device=logits.device)
        routing_map.scatter_(1, idx, True)
        probs = torch.zeros(T, E, dtype=logits.dtype, device=logits.device)
        probs.scatter_(1, idx, 1.0 / K)
        return probs, routing_map


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
    ap.add_argument("-L", "--lr", type=float, default=1e-4)
    ap.add_argument("-S", "--seed", type=int, default=12345)
    ap.add_argument("--bf16", action="store_true", help="bf16 parameters and activations")
    ap.add_argument("--grouped", action="store_true", help="grouped GEMM experts (with --te; the local spec ignores it)")
    ap.add_argument("--te", action="store_true",
                    help="the Transformer Engine layer spec (Megatron-LM's default --transformer-impl): "
                         "with --grouped the experts are TE's GroupedLinear (TEGroupedMLP)")
    ap.add_argument("--permute-fusion", action="store_true",
                    help="moe_permute_fusion: TE's fused token permutation in the dispatcher")
    ap.add_argument("--master", action="store_true",
                    help="fp32 master weights for the experts, the SGD step on them and a cast into the bf16 "
                         "parameters each step, as Megatron-LM's bf16 optimizer does (and the example)")
    ap.add_argument("--dispatcher", choices=["alltoall", "allgather"], default="alltoall")
    ap.add_argument("--capacity-factor", type=float, default=None,
                    help="Megatron's moe_expert_capacity_factor: tokens above capacity are dropped (GShard/Switch)")
    ap.add_argument("--forward-only", action="store_true",
                    help="serving: the forward under no_grad, no loss, no backward, no update")
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
    dtype = torch.bfloat16 if a.bf16 else torch.float32

    def log(*s):
        if rank == 0:
            print(*s, flush=True)

    parallel_state.initialize_model_parallel(tensor_model_parallel_size=1,
                                             expert_model_parallel_size=P)
    model_parallel_cuda_manual_seed(a.seed)

    cfg = TransformerConfig(
        num_layers=1, hidden_size=dm, num_attention_heads=16,
        ffn_hidden_size=dff, moe_ffn_hidden_size=dff,
        num_moe_experts=E, moe_router_topk=K,
        moe_token_dispatcher_type=a.dispatcher,
        moe_router_load_balancing_type="none", moe_aux_loss_coeff=0.0,
        moe_router_pre_softmax=True,   # required by the config check for top-1; routing is overridden anyway
        expert_model_parallel_size=P, tensor_model_parallel_size=1,
        add_bias_linear=False, gated_linear_unit=False,
        activation_func=torch.nn.functional.relu,
        moe_grouped_gemm=a.grouped,
        moe_permute_fusion=a.permute_fusion,
        moe_expert_capacity_factor=a.capacity_factor,
        moe_token_drop_policy="probs",
        bf16=a.bf16, params_dtype=dtype,
        use_cpu_initialization=False,
    )
    # Megatron-Core 0.20 has no GroupedMLP: the local backend builds SequentialMLP whatever
    # moe_grouped_gemm says, and the grouped path is the Transformer Engine spec.
    if a.te:
        spec = get_gpt_layer_with_transformer_engine_spec(num_experts=E, moe_grouped_gemm=a.grouped).submodules.mlp
    else:
        spec = get_gpt_layer_local_spec(num_experts=E, moe_grouped_gemm=a.grouped).submodules.mlp
    submods = spec.keywords["submodules"]
    submods.router = HashedRouter
    layer = spec.func(cfg, submodules=submods, layer_number=1).to(device)
    layer.train()
    layer.router.hashed = make_router(a, rank, P)
    params = [p for p in layer.parameters() if p.requires_grad]
    opt = torch.optim.SGD(params, lr=a.lr / T)   # per-token mean, as the example's 1/n
    masters = [p.detach().float().clone() for p in params] if a.master else None

    x_in = init_uniform(T * dm, hmix(hmix(a.seed, 1000), rank), math.sqrt(3.0),
                        device, dtype).view(T, 1, dm)
    attn = opt_a = None
    if a.attn:
        attn = Attention(dm, a.heads, a.seq, dtype, device, hmix(a.seed, 2000))
        opt_a = torch.optim.SGD(attn.parameters(), lr=a.lr / T)
        x_tok = x_in.view(T, dm)

    n_total = a.warmup + a.steps
    H = sum(math.pow(r + 1, -a.zipf) for r in range(E)) if a.zipf > 0 else E
    nparams = sum(p.numel() for p in layer.experts.parameters())
    log("\n[Megatron-Core %s MoE layer: expert parallel, %s dispatcher%s, experts %s%s]" % (
        __import__("megatron.core").core.__version__, a.dispatcher,
        " with fused permute" if a.permute_fusion else "", type(layer.experts).__name__,
        " (Transformer Engine spec)" if a.te else " (local spec)"))
    if a.master:
        log("Experts' optimizer: fp32 master weights, the SGD step on them, cast into the bf16 parameters each step")
    log("Experts: %d, d_model %d, d_ff %d, %s, %d local, %.1f MB expert parameters per rank" % (
        E, dm, dff, "bf16" if a.bf16 else "fp32", E // P,
        nparams * (2 if a.bf16 else 4) / 1e6))
    log("Tokens: %d per rank per step on %d ranks, top-%d" % (T, P, K))
    log(routing_line(a, layer.router.hashed))
    log("Steps: %d (+%d warmup); static placement (expert e on rank e*P/E), no balancing%s%s\n" % (
        a.steps, a.warmup,
        "; capacity factor %.2f (tokens above capacity dropped)" % a.capacity_factor if a.capacity_factor else "",
        ("; forward only (serving)" if a.forward_only else "") +
        ("; attention sublayer (%d heads, sequences of %d) before the MoE" % (a.heads, a.seq) if a.attn else "")))

    ev = [[torch.cuda.Event(enable_timing=True) for _ in range(3)] for _ in range(n_total + 2)]
    tok_max = np.zeros(n_total + 1)
    for s in range(1, n_total + 1):
        layer.router.step = s
        ev[s][0].record()
        if attn is not None:
            x_in = attention_step(attn, opt_a, x_tok, 1.0, a.forward_only).view(T, 1, dm)
        if a.forward_only:
            with torch.no_grad():
                out, _ = layer(x_in)
            ev[s][1].record()
            loss = out.float().square().sum()   # a read of the output, as a server would make
        else:
            out, _ = layer(x_in)
            ev[s][1].record()
            loss = 0.5 * out.float().square().sum()
            loss.backward()
            if masters is not None:
                master_sgd_step(params, masters, a.lr / T)
            else:
                opt.step()
                opt.zero_grad(set_to_none=True)
        ev[s][2].record()
        if s == a.warmup:
            torch.cuda.synchronize()
            t_start = time.perf_counter()
    torch.cuda.synchronize()
    total_s = time.perf_counter() - t_start
    if not torch.isfinite(loss):
        log("loss is not finite")

    fwd = np.array([ev[s][0].elapsed_time(ev[s][1]) for s in range(1, n_total + 1)])
    step = np.array([ev[s][0].elapsed_time(ev[s + 1][0]) if s < n_total else
                     ev[s][0].elapsed_time(ev[s][2]) for s in range(1, n_total + 1)])
    t = torch.tensor(np.stack([fwd, step]), dtype=torch.float64, device=device)
    allt = [torch.empty_like(t) for _ in range(P)]
    dist.all_gather(allt, t)
    allt = torch.stack(allt).cpu().numpy()   # [rank, 2, step-1]
    if rank == 0:
        for s in range(a.warmup + 1, n_total + 1):
            print("Step %d: %.3f ms/step, forward %.1f ms slowest rank, %.1f ms fastest" % (
                s, allt[:, 1, s - 1].max(), allt[:, 0, s - 1].max(), allt[:, 0, s - 1].min()))
        print("\nTotal time: %.3f s\nAverage step time: %.3f ms" % (total_s, total_s / a.steps * 1e3))
    dist.barrier()
    dist.destroy_process_group()


if __name__ == "__main__":
    main()

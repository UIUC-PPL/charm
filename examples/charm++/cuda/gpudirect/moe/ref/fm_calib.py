#!/usr/bin/env python
"""Measures the two machine parameters FasterMoE's shadow policy takes, on the
ranks of the run that will use them: the NCCL broadcast rate of one expert's
weights across every rank (what shadowing an expert costs) and the cuBLAS GEMM
rate at the expert's shape, fp32 and bf16. Rank 0 prints shell assignments
that run_fm.sh exports as FMOE_FASTER_GLBPLC_NETBW / FMOE_FASTER_GLBPLC_GPUTP.

  srun ... refwrap.sh python fm_calib.py -m 4096 -H 14336 -t 8192
"""
import argparse
import os
import time

import torch
import torch.distributed as dist


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-m", "--d-model", type=int, default=2048)
    ap.add_argument("-H", "--d-ff", type=int, default=8192)
    ap.add_argument("-t", "--tokens", type=int, default=8192, help="per rank; the GEMM uses 2x this many rows")
    ap.add_argument("-r", "--reps", type=int, default=5)
    a = ap.parse_args()
    rank = int(os.environ.get("RANK", os.environ.get("SLURM_PROCID", 0)))
    world = int(os.environ.get("WORLD_SIZE", os.environ.get("SLURM_NTASKS", 1)))
    local = int(os.environ.get("LOCAL_RANK", os.environ.get("SLURM_LOCALID", rank)))
    if torch.cuda.device_count() == 1:
        local = 0
    device = torch.device("cuda", local)
    torch.cuda.set_device(device)
    dist.init_process_group("nccl", rank=rank, world_size=world, device_id=device)

    # one expert's parameters, fp32, as the policy sizes them (2 * alpha * d_model^2 * 4 bytes)
    n = 2 * a.d_model * a.d_ff
    w = torch.empty(n, dtype=torch.float32, device=device)
    for _ in range(2):
        dist.broadcast(w, 0)
    torch.cuda.synchronize(); dist.barrier()
    t0 = time.perf_counter()
    for _ in range(a.reps):
        dist.broadcast(w, 0)
    torch.cuda.synchronize()
    bcast_s = (time.perf_counter() - t0) / a.reps
    t = torch.tensor([bcast_s], dtype=torch.float64, device=device)
    dist.all_reduce(t, op=dist.ReduceOp.MAX)
    bcast_s = t.item()

    rows = 2 * a.tokens
    rates = {}
    for name, dt in (("FP32", torch.float32), ("BF16", torch.bfloat16)):
        x = torch.randn(rows, a.d_model, dtype=dt, device=device)
        W = torch.randn(a.d_model, a.d_ff, dtype=dt, device=device)
        for _ in range(2):
            y = x @ W
        torch.cuda.synchronize()
        t0 = time.perf_counter()
        for _ in range(a.reps):
            y = x @ W
        torch.cuda.synchronize()
        s = (time.perf_counter() - t0) / a.reps
        rates[name] = 2.0 * rows * a.d_model * a.d_ff / s
    if rank == 0:
        print("# broadcast of %.0f MB to %d ranks: %.1f ms; GEMM %d x %d x %d: fp32 %.1f TFLOP/s, bf16 %.1f TFLOP/s" % (
            4 * n / 1e6, world, bcast_s * 1e3, rows, a.d_model, a.d_ff, rates["FP32"] / 1e12, rates["BF16"] / 1e12))
        print("NETBW=%.4g" % (4 * n / bcast_s))
        print("GPUTP_FP32=%.4g" % rates["FP32"])
        print("GPUTP_BF16=%.4g" % rates["BF16"])
    dist.barrier()
    dist.destroy_process_group()


if __name__ == "__main__":
    main()

#!/usr/bin/env python
"""Routing traces from SmartMoE's expert-selection dataset (thu-pacman/SmartMoE-AE,
`moe_trace/table_iter<it>_rank<r>.log`): per training iteration, rank and
MoE layer, the number of tokens each of the 32 experts received from each
micro-batch of 4096 tokens (top-1), recorded while training a GPT-MoE. These
are training-time loads, the complement of trace_routing.py's inference
routing of a trained checkpoint. The per-token expert ids are not in the
dataset, so each micro-batch's tokens get its experts in a hashed order with
the recorded counts; the loads per (chunk, expert) are exactly the recorded
ones, which is all the replay's balance depends on.

  python smartmoe_trace.py <moe_trace dir> --out traces/smartmoe [--layers 0,5,11] [--every 10]

Chunks follow (iteration, rank, micro-batch) order, so the example's replay
(a chunk of -t tokens per dispatcher and step) walks the training run in time.
"""
import argparse
import ast
import glob
import os
import re

import numpy as np

MAGIC = 0x4D4F4554


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dir")
    ap.add_argument("--out", required=True)
    ap.add_argument("--layers", default=None, help="comma-separated layer ids (default: all)")
    ap.add_argument("--seed", type=int, default=12345)
    ap.add_argument("--every", type=int, default=1, help="use every n-th recorded iteration (the dataset has 501, every 10 training steps)")
    a = ap.parse_args()
    files = glob.glob(os.path.join(a.dir, "table_iter*_rank*.log"))
    key = {}
    for f in files:
        m = re.search(r"table_iter(\d+)_rank(\d+)\.log$", f)
        key[(int(m.group(1)), int(m.group(2)))] = f
    its = sorted(set(k[0] for k in key))[::a.every]
    ranks = sorted(set(k[1] for k in key))
    print("%d files: iterations %d..%d (%d), ranks %d" % (len(files), its[0], its[-1], len(its), len(ranks)))
    tables = {}   # layer -> list of count rows in (iteration, rank, microbatch) order
    for it in its:
        for r in ranks:
            for line in open(key[(it, r)]).read().strip().split("\n"):
                l, v = line.split(":", 1)
                l = int(l)
                for row in ast.literal_eval(v):
                    tables.setdefault(l, []).append(np.asarray(row, dtype=np.int64))
    layers = sorted(tables) if a.layers is None else [int(x) for x in a.layers.split(",")]
    os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
    rng = np.random.default_rng(a.seed)
    print("%-6s %-8s %-8s %-8s %-9s  %s" % ("layer", "chunks", "tokens", "hottest", "x mean", "top shares over the run (%)"))
    for l in layers:
        rows = tables[l]
        E = len(rows[0])
        ids = []
        for cnt in rows:
            ids.append(rng.permutation(np.repeat(np.arange(E, dtype=np.int32), cnt)))
        ids = np.concatenate(ids).astype(np.int32)
        N = ids.shape[0]
        tot = np.sum(rows, axis=0).astype(np.float64)
        share = tot / tot.sum()
        with open("%s_L%02d.trace" % (a.out, l), "wb") as g:
            np.array([MAGIC, E, 1, N], dtype=np.int32).tofile(g)
            ids.reshape(N, 1).tofile(g)
        print("%-6d %-8d %-8d %-8.3f %-8.2f  %s" % (
            l, len(rows), N, share.max(), share.max() * E,
            " ".join("%.1f" % (100 * s) for s in sorted(share, reverse=True)[:10])))


if __name__ == "__main__":
    main()

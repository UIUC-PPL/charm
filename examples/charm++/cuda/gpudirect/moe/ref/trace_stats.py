#!/usr/bin/env python
"""Per-layer skew of recorded routing traces (trace_routing.py): each expert's
share of the (token, slot) pairs, the hottest expert against the mean (1/E),
and the hottest share per window of W tokens (one 8-GPU step at 8192 tokens
per GPU is 65536), plus what the skew means for P GPUs with E/P experts each:
the hottest expert's share against a GPU's share 1/P (a copy pays only when
an expert exceeds what one GPU would hold).

  python trace_stats.py traces/olmoe [--window 65536] [--gpus 8]
"""
import argparse
import glob
import sys

import numpy as np

MAGIC = 0x4D4F4554


def load(path):
    hdr = np.fromfile(path, dtype=np.int32, count=4)
    assert hdr[0] == MAGIC, path
    E, K, N = int(hdr[1]), int(hdr[2]), int(hdr[3])
    return E, K, np.fromfile(path, dtype=np.int32, offset=16).reshape(N, K)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("prefix")
    ap.add_argument("--window", type=int, default=65536)
    ap.add_argument("--gpus", type=int, default=8)
    a = ap.parse_args()
    files = sorted(glob.glob(a.prefix + "_L*.trace"))
    if not files:
        sys.exit("no %s_L*.trace" % a.prefix)
    W, P = a.window, a.gpus
    print("%-6s %-8s %-8s %-9s %-9s %-10s  %s" % ("layer", "hottest", "x mean", "x GPU/%d" % P, "win max", "win mean", "shares of the top 12 experts (%)"))
    for f in files:
        E, K, ids = load(f)
        N = ids.shape[0]
        cnt = np.bincount(ids.reshape(-1), minlength=E).astype(np.float64)
        share = cnt / cnt.sum()
        nw = N // W
        wins = ids[:nw * W].reshape(nw, W * K)
        wmax = np.array([np.bincount(w, minlength=E).max() / w.size for w in wins])
        l = int(f.rsplit("_L", 1)[1].split(".")[0])
        print("%-6d %-8.3f %-8.2f %-9.2f %-9.3f %-10.3f  %s" % (
            l, share.max(), share.max() * E, share.max() * P, wmax.max(), wmax.mean(),
            " ".join("%.1f" % (100 * s) for s in sorted(share, reverse=True)[:12])))


if __name__ == "__main__":
    main()

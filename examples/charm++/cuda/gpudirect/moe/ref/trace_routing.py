#!/usr/bin/env python
"""Records the routing of a real MoE checkpoint on real text: the top-k expert
ids of every token in every MoE layer, in text order, so the example (`-E
file`) and the harnesses (`--trace file`) can replay the model's own token
skew instead of the hashed Zipf.

  python trace_routing.py --model mistralai/Mixtral-8x7B-v0.1 --tokens 2800000 --out traces/mixtral
  python trace_routing.py --model allenai/OLMoE-1B-7B-0924   --tokens 2800000 --out traces/olmoe

Text: WikiText-103 (raw, train split), documents joined with newlines, packed
into sequences of --seq tokens. The model runs in bf16 over all the GPUs of
the node (`device_map="auto"`), `output_router_logits=True`; the expert of a
(token, slot) is the slot-th largest router logit, which is what the model's
own softmax-then-top-k selects. One file per MoE layer: header int32[4] =
{0x4d4f4554, n_experts, k, n_tokens}, then int32 ids[n_tokens][k]. A stats
file lists, per layer, each expert's share of the (token, slot) pairs, the
hottest share, and the hottest share per 65536-token window (one 8-GPU step
of the example at 8192 tokens per GPU), so the layers to replay can be chosen
by their skew.
"""
import argparse
import os
import sys
import time

import numpy as np
import torch

MAGIC = 0x4D4F4554


def tokenize_wikitext(tok, need):
    """WikiText-103 raw train split, documents joined as they come, tokenized until `need` tokens."""
    from datasets import load_dataset
    ds = load_dataset("Salesforce/wikitext", "wikitext-103-raw-v1", split="train")
    ids = []
    n = 0
    buf = []
    for row in ds:
        buf.append(row["text"])
        if len(buf) >= 2000:
            enc = tok("".join(buf), add_special_tokens=False)["input_ids"]
            ids.append(np.asarray(enc, dtype=np.int64)); n += len(enc); buf = []
            if n >= need:
                break
    if buf and n < need:
        enc = tok("".join(buf), add_special_tokens=False)["input_ids"]
        ids.append(np.asarray(enc, dtype=np.int64)); n += len(enc)
    return np.concatenate(ids)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--model", required=True)
    ap.add_argument("--tokens", type=int, default=2800000, help="tokens to route (rounded down to whole batches)")
    ap.add_argument("--seq", type=int, default=2048)
    ap.add_argument("--batch", type=int, default=8)
    ap.add_argument("--window", type=int, default=65536)
    ap.add_argument("--out", required=True, help="prefix of the output files")
    ap.add_argument("--token-file", help="pre-tokenized text (int64 .npy from --tokenize-only) instead of tokenizing WikiText here")
    ap.add_argument("--tokenize-only", action="store_true", help="tokenize WikiText for --model, write <out>_tokens.npy, no GPU")
    a = ap.parse_args()

    from transformers import AutoModelForCausalLM, AutoTokenizer

    t0 = time.time()
    if a.tokenize_only:
        tok = AutoTokenizer.from_pretrained(a.model)
        ids = tokenize_wikitext(tok, a.tokens + a.seq)
        os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
        np.save(a.out + "_tokens.npy", ids)
        print("wrote %d tokens to %s_tokens.npy in %.0f s" % (len(ids), a.out, time.time() - t0))
        return
    tok = AutoTokenizer.from_pretrained(a.model)
    model = AutoModelForCausalLM.from_pretrained(a.model, dtype=torch.bfloat16, device_map="auto")
    model.eval()
    cfg = model.config
    E = getattr(cfg, "num_local_experts", None) or getattr(cfg, "num_experts", None)
    K = getattr(cfg, "num_experts_per_tok", None)
    print("model %s: %d experts, top-%d, %d layers, loaded in %.0f s over %s" % (
        a.model, E, K, cfg.num_hidden_layers, time.time() - t0,
        sorted(set(str(p.device) for p in model.parameters()))), flush=True)

    ids = np.load(a.token_file) if a.token_file else tokenize_wikitext(tok, a.tokens + a.seq)
    per_batch = a.batch * a.seq
    n_batches = min(a.tokens, len(ids)) // per_batch
    N = n_batches * per_batch
    print("text: %d tokens tokenized, routing %d in %d batches of %d x %d" % (len(ids), N, n_batches, a.batch, a.seq), flush=True)

    dev0 = next(model.parameters()).device
    L = None
    out = None
    for b in range(n_batches):
        x = torch.from_numpy(ids[b * per_batch:(b + 1) * per_batch]).view(a.batch, a.seq).to(dev0)
        with torch.no_grad():
            try:
                res = model(input_ids=x, output_router_logits=True, logits_to_keep=1)
            except TypeError:
                res = model(input_ids=x, output_router_logits=True)
        rl = res.router_logits
        if L is None:
            L = len(rl)
            out = np.zeros((L, N, K), dtype=np.int32)
            print("router logits: %d layers, %s per layer" % (L, tuple(rl[0].shape)), flush=True)
        for l in range(L):
            top = rl[l].float().topk(K, dim=-1).indices.to(torch.int32).cpu().numpy()   # [batch*seq, K], text order
            out[l, b * per_batch:(b + 1) * per_batch] = top.reshape(per_batch, K)
        if b % 10 == 0:
            print("batch %d/%d, %.0f s" % (b + 1, n_batches, time.time() - t0), flush=True)

    os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
    W = a.window
    with open(a.out + "_stats.txt", "w") as f:
        hdr = "model %s, %d tokens of WikiText-103, %d experts, top-%d, windows of %d tokens" % (a.model, N, E, K, W)
        print(hdr); f.write(hdr + "\n")
        line = "%-6s %-10s %-10s %-10s %-10s  %s" % ("layer", "hottest", "x mean", "win max", "win mean", "shares (%)")
        print(line); f.write(line + "\n")
        for l in range(L):
            flat = out[l].reshape(-1)
            cnt = np.bincount(flat, minlength=E).astype(np.float64)
            share = cnt / cnt.sum()
            wins = out[l].reshape(-1, W * K) if (N * K) % (W * K) == 0 else out[l][: (N // W) * W].reshape(-1, W * K)
            wmax = np.array([np.bincount(w, minlength=E).max() / w.size for w in wins])
            line = "%-6d %-10.3f %-10.2f %-10.3f %-10.3f  %s" % (
                l, share.max(), share.max() * E, wmax.max(), wmax.mean(),
                " ".join("%.1f" % (100 * s) for s in sorted(share, reverse=True)[:16]))
            print(line); f.write(line + "\n")
            with open("%s_L%02d.trace" % (a.out, l), "wb") as g:
                np.array([MAGIC, E, K, N], dtype=np.int32).tofile(g)
                out[l].tofile(g)
    print("wrote %d trace files to %s_L*.trace in %.0f s" % (L, a.out, time.time() - t0))


if __name__ == "__main__":
    main()

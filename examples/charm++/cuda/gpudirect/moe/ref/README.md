# ref — PyTorch expert-parallel reference for the moe example

`moe_ref.py` is the same MoE layer as `../moe.C`, written the way an MoE
training framework writes it, so the Charm++ example can be compared against
current practice on the same node with the same work:

- one process per GPU, `torch.distributed` over NCCL;
- dispatch and combine are one `all_to_all_single` each, every token bundled
  per destination rank (the example sends one message per expert);
- expert compute with cuBLAS through `torch.mm`, the example's chunked SGD in
  the example's order (per source, chunks of `-c` tokens, `dh` reads the
  pre-update `W2`), on `-n` CUDA streams per rank;
- the host runs a step ahead of the device: slabs are double-buffered and the
  next step's token counts are exchanged (one `all_gather`) before this
  step's combine is issued, so routing and launch overhead hide under the
  previous step;
- load balancing by expert re-placement, the way EPLB does it, in two flavours:
  `--lb sync` recomputes the placement at an LB step from that step's token
  counts and moves the weights with NCCL point-to-point sends before the step
  runs (a full stop); `--lb async` makes the same decision but moves the
  weights after the old owner's compute of that step, overlapped with the rest
  of the step, and the placement takes effect at the next step. `--place
  greedy` moves as few experts as needed (pairwise transfers from the most to
  the least loaded rank, the same objective as DiffusionLB); `--place lpt` is
  EPLB's packing: every expert re-placed largest-first with an equal count per
  rank, with rank labels permuted afterwards to keep as many experts in place
  as possible.

The routing hash, the init hash, the loss and the update are ports of the
example's code; `selftest.py` checks the routing, the hashing and the init
values bit for bit against `hashcheck` (a copy of the example's functions):

    g++ -O1 -ffp-contract=off -o hashcheck hashcheck.C
    python selftest.py ./hashcheck

The Step lines use the example's format, and `out2` / `w2` are the example's
checksums (sums of squares of the combined outputs and of every expert's
weights after the step's update), so the two implementations can be checked
against each other: with the same arguments they agree to the fp32
accumulation-order noise of cuBLAS (about 1e-6 relative), and the
`tokens/PE` and `expert` imbalance ratios are identical.

## Options

The example's letters where they exist (`-e -m -t -k -i -u -z -p -c -n -L -C
-f -b -S -A -T -P`; `-H` for d_ff since argparse owns `-h`), plus

    --lb none|sync|async     re-placement mode (default none)
    --place greedy|lpt       placement algorithm (default greedy)
    --dtype fp32|bf16        bf16 weights and activations, throughput only
    --fuse                   one chunked pass per expert over all sources
    --no-pipeline            synchronize the host at every step end
    --replicate              one copy of the hottest expert from the start (EPLB-style redundancy)
    -G, --forward-only       serving: forward only, no gradient GEMMs, no update (the example's -G)
    --trace FILE             replay a recorded routing trace (the example's -E), see below
    --attn [--heads 32 --seq 1024]
                             a transformer block: the attention sublayer before the MoE (the example's -w)

The same `--trace`, `--attn` and `--forward-only` exist in `megatron_moe.py`
and `fastmoe_moe.py`; `make_router`, `routing_line`, `Attention` and
`attention_step` in `moe_ref.py` are shared by all three.

### Real routing: traces of a checkpoint on WikiText-103

`trace_routing.py --model M --tokens N --out traces/m` loads a Hugging Face
MoE checkpoint in bf16 over the node's GPUs (`device_map="auto"`), runs
WikiText-103's training text through it with `output_router_logits=True` and
writes, per MoE layer, the top-k expert ids of every token in text order
(`traces/m_Lnn.trace`: int32 header {magic, experts, k, tokens}, then
ids[tokens][k]); `--tokenize-only` / `--token-file` split the tokenization
off (the dataset read is slow on Lustre). `trace_stats.py traces/m` prints
each layer's skew: the hottest expert's share, its ratio to the mean 1/E and
to a GPU's share 1/P, and the hottest share per 65536-token window (one
8-GPU step). Every implementation replays a trace with the same rule, so the
tokens routed are identical across them. The checkpoints used
(`mistralai/Mixtral-8x7B-v0.1`, `allenai/OLMoE-1B-7B-0924`) are open; the HF
cache is `HF_HOME=/scratch/mzu/bhosale/hf`.

`smartmoe_trace.py <moe_trace dir> --out traces/smartmoe --every 10` builds
traces of the other kind, training-time routing, from SmartMoE's
expert-selection dataset (thu-pacman/SmartMoE-AE: per training iteration,
rank and layer, the tokens each of 32 experts received from each 4096-token
micro-batch, top-1, a GPT-MoE trained without a balancing loss; 501
iterations, 16 ranks, 24 layers). The dataset has counts, not per-token ids,
so a chunk's tokens get its experts in a hashed order with the recorded
counts; the load per (chunk, expert) is exact. Skew: the hottest expert takes
14-48% of a layer's tokens over the run (4.6-15x the mean, 1.1-3.8x a GPU's
share on 8 GPUs), the strongest of the three sources.

## Run

    ref/run_ref.sh <jobid> <tag> [arguments]      # one A40 node, four ranks, logs in ranklogs/ref_<tag>/
    ref/compare.sh <jobid> [reps]                 # app noLB/sync/async vs ref none/sync/async (greedy, lpt)

`refwrap.sh` pins rank r to GPU r and the cores and memory of NUMA node 3-r
(the A40 nodes' GPU order is the reverse of the NUMA order) and sets
`NCCL_P2P_LEVEL=SYS` so NCCL uses PCIe peer-to-peer between the sockets
instead of bouncing through host memory. PyTorch 2.12 from
`/sw/rh9.4/user/python/conda-env/pytorch-2.12.1-cu130`, no module needed.

## Measured against the example (2026-09-06, one A40 node, four GPUs)

Same node, same allocation, same arguments; the example runs on `+gpupool`
with DiffusionLB, the reference on NCCL with greedy re-placement. Step times
are the mean over the timed steps.

Defaults: 64 experts of 2048 x 8192, 8192 tokens per rank per step, top-1,
fp32, 30 steps, LB every 5 steps from step 5.

| implementation                  | no LB   | sync LB | async LB |
|---------------------------------|--------:|--------:|---------:|
| Charm++ example, per source (-U) | 204-206 |     215 |  195-206 |
| Charm++ example, fused (default) |     191 |     199 |      185 |
| reference, per-source chunks    |     219 |     190 |      193 |
| reference, one pass per expert  | 189-199 | 153-155 |      159 |

Llama-sized FFN (4096 x 14336), 20 steps:

| implementation                   | no LB | sync LB | async LB |
|----------------------------------|------:|--------:|---------:|
| Charm++ example, per source (-U) |   631 |     623 |      605 |
| Charm++ example, fused (default) |   560 |     545 |      530 |
| reference, one pass per expert   |   568 |     448 |      498 |

Correctness: with identical arguments the two agree to fp32 accumulation
noise. At step 30 of the defaults the example gives `out2
2.625314025759872e+06`, the reference `2.625314019798698e+06` (2e-9 relative);
`w2` agrees to 7e-12. The `tokens/PE` and `expert max/avg` imbalance ratios
are identical step for step, which is the check that the routing really is the
same function.

### Where the difference is

**Not the migration mechanism.** The example's async LB beats its own sync LB
(605 against 623 at Llama size, 195 against 215 at the defaults); the
reference's async loses to its own sync (498 against 448, 159 against 155).
Overlapping the weight transfer with NCCL does not pay, for two reasons: the
placement only takes effect at the next step, so one more step runs
unbalanced, and the point-to-point transfer shares the PCIe links with the
next dispatch. The Charm++ runtime's overlap is the better mechanism.

**The placement decision.** Between reshuffles the reference's greedy
re-placement holds `tokens/PE max/avg` at 1.00 to 1.02; DiffusionLB holds 1.27
to 1.29 at the defaults and 1.52 at Llama size, moving 6 to 8 experts where
greedy moves 22. That is most of the reference's advantage, and it is a
balancer-quality gap, not a runtime gap.

**The GEMM structure.** Summing every rank's compute time gives the GPU
seconds spent in GEMMs for a known number of FLOPs (10 x d_model x d_ff per
token, forward and backward):

| variant                        | GPU ms/step, all ranks | TFLOPS/GPU | of peak |
|--------------------------------|-----------------------:|-----------:|--------:|
| reference, one pass per expert |                    278 |       19.8 |     53% |
| reference, per-source chunks   |                    397 |       13.8 |     37% |
| reference, TF32                |                    148 |       37.2 |     50% |
| reference, bf16                |                     72 |       76.5 |     51% |

The example chunked per source, as the reference does with `--fuse` off, and
its busiest rank took 153 ms against the reference's 150: the same 37%. Fusing
was worth 30% of the GEMM time and needed only a copy into contiguous scratch,
so the example now does it by default and its compute span is 131 ms. That
closes the no-LB gap: 191 ms against the fused reference's 189-199, and 560
against 568 at Llama size. A real framework goes further with a grouped GEMM
over all local experts.

**Precision.** Reference without LB: fp32 199 ms, TF32 120 ms, bf16 59 ms per
step (that bf16 was pure bf16: weights, gradients and updates in bf16). The
example at TF32 takes 148 ms against the reference's 120. Since 2026-10-31
`--dtype bf16` is mixed precision as the example's `-B`: bf16 tokens,
activations and weight copies for the GEMMs, fp32 accumulation (`torch.mm`
with `out_dtype`), fp32 master weights, moments and gradient accumulators;
its checksums then agree with the example's bf16 run to 3e-7 on `out2`
(2.534871297842643e+06 against 2.534870436344967e+06 at the arguments of the
2026-10-31 tables) and the fp32 path's bits are unchanged.

**Exchange.** The reference moves 189 MB in and out per rank in an 18 ms
dispatch, 10.5 GB/s. The example's token exchange moves 191 MB in 33 ms, 5.8
GB/s, because it sends one message per expert (64 per dispatcher) at about
0.35 ms of host cost each, where the reference sends one per destination rank.
Both are far from the 24 GB/s the links can carry.

Two caveats on the tables. The example's `sync` and `async` include the LB
step's own cost, and its first LB step is expensive; the reference's greedy
decision is a few milliseconds of NumPy on data it already has. And the
reference keeps the host a step ahead of the device, which the example does
not need to do because its scheduler already overlaps; `--no-pipeline` costs
the reference nothing here (190 against 189 ms), so this is not what separates
them.

## Megatron-Core (2026-10-31, one A40 node, four GPUs)

`megatron_moe.py` builds one MoE layer with Megatron-Core 0.20's own modules
(`get_gpt_layer_local_spec`, expert parallelism over the four ranks, the
all-to-all token dispatcher, sequential or grouped-GEMM experts, fp32 or bf16)
and replaces its router's top-k with this reference's `Router`, so every
(token, slot) goes to the expert the example sends it to and the skew per step
is the same. Placement is static, as Megatron trains; the loss is 1/2 |y|^2
and the update SGD. Weights are Megatron's own, so only throughput compares.
`run_mg.sh <jobid> <tag> [args]` launches it (logs in `../ranklogs/mg_<tag>/`);
`megatron-core` 0.20.0 and `grouped_gemm` 0.3.0 are pip-installed, unpatched,
in the user site of the `pytorch-2.12.1-cu130` environment, and the environment's `libstdc++` and NCCL
must lead `LD_LIBRARY_PATH` (the launcher does this) for Transformer Engine,
which `megatron.core` imports, to load.

Same arguments as the example's 2026-10-31 runs (`-e 64 -m 2048 -H 8192 -t
8192 -z 1.3 -p 0 -i 60 -u 3`), steady state steps 33-63:

| arm                                              | ms/step | forward, slowest / fastest rank |
|--------------------------------------------------|--------:|--------------------------------:|
| fp32, sequential experts                         |   294.1 | 248 / 144 ms                    |
| fp32, grouped GEMM                               |   294.1 | 249 / 146                       |
| bf16, grouped GEMM                               |    95.5 |  76 / 51                        |
| fp32, sequential, uniform routing (`-z 0`)       |   161.2 |  77 / 72                        |
| bf16, grouped GEMM, uniform routing (`-z 0`)     |    59.9 |  30 / 27                        |

This reference without balancing ran 289.2 on the same arguments, so the
PyTorch reference stands in for Megatron's structure at the same precision.
The example ran 179.7 unbalanced and 115.3 (109.0 on the Megatron node) with
ShedLB's copies; the uniform-routing arm shows that the example's balanced
number is below what any balancing inside Megatron's lock-step structure
reaches in fp32 on this node.

Forward only (`--forward-only`: the layer under `no_grad`, no loss, no
backward, no update; the example's `-G`), same arguments, steady state:

| Megatron-Core arm, serving                        | ms/step |
|---------------------------------------------------|--------:|
| fp32, grouped GEMM, z 1.3                         |   150.1 |
| bf16, grouped GEMM, z 1.3                         |    54.5 |
| fp32, Mixtral-8x7B shape, unbalanced router z 1.0 |   536.4 |
| bf16, Mixtral-8x7B shape, unbalanced router z 1.0 |   163.7 |

The example served the same steps in 22.9 (bf16, z 1.3; 20.2 with ShedLB's
copy) and, at the Mixtral shape with the unbalanced router, 287.4 / 219.6
(fp32, plain / with the copy) and 110.3 / 85.8 (bf16). The real-model
shapes (Mixtral-8x7B: 8 experts of 4096 x 14336, top-2; DeepSeek-V3: 256
routed experts of 7168 x 2048, top-8) and the rest of the arms are in the
example's README.

Two nodes, eight GPUs, expert parallel over all eight (`NODES=2 ref/run_mg.sh`,
job 23388275), steady state steps 23-43:

| Megatron-Core arm, 8 GPUs                              | ms/step |
|--------------------------------------------------------|--------:|
| fp32, grouped GEMM, Mixtral-8x7B shape, z 0.3          |  1155.0 |
| bf16, grouped GEMM, Mixtral-8x7B shape, z 0.3          |   337.7 |
| fp32, grouped GEMM, Mixtral-8x7B shape, z 0.3, serving |   570.0 |
| bf16, grouped GEMM, Mixtral-8x7B shape, z 0.3, serving |   201.3 |
| fp32, grouped GEMM, default shape, z 1.0               |   385.0 |
| fp32, grouped GEMM, Mixtral-8x7B shape, z 1.0          |  1882.1 |
| bf16, grouped GEMM, Mixtral-8x7B shape, z 1.0          |   522.6 |
| fp32, grouped GEMM, Mixtral-8x7B shape, z 1.0, serving |   908.8 |
| bf16, grouped GEMM, Mixtral-8x7B shape, z 1.0, serving |   279.6 |

The example on the same eight GPUs: Mixtral shape 649.4 / 164.7 (fp32 / bf16
training) and 280.0 / 113.4 (serving), default shape z 1.0 198.0 plain and
134.1 with ShedLB's copy (the example's README has the tables).

## FastMoE and FasterMoE (2026-11-01, two A40 nodes, eight GPUs)

FasterMoE (He et al., PPoPP'22) is the prior system that replicates hot
experts at run time: before every iteration a performance model picks the
experts whose load would make them the critical path, broadcasts their
weights to every worker ("shadows"), each worker computes its own tokens for
them locally, and the gradients are reduced back to the owner after the
backward. Its smart schedule splits the two all-to-alls into a group-wise
exchange overlapped with the expert computation. Both are in FastMoE 1.x
(`github.com/laekov/fastmoe`), switched on by FastMoE's own environment
variables. FlexMoE (Nie et al., SIGMOD'23), which adds expand/shrink/migrate
primitives over the same idea, has no public code; its paper reports 1.30x
over FasterMoE on average.

`fastmoe_moe.py` builds one FastMoE layer (`FMoE`, E/P experts per rank, the
example's expert W1 - ReLU - W2 without bias on `FMoELinear`) with a gate that
returns this reference's `Router` draws and 1/k scores, so the routing is the
example's; loss 1/2 |y|^2, SGD, `--bf16`, `--forward-only`, the Megatron
harness's timing and output format, plus the experts FasterMoE shadowed at
each step. `run_fm.sh <jobid> <tag> <plain|sched|faster> [args]` runs FastMoE
as shipped, with the smart schedule, or with schedule and shadowing (the
FasterMoE system); `run_fm.sh <jobid> calib calib -m .. -H .. -t ..` first
measures the two machine parameters the shadow policy takes on the ranks of
the run (NCCL broadcast rate of one expert's weights across all ranks, cuBLAS
GEMM rate at the expert shape in fp32 and bf16) and the launcher exports them
as `FMOE_FASTER_GLBPLC_NETBW` / `FMOE_FASTER_GLBPLC_GPUTP`; `ALPHA` is set
from `-H/-m`. FastMoE master (55af4f9, 2025-02-10) is installed in the user
site of the same `pytorch-2.12.1-cu130` environment, built with plain gcc
against the environment's NCCL headers and `libnccl.so.2` (`build_fastmoe.sh
<work dir>`: clone, `fastmoe-55af4f9.patch`, the NCCL stub, user-site install;
FastMoE reports itself as 1.1.0). The group size of the group-wise exchange is
FastMoE's default (4).

**The network NCCL uses.** Delta has no NCCL module; every harness uses the
`nvidia-nccl-cu13` 2.29.7 wheel inside the `pytorch-2.12.1-cu130` environment.
Across nodes NCCL reaches Slingshot only through the `aws-ofi-nccl` plugin
(`module aws-ofi-nccl/1.19.2`: `NCCL_NET_PLUGIN=ofi`, `FI_PROVIDER=cxi`,
`FI_CXI_DISABLE_HOST_REGISTER=1`, `FI_MR_CACHE_MONITOR=userfaultfd`, the plugin
and libfabric 2.3.1 on the library path); without it NCCL finds no IB device
and falls back to its socket transport, which is what every two-node baseline
number dated 2026-11-01 ran on (the example's README, "Measured 2026-11-02",
has the re-measurement). `refwrap.sh` sets the plugin on every rank and logs
NCCL's INIT and NET lines, so a rank log shows `Using network Libfabric`. The
userfaultfd monitor is required: with libfabric's default memhooks one arm
hung in NCCL's connection setup (job 23452305).

Three patches were needed to run it at all, in the clone that was built
(`git diff` against 55af4f9): the smart-schedule backward's dtype dispatch
did not include bf16 (the forward's did); the shipped shadow policy sums the
gathered token counts over the experts for each source rank instead of over
the ranks for each expert (`sum(1)` of a [rank, expert] stack), so with one
expert per rank every "load" is the rank's token count and nothing is ever
shadowed, and with several experts per rank the sorted indices address the
wrong experts; and FasterMoE's fused forward keeps the experts' recorded
sub-graphs alive until a backward consumes them, so a forward-only run grows
by the hot rank's activations every step and ran out of 44 GB (the harness
drops the output and collects garbage each step in that mode). The FastMoE
arms' input requires grad, as a transformer layer's does, because the fused
forward builds its graph only through the input; the schedule returns the
input gradient regardless, while Megatron's harness and the example do not
compute it.

Measured 2026-11-01 (job 23403736, gpub002+062), the cases of the example's
8-GPU section, steady state steps 23-43. The policy ran with the measured
parameters: NETBW 3.05 GB/s (a 470 MB expert broadcast to the 8 ranks took
117-154 ms across three calibrations), GPUTP 23.7 TFLOP/s fp32 and 123
bf16, ALPHA 3.5, group size 4 (FastMoE's default).

| arm, 8 GPUs                                             | FastMoE plain | + smart schedule | FasterMoE: schedule + shadows |
|---------------------------------------------------------|--------------:|-----------------:|------------------------------:|
| fp32 training, Mixtral-8x7B shape, z 1.0                |        2102.5 |           2018.6 | 1195.0 (experts 1, 2, 4 shadowed every step) |
| bf16 training, Mixtral-8x7B shape, z 1.0                |         631.2 |            579.5 | 402.9 (experts 1, 4; loss NaN from step 2) |
| fp32 training, default shape, z 1.0                     |         509.6 |            449.1 | 322.1 (1.6 experts shadowed per step) |
| fp32 serving, Mixtral-8x7B shape, z 1.0                 |         901.9 |     out of memory | 661.5 (28 steps, steady 14-28; 3 shadows) |
| bf16 serving, Mixtral-8x7B shape, z 1.0                 |         276.3 |     out of memory | 306.7 (28 steps, steady 14-28; 2 shadows) |

Serving under the smart schedule: the fused forward's recorded sub-graphs
are released only by a backward, so a forward-only run grows every step by
the hot rank's activations; the schedule-only arms ran out of the A40's 44 GB
on the hot rank in every attempt (`no_grad`, grad enabled, a garbage
collection per step), the shadow arms reached step 30 but not 43 with the
collection, so their serving rows are 28-step runs. The example on the same
cases (its README): Mixtral z 1.0 fp32 training 1209.9 plain / 692.7 with
ShedLB's two copies, bf16 276.2 / 219.1, fp32 serving 491.9 / 282.0, bf16
serving 121.8 / 113.3; default shape 198.0 / 134.1. Megatron-Core: 1882.1,
522.6, 908.8, 279.6, 385.0.

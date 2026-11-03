# moe — mixture-of-experts layer miniApp for GPU load balancing

A single MoE feed-forward layer, written so that the experts are migratable
Charm++ objects with their state on the GPU, and the load moves the way it does
in a real MoE: by routing. Every step each PE routes its batch of tokens across
all experts, the experts run their feed-forward on what they were sent, the
outputs go back to the tokens' owners, and the experts update their weights.
The popularity of the experts is skewed (Zipf) and the hot set is reshuffled
periodically, so which GPU is overloaded changes during the run and no static
placement can fix it.

Unlike the halo-exchange miniApps (jacobi2d, leanmd, pic2d, barnes) this
exercises

- all-to-all traffic every step (every dispatcher to every expert and back),
- objects with no position key, so DiffusionLB takes its heap path rather
  than the interval cut,
- migration payloads of 100 MB to 1.6 GB per object,
- application-declared loads instead of CUPTI measurement: an expert knows its
  cost before it runs (token count times its measured seconds per token), so
  the balancer needs no kernel tracing at all.

## Model

Expert e holds `W1 [d_model x d_ff]` and `W2 [d_ff x d_model]` in fp32 (plus
Adam moments with `-A`, tripling the payload). For a run of n tokens it runs

    h  = relu(x W1)        y  = h W2                      forward
    dy = y                                                loss = 1/2 |y|^2
    dh = dy W2^T           dz = dh * relu'(z)             backward
    dW2 += h^T dy          dW1 += x^T dz                  gradient

five GEMMs per chunk of `-c` tokens (cuBLAS, fp32; `-T` for TF32), and once
per step the update `W -= (lr/n) dW` (SGD) or Adam on `dW/n`, so the cost of
an expert is linear in the tokens it receives with a 3:1 backward to forward
ratio like real training, plus one pass over the weights per step. The
weights are read only within a step: the gradient is accumulated in two
parts, one per dispatcher partition (the even dispatchers' tokens and the
odd dispatchers'), each its own run with its own chunk grid, and the two
parts are added once in a fixed order. That is the structure two copies of
an expert compute in (below), so the bits agree whether or not an expert
was replicated. The loss is self-similar so no labels are needed; it only
exists to make the weights change every step in a way that depends on
exactly which tokens each expert saw and in which order, which is what the
checksums verify.

## Decomposition and communication

- `Dispatcher` group, one per PE, never migrates. Holds `-t` tokens of
  `d_model` floats on the device (fixed for the run), routes them each step on
  the host with a hashed Zipf draw, sorts them by expert with a gather kernel,
  and ships one slab per expert with a zerocopy device send (`CkDeviceBuffer`
  and a post entry method on the expert). Expert outputs land in a receive
  slab by expert offset and a combine kernel scatters them back to token
  order (weighted `1/k` for top-k). It also owns the PE's GPU context: the one
  stream every kernel and transfer on the PE goes through, the cuBLAS handle
  with a fixed workspace, and the activation scratch.
- `Expert` 2-D chare array `(expert, copy)`, `usesAtSync`. Created as
  `(e, 0)` for every expert; `(e, 1)` exists while the balancer keeps a
  second copy (below). A copy receives one slab per dispatcher it serves per
  step into a per-source segment (arrival order does not matter: the post
  entry method knows the source), runs the chunked step partition by
  partition in source order, sends each dispatcher its outputs from the
  matching output segment, and waits for those sends to complete before the
  next step.

Routing is a pure function of (seed, step, PE, token, slot). Slab order is
(token, slot) order within a source, sources are processed in PE order, chunk
boundaries fall at fixed offsets, GEMMs use a fixed workspace with atomics
disallowed, and the checksums are fixed-order double reductions. So every run
with the same arguments produces the same `out2` (sum of squares of the
combined outputs) and `w2` (sum of squares of all weights) whatever the
placement, and a migration that loses or reorders anything shows up as a
different checksum.

## Load balancing

The balancer is ShedLB, linked in (`-balancer ShedLB` in the Makefile; a
`+balancer` on the command line still overrides it). Every expert holds the
same bytes but the loads differ by routing (the hottest draws about 13x the
average at z 1.0), migration cost is bytes, and the communication graph
carries no placement information (each expert receives equal shares from
every dispatcher), so the objective that matters is ShedLB's: clear each
overloaded GPU's excess for the fewest bytes, and leave a drift-sized
imbalance (`+LBShedEps`, 0.10) alone instead of chasing it with a 128 MB
move. The runtime's default chain, MetisLB then DiffusionLB, repartitions
(46 of 64 experts proposed at one step on two nodes) or under-moves
(tokens/PE 1.28 against 1.01 for a greedy re-placement).

### Replication

A placement balancer stops at the heaviest object: at z 1.0 the hottest
expert draws ~21% of the tokens, so on 8 GPUs no placement gets below an
imbalance of 1.69 (mean share 12.5%). The experts therefore declare
themselves replicable (`setLBReplica`, up to `-R` copies, default 2), and
ShedLB asks for a copy of an expert whose load exceeds the per-GPU
threshold (`ckClone`), and takes it back once the expert is light again
(`ckRetire`). The runtime decides when, which expert and where, from the
measured loads, the memory ledger and its cost model; the application
implements the copy:

- The dispatchers are in four fixed partitions, `p = d mod 4`; copy r of k
  serves the contiguous block of partitions `[4r/k, 4(r+1)/k)`, a single
  partition or an aligned pair. The step's gradient is always the fixed
  tree `((P0 + P1) + (P2 + P3))` over the partitions' accumulators: a copy
  computes its block's leaves against the same weights, folds them to its
  block sum, sends it to every other copy (zero-copy device messages,
  landing in per-peer parity double-buffers so the next step's can land
  while this step's are read), assembles the two tree nodes from the block
  sums and applies the same update, so the copies' weights stay identical
  and equal an unreplicated expert's, which computes the four leaves itself
  and folds the same way. Per step that is k - 1 weight-sized exchanges per
  copy (128 MB each at the defaults) and `4 + 4 (R - 1)` weight-sized
  buffers per copy (`replicaExtraBytes`, declared to the ledger).
- The balancer's decision for a round reaches every copy of the expert
  before the round releases any of them: `ckClone` on the copy that will
  send the state, `ckRetire` on the copy that goes (always the highest
  rank), `ckCloneNotice` / `ckRetireNotice` on the others. All act at the
  end of the step the round released them in: the step's outputs announce
  the number of copies serving the next step (lookahead is off in the
  window, so every dispatcher routes the next step with it), the source
  inserts `(e, k)` where the balancer said and sends it the weights (and
  moments) behind the update, the copies whose block grew open channels for
  the dispatchers they newly serve, and a copy left alone frees its exchange
  buffers. A new copy waits for its state, opens channels for its
  dispatchers and starts at the next step. A retiring copy's dispatchers
  drop their channels at the start of the next step and say so, then it
  closes its own and goes.
- Only copy 0 reports to the main chare (the others' tokens, PEs and device
  times ride in with their block sums), so the per-step report count never
  changes; `w2` is copy 0's weights, identical to every copy's.

`-X` starts the hottest expert of the first hot set as two copies (both
initialised from the same seed, no transfer) with no balancer involved: the
replicated arithmetic alone, whose checksums must equal the plain run's bit
for bit. `-R 1` declares one copy and the balancer never replicates.

The join point is inside the expert's step, after it has consumed every slab
of the step and before it computes (see `runStep` in `moe.ci`). Nothing can
be sent to an expert between those two events (the dispatchers are waiting
for its outputs), so a parked or migrating expert never has a device receive
landed or in flight; the runtime holds a migration until every such receive is
consumed, and an expert parked in `AtSync` with an unconsumed slab would
deadlock. `-a` uses the split barrier (`AtSyncStart` at an LB step,
`AtSyncWait` `-l` steps later) and needs `+LBAsync`.

The migratable state is the weights (and moments) plus whatever the step has
live at the moment of the move: the slabs received so far before compute, the
outputs after it. Each expert declares its load with `setObjGPUTime` every
step: its token count times an EMA of measured seconds per token (device
events around its own kernels), which is also rate-aware for free. With `-I`
the app instead switches CUPTI instrumentation on for three steps before each
LB step, as pic2d does, and the balancer uses measured loads.

## Options

    -e experts (64)        -m d_model (2048)      -h d_ff (8192)
    -t tokens per PE per step (8192)              -k top-k (1)
    -i steps (50)          -u warmup steps (5)
    -z zipf exponent (1.0; 0 = uniform)           -p hot-set reshuffle period (10; 0 = static)
    -c chunk tokens (1024) -r slab headroom (0 = auto from the zipf peak)
    -L learning rate (1e-4) -A Adam               -T TF32
    -B bf16 GEMM operands (tokens, activations, outputs, a bf16 copy of each
       weight), fp32 accumulation, fp32 master weights, moments, gradient
       accumulators and checksums; half the exchange bytes
    -G serving: the forward alone, no gradient, no update, no exchange between
       an expert's copies (w2 stays its initial value; out2 is the oracle)
    -C checksum frequency (5; 0 off)              -s stats frequency (1)
    -f first LB step (10)  -b LB frequency (9999) -a async LB  -l wait lag (3)
    -R copies the balancer may keep of one expert (2, up to 4; 1 = never replicate)
    -X start with the hottest expert as two copies (the arithmetic without a balancer)
    -I CUPTI-measured loads instead of estimates  -S seed  -P print placement
    -U one chunked pass per source instead of one per expert (slower; the
       fused default is described under Measured below)
    -E routing trace to replay instead of the hashed Zipf (ref/trace_routing.py
       records a real checkpoint's top-k per token on WikiText-103; token t of
       dispatcher d at step s is text token ((s-1)*n_disp + d)*n_tokens + t,
       modulo the trace; the slab capacity and -X's expert come from the trace)
    -w a transformer block: an attention sublayer (q/k/v/o projections, 32
       heads, causal, sequences of 1024 tokens) on each dispatcher's tokens
       before the MoE, trained on its own loss 1/2|h|^2 with SGD; the MoE routes
       its output. The same sublayer is the harnesses' --attn (ref/moe_ref.py).

## Build and run

    make                                   # needs cuBLAS: MATHLIBS=<nvhpc>/math_libs/<ver>
                                           # a HIP tree picks hipcc and hipBLAS itself (HIP_PORT.md)

On Delta (one A40 node, four PEs, one per GPU, NUMA-pinned by the wrapper):

    export RANKLOG_DIR=ranklogs/x; mkdir -p $RANKLOG_DIR
    srun --jobid=$J --mpi=cray_shasta -N 1 -n 4 --ntasks-per-node=4 --cpus-per-task=8 \
         --cpu-bind=none --exact --unbuffered ranklogs/moewrap.sh ./moe \
         -i 60 -u 3 -f 20 -b 20 -a \
         +gpushm +gpupool +gpupoolsize 1024 +gpuipceventpool 256

No balancer flags: ShedLB is linked in, and `-a` declares the split barrier
to the runtime (`CkLBSetAsync`), as sph3d, pic2d and leanmd do.

`ranklogs/lbcmp.sh <jobid> <reps> noLB_pool sync_pool async_pool` runs the
comparison and checks every run's checksums against the first.

Per-step output: `tokens/PE max/avg` is the routed-token imbalance across
PEs, `gpu/PE max/avg` the measured device-time imbalance (lagged one step:
an expert's time is sampled when its next step starts), `expert max/avg` the
skew across experts. With the defaults the hottest expert draws about 13x the
average.

## The load declaration (2026-10-31)

An expert declares its load as tokens times a rate times the steps of the
interval. The rate is job-wide: each PE keeps the union of its experts' kernel
spans on the device clock -- one interval set across steps, so a window's busy
time can never exceed its wall time -- and the tokens they covered, both with
a per-step decay; copy 0 of every expert reports its PE's two figures with its
step report, and the main chare broadcasts their ratio over all PEs to every
PE each step (`MoeCtx::global_spt`). Every expert does the same work per
token on identical GPUs, so this is the model, and every GPU's declared load
is its tokens on one scale.

What it replaced: a per-PE rate, busy time per step divided by that PE's
tokens. Experts' steps overlap in time under lookahead, so per-step interval
sets counted the same device time once per step, and a crowded GPU, whose
kernels run slower under contention, read a higher rate as well as more
tokens. In the async replication run below (job 23383169) two GPUs read
2.3-2.6e-5 s/token against 1.2-1.3e-5 on the other two with tokens within
15%, the balancer moved most of their experts away, and the run stayed at a
1.36 token imbalance; a round later the reading had flipped. The flat-load
diagnostic (`CHARM_MOE_FLATLOAD=1`) declares a constant rate and remains the
check of the model.

## Measured 2026-10-31: replication, one A40 node, four PEs

`-z 1.3 -p 0 -i 60 -u 3` (the hottest expert draws ~34% of the tokens, above
a GPU's share of 25% and the 27.5% threshold), 64 experts of 2048 x 8192,
8192 tokens per process, `+gpupoolsize 32768` (below), the job-wide rate.
Steady state = steps 33-63. Every arm's `out2` and `w2` are the same bits:
`out2 2.534902339438027e+06, w2 2.183175448111042e+05`.

| arm                                   | ms/step | what the balancer did                              |
|---------------------------------------|--------:|----------------------------------------------------|
| plain (`-R 1 -f 99999`)               |   179.7 | nothing                                            |
| `-X` (hottest expert as two copies)   |   143.1 | nothing; the replicated arithmetic alone           |
| ShedLB `-R 1`, sync / async           | 182.2 / 181.1 | nothing placeable: the floor (earlier run)     |
| ShedLB `-R 2`, sync                   |   122.1 | one copy decided at step 20, serving from 21; 7 moves |
| ShedLB `-R 2`, async                  |   115.3 | one copy decided at step 21, made at 22, serving from 23; 7 moves |

With a static hot set, replication under ShedLB takes a third off the step,
and the balancer's choice of destination beats `-X`'s fixed one. Both arms
end at tokens/PE 1.11 and a measured device imbalance of 1.27. With `-p 10`
(the hot set moves every 10 steps, balancing every 20) `-R 2` sync made 3
copies, retired 2 and moved 23 experts, checksums equal to its plain run, and
lost: 173.6 against 136.0 ms/step. Every decision there balances a hot set
that is gone the next step, as before, and a copy costs its exchange every
step until it is retired a round later.

Against the PyTorch and NCCL reference (`ref/`, same node class, same
arguments, `--fuse`, 4 lanes, fp32; `--replicate` is EPLB-style redundancy:
a second copy of the hottest expert, odd ranks' tokens to it, gradients
all-reduced between the copies; greedy re-placement for the rest):

| implementation                            | ms/step |
|-------------------------------------------|--------:|
| reference, no balancing                   |   289.2 |
| reference, greedy re-placement            |   188.8 |
| reference, greedy + redundancy, sync      |   171.1 |
| reference, greedy + redundancy, async     |   170.9 |
| this app, no balancing                    |   179.7 |
| this app, ShedLB with copies, sync        |   122.1 |
| this app, ShedLB with copies, async       |   115.3 |

The two agree on `out2`/`w2` to 4e-10 (same computation, different GEMM
library). The app's lead comes from the overlap work of 2026-10-25
(lookahead, token channels, streamed outputs), which the reference does not
have, and holds with replication on both sides.

Against Megatron-Core 0.20 (`ref/megatron_moe.py`: Megatron's own MoE layer
in expert parallelism with its all-to-all token dispatcher, the example's
hashed routing substituted for its top-k so the token skew per step is the
same, static expert placement as Megatron trains, SGD; same arguments, one
A40 node, `ref/run_mg.sh`). Megatron initializes its own weights, so only
throughput compares; steady state is steps 33-63 as above.

| Megatron-Core arm                                 | ms/step |
|---------------------------------------------------|--------:|
| fp32, one GEMM per expert, z 1.3                  |   294.1 |
| fp32, grouped GEMM, z 1.3                         |   294.1 |
| bf16, grouped GEMM, z 1.3                         |    95.5 |
| fp32, one GEMM per expert, uniform routing (z 0)  |   161.2 |
| bf16, grouped GEMM, uniform routing (z 0)         |    59.9 |

At the same precision Megatron runs at the reference's unbalanced number (294
against 289): the same strict sequence of dispatch all-to-all, the slowest
GPU's experts, combine all-to-all, and the backward of each, with the hottest
expert's GPU setting the pace (forward 248 ms on the slowest rank, 144 on the
fastest). The uniform-routing arm is the floor that perfect balancing inside
that structure could reach with the same token count: 161 ms/step, 1.4x the
app's 115 with the skew still in the data. Grouped GEMM buys nothing in fp32
on an A40, where one expert's GEMM already fills the GPU; bf16 tensor cores
buy 3x, which this fp32 example does not use. The same-node control on the
Megatron node ran plain 179.6 and ShedLB `-R 2` async 109.0 ms/step (one copy,
9 moves) with the checksums above.

Three things the runs showed:

- **The memory ledger needs a pre-sized pool.** With 1024 MB arenas mapped on
  demand, and even with one 16 GB arena, ShedLB refused every copy ("would
  need 2 copies, but no bin can hold one"): the buddy pool rounds each block
  to a power of two, so an expert's 521 MB of state occupies 896 MB, 16
  experts take 14.3 GB, and the plannable headroom is gone. A 32 GB arena per
  PE works on one node. Moves were refused for the same reason before.
- **The per-PE rate misled the balancer** (see "The load declaration" above).
  With it, the async arm ran 140.1 ms/step: at its second round two GPUs
  read twice the others' seconds per token with tokens within 15%, most of
  their experts were moved away, the run stayed at a 1.36 token imbalance,
  and at step 60 the copy of the still-hottest expert was retired on a load
  that read as cooled. With the job-wide rate the same arm runs 115.3 and
  every GPU declares on one scale.
- **The update scheme costs.** At the defaults (z 1.0, `-p 10`, no
  balancing) the previous binary, with its in-place per-chunk SGD update,
  ran 98.8 ms/step; the accumulate-then-apply scheme that bit-identity across
  copies requires ran 117.6, 19% more: one pass over every expert's weights
  per step (16 experts x 512 MiB of traffic per GPU, ~12 ms) and a second
  chunk grid for the second partition of every expert. The cost falls with
  model size and with fewer experts per GPU (estimated ~4% at the Llama size
  on two nodes); it was measured here, not there.

## Measured 2026-10-31, later: bf16, serving, real-model shapes, Megatron-Core

Two modes were added for this comparison. `-B` runs the GEMMs on bf16
operands (the tokens, activations and outputs, and a bf16 copy of each
weight) with fp32 accumulation, fp32 master weights, moments and gradient
accumulators: the mixed precision a framework trains in. The exchange bytes
halve; the checksums differ from fp32's and are placement-independent as
before, so every arm below agrees with its plain arm bit for bit. `-G`
serves: the forward alone, no gradient, no update and no exchange between an
expert's copies, which then cost one state send each; `w2` stays its initial
value and `out2` is still the oracle. The bf16 values are raw 16-bit words
with the conversions written by hand (`moe.cu`), so the CUDA and HIP builds
share one path; the mixed-precision GEMM is `hapiBlasGemmEx` in
`../hapi_blas.h`. The Kokkos variant has the same kernels and the same
smoke checksums bit for bit. The reference's `--dtype bf16` became the same
mixed precision (its `out2` agrees with `-B`'s to 3e-7); the Megatron-Core
harness gained `--forward-only`. The one-node arms below ran without the
balancer's helper thread (`CHARM_LB_HELPER=near`, which takes the strategy
and the CUPTI load build off the PE); the runner sets it from the two-node
arms on. It changes the balancing steps' own cost, not the steady state
between rounds that the tables report.

Default shape (64 experts of 2048 x 8192, 8192 tokens per process, `-z 1.3
-p 0`, steady steps 33-63, one A40 node), ms/step:

| arm                                            | fp32 training | bf16 training | bf16 serving |
|------------------------------------------------|--------------:|--------------:|-------------:|
| this app, plain                                |         179.7 |          62.4 |         22.9 |
| this app, `-X`                                 |         143.1 |          64.0 |         18.6 |
| this app, ShedLB `-R 2`, sync                  |         122.1 |          58.9 |         21.3 |
| this app, ShedLB `-R 2`, async                 |         115.3 |          52.8 |         20.2 |
| Megatron-Core, grouped GEMM                    |         294.1 |          95.5 |         54.5 |
| Megatron-Core, uniform routing (its balanced floor) |    161.2 |          59.9 |              |
| reference, no balancing                        |         289.2 |         156.8 |              |
| reference, greedy + redundancy, async          |         170.9 |         131.2 |              |

(Megatron-Core serving in fp32: 150.1.) In bf16 the step is a third of
fp32's and the copy's gain shrinks from 36% to 15%: the exchange of the
copies' fp32 partials (two 64 MB messages each way per step) and the second
apply pass do not shrink with the GEMMs, and `-X` is now slower than plain.
Serving, a copy exchanges nothing, and the balancer's copy and `-X` both
beat plain.

Real-model layer shapes, with the routing skew set to what the trained
models show per expert, a hottest expert 1.4-1.6x the mean (the per-domain
routing plots of the Mixtral and OLMoE papers; the loss-free balancing
paper's MaxVio of 0.04 for its method against 0.52-0.72 for its
auxiliary-loss baselines), steady steps 23-43, ms/step:

| shape                                                                  | app fp32 | app bf16 | Megatron fp32 | Megatron bf16 |
|------------------------------------------------------------------------|---------:|---------:|--------------:|--------------:|
| Mixtral-8x7B: 8 experts of 4096 x 14336, top-2, 8192 tokens, z 0.3     |    507.4 |    126.6 |         845.5 |         236.4 |
| DeepSeek-V3: 256 routed experts of 7168 x 2048, top-8, 4096 tokens, z 0.1 | 621.2 |    295.1 |        1136.6 |         494.6 |

The app leads 1.7-1.9x at both shapes, and the balancer has nothing to do at
either: a 1.5x hottest expert averages away over the 2 or 64 experts each of
4 GPUs holds (per-GPU token ratios 1.10 and 1.02, under the 10% threshold),
and the ShedLB arms cost their balancing steps and nothing else (519.0 /
130.1 and 672.1 / 311.0). Imbalance a balancer can act on comes from fewer
experts per GPU (Mixtral on 8 GPUs holds one each, and its hottest expert's
18% exceeds a GPU's 12.5% share; not run, two nodes) or from batch-level
skew: an auxiliary loss balances the load over the data mix, not within a
batch, so a batch of one language, one repository or mathematics alone
routes 2-3x the mean to that domain's specialists, and serving sees the
same, which is why DeepSeek-V3's deployment duplicates hot experts (EPLB).

The Mixtral-8x7B shape with such a router (`-z 1.0`: the hottest expert
takes 37% of the tokens, the per-GPU token ratio is 1.47; ShedLB decides one
copy at step 20, serving from step 23), ms/step:

| arm                                       | training fp32 | training bf16 | serving fp32 | serving bf16 |
|-------------------------------------------|--------------:|--------------:|-------------:|-------------:|
| this app, plain                           |         672.6 |         177.2 |        287.4 |        110.3 |
| this app, ShedLB `-R 2` async, one copy   |         602.9 |         197.1 |        219.6 |         85.8 |
| Megatron-Core, grouped GEMM               |        1108.6 |         302.1 |        536.4 |        163.7 |

Training in fp32 the copy takes 10% off the step; in bf16 it adds 11%: the
copies' partial exchange is 470 MB each way per step at this shape (4096 x
14336 in fp32) and a 177 ms step cannot hide it. Serving, the copy takes 24%
(fp32) and 22% (bf16) off, with nothing to exchange. The checksums are the
same bits with and without the copy in every arm.

### Two A40 nodes, eight GPUs (2026-10-31, job 23388275)

Same recipe as above (`-i 40 -u 3`, steady state steps 23-43, LB helper
thread on), one PE per GPU, `+gpupoolalloc vmm +gpupoolsize 16384
+gpupoolchunk 1536`. The two shapes are the Mixtral-8x7B expert (8 experts of
4096 x 14336, top-2, 8192 tokens per GPU, the trained skew `-z 0.3`: one
expert per GPU, the hottest 1.4x the mean) and the default shape at `-z 1.0`
(64 experts of 2048 x 8192, eight per GPU, the hottest expert 21% of the
tokens). "copies" is ShedLB (`-R 2 -f 20 -b 20 -a`); "plain" is `-R 1
-f 99999`. Megatron-Core is expert parallel over the 8 GPUs, grouped GEMM,
`--forward-only` for serving. Checksums (`out2`, `w2`) are bit-identical
between plain and copies in every pair.

| Mixtral-8x7B shape, trained skew       | plain | copies (what ShedLB did)        | Megatron-Core |
|----------------------------------------|------:|--------------------------------:|--------------:|
| fp32 training                          | 649.4 | 656.5 (2 copies)                |        1155.0 |
| bf16 training (`-B`)                   | 164.7 | 212.2 (2 copies)                |         337.7 |
| fp32 serving (`-G`)                    | 280.0 | 290.7 (2 copies)                |         570.0 |
| bf16 serving (`-B -G`)                 | 113.4 | 153.1 (2 copies)                |         201.3 |

| default shape, `-z 1.0`, fp32 training | plain | copies (what ShedLB did)        | Megatron-Core |
|----------------------------------------|------:|--------------------------------:|--------------:|
| 64 experts, 8 per GPU                  | 198.0 | 134.1 (1 copy + 6 moves)        |         385.0 |

The default shape is the case the design was for: with eight experts per
GPU the hottest expert (21% of the tokens, 1.69x a GPU's share) is split
over two GPUs and the rest re-placed around it, tokens per GPU max/mean
2.86 -> 1.69, the step -32%. The Mixtral shape on eight GPUs has one expert
per GPU, each 0.7-1.4x the mean: a copy of the hottest expert has to land
on a GPU that already holds a whole expert, which leaves that GPU as full
as the one the copy relieved, and the exchange is paid for nothing (the
balancer at the time took the emptiest bin when no bin cleared the target;
this run is why ShedLB now declines a copy that does not lower the fullest
bin it touches, `doc/internals/lb-multiplicity.md`; with that rule, same
nodes: bf16 training plain 166.2, ShedLB 170.9 with both copies declined,
165.5 against 165.5 over steps 33-43; the default shape keeps its copy and
its six moves, 197.4 -> 134.2). bf16 pays most: the
gradient partials are fp32 (470 MB per copy per step at this shape) and in
serving the copies only split work a single GPU did in 113 ms.

**The Mixtral shape with the unbalanced router (`-z 1.0`: hottest expert
37% of the tokens, lightest 5%), eight GPUs, one expert each** (job
23389495, same nodes and flags, `+gpupoolchunk 2046`). This is the case a
copy pays with one expert per GPU: the hottest expert is more than twice
the lightest, so half of it fits beside the lightest. ShedLB made the same
decision in every arm: expert 1 copied to PE 0, expert 4 copied to PE 5, one
move; tokens per GPU max/mean 2.53 -> 1.27. The fp32 slab block at this
skew is 2.4 GB with lookahead (capacity 9120 tokens per source), more than
any registrable chunk, so the fp32 arms ran with lookahead off (`-Y`, one
slab parity, both arms alike); the bf16 arms ran as usual, and once with
`-Y` for the comparison. Checksums identical plain vs ShedLB in every pair.
Steady state steps 23-43, and steps 33-43 in brackets (the copies land at
step 22).

| Mixtral-8x7B shape, unbalanced router, 8 GPUs | plain           | ShedLB (2 copies + 1 move) | change     | Megatron-Core EP8 |
|-----------------------------------------------|----------------:|---------------------------:|-----------:|------------------:|
| fp32 training, `-Y`                           | 1209.9 (1222.2) |              692.7 (687.2) | -43%       |            1882.1 |
| bf16 training                                 |   276.2 (276.2) |              219.1 (203.8) | -21% (-26%)|             522.6 |
| bf16 training, `-Y`                           |   294.4 (294.1) |              224.7 (217.6) | -24% (-26%)|                   |
| fp32 serving, `-Y`                            |   491.9 (495.9) |              282.0 (287.2) | -43%       |             908.8 |
| bf16 serving                                  |   121.8 (126.5) |               113.3 (98.9) | -7% (-22%) |             279.6 |

**FasterMoE on the same case** (job 23403736, gpub002+062, 2026-11-01, same
eight GPUs' worth of A40s on two nodes): FastMoE 1.1 as shipped ("plain", two
all-to-alls around the experts, static placement), with FasterMoE's smart
schedule, and with the schedule plus expert shadowing, which is the PPoPP'22
FasterMoE system: before every step its performance model picks the experts
that would be the critical path, broadcasts their weights to all eight GPUs
(each GPU then runs its own tokens for them) and reduces their gradients back
to the owner after the backward. Harness, launcher and the three patches the
shipped code needed are in `ref/README.md`; the policy's machine parameters
were measured on these ranks (470 MB expert broadcast to 8 ranks 117-154 ms,
GEMM 23.7 TFLOP/s fp32, 123 bf16). Steady state steps 23-43 (33-43 in
brackets); "shadows" is how many of the 8 experts FasterMoE shadowed every
timed step.

| Mixtral-8x7B shape, unbalanced router, 8 GPUs | FastMoE plain   | + smart schedule | FasterMoE (schedule + shadows) | example plain | example ShedLB | Megatron-Core |
|-----------------------------------------------|----------------:|-----------------:|-------------------------------:|--------------:|---------------:|--------------:|
| fp32 training                                 | 2102.5 (2106.8) |  2018.6 (2023.4) |   1195.0 (1195.8), 3 shadows   |        1209.9 |          692.7 |        1882.1 |
| bf16 training                                 |   631.2 (631.4) |    579.5 (579.8) |   402.9 (404.6), 2 shadows (a) |         276.2 |          219.1 |         522.6 |
| fp32 serving                                  |   901.9 (904.2) |          (b)     |   661.5, 3 shadows (b)         |         491.9 |          282.0 |         908.8 |
| bf16 serving                                  |   276.3 (276.3) |          (b)     |   306.7, 2 shadows (b)         |         121.8 |          113.3 |         279.6 |

| default shape, `-z 1.0`, fp32 training        | FastMoE plain   | + smart schedule | FasterMoE (schedule + shadows) | example plain | example ShedLB | Megatron-Core |
|-----------------------------------------------|----------------:|-----------------:|-------------------------------:|--------------:|---------------:|--------------:|
| 64 experts, 8 per GPU                         |   509.6 (507.9) |    449.1 (448.1) |   322.1 (322.9), 1.6 shadows   |         198.0 |          134.1 |         385.0 |

(a) FasterMoE's bf16 shadow path is numerically broken: the loss is inf or
NaN from step 2 in every bf16 shadow run while plain and schedule-only bf16
stay finite, the weights of every expert are still finite at the end (a
one-step probe, 2026-11-01), so the shadowed experts' outputs themselves are
wrong; the shipped code did not compile that path for bf16 at all (the
dispatch patch enabled it). The time is a throughput figure only. The fp32
shadow run reproduces FastMoE plain's loss to six digits at steps 1, 3 and
10, so the fp32 rows compare equal work. (b) FasterMoE's fused forward keeps
the experts' recorded sub-graphs until a backward consumes them, so a
forward-only run grows by the hot rank's activations every step: the
schedule-only serving arms ran out of 44 GB on the hot rank in every attempt
(under `no_grad`, with grad enabled, with a garbage collection per step),
and the shadow arms, whose hot rank holds a smaller share, reached step 30
with the collection but not step 43. The shadow serving rows are therefore
28-step runs (`-i 25 -u 3`), steady state steps 14-28, with 3 and 2 experts
shadowed every step (the fp32 forward sat at 650-720 ms throughout).

Shadowing takes FasterMoE from 43% slower than Megatron to the example's
plain arm (1195 against 1210 in fp32 training, 662 against 492 serving)
and no further: a shadow is eight
copies of a 470 MB expert re-broadcast and reduced every step, so the
model stops at three experts and the fp32 step keeps about 500 ms of that
traffic; ShedLB's second copy is one persistent replica with one 470 MB
partial exchange per step, 692.7. At the default shape (eight experts per
GPU) FasterMoE shadows one or two of 64 experts and lands at 322 against
the example's 198 plain and 134 with the copy and six moves. FlexMoE
(SIGMOD'23) has no public code; its paper reports 1.30x over FasterMoE on
average, which is not a measurement on this case.

What the invalid evening runs were: a VMM pool block bigger than a chunk
spans chunks, and the fabric registers one chunk at a time, so a remote
write into the part of a slab beyond its first chunk stopped at the chunk
end (`CHARM_MOE_SLABPROBE=1` showed exactly 9 MiB of each affected segment
landing, every one 9 MiB before a 256 MiB boundary). `+gpupoolchunk` above
the largest block fixes that, but not any value: a 2048 MB chunk registers
without error and then accepts no remote write at all (every rank, first
step, `ADDR_OUT_OF_RANGE`), while 1536, 1792 and 2046 MB carry every
transfer exactly (the probe: 0 of 64 slabs differ): the ceiling is 2^31
bytes. The runtime now refuses a VMM chunk of 2048 MB or more on more than
one node (`hapiDevPoolCheckRegistrable`).

## Measured 2026-11-01: real routing, two A40 nodes, eight GPUs

> **Superseded in part (2026-11-02):** the NCCL baselines in this section
> ran without the Slingshot plugin and the example's serving arms from
> "Copies up to four" on ran with a serving bug; the next section has the
> re-measured matrix and the corrected ratios. The example's training rows
> and the four-GPU rows stand.

Everything above routes a hashed Zipf. This section replays recorded routing
(`-E`, `--trace`; `ref/README.md` has the tools; the traces are release
`traces-2026-11-01` of <https://github.com/adityapb/charm-moe-traces>, 473 MB
packed, fetched by its `fetch_traces.sh`), bf16 throughout, lookahead
on, with the attention sublayer (`-w`, `--attn`) so a step is a transformer
block's, and every system on the same tokens. Three sources, from the mildest
to the strongest skew:

- **Mixtral-8x7B on WikiText-103, layer 12** (`traces/mixtral_L12.trace`, the
  checkpoint's own router): 8 experts, top-2; the hottest expert takes 15.9%
  of the (token, slot) pairs and the lightest 3.1%, so one GPU of eight holds
  1.3x the mean. Mixtral's inference routing is close to balanced (13-17% in
  every layer); this is its most unequal layer.
- **OLMoE-1B-7B on WikiText-103, layer 12** (`traces/olmoe_L12.trace`): 64
  experts, top-8; the hottest 5.7% (3.6x the mean), but 0.46 of a GPU's share
  with eight experts per GPU: placement balances it, no copy is needed.
- **SmartMoE's training routing, layer 2** (`traces/smartmoe_L02.trace`,
  thu-pacman/SmartMoE-AE: the tokens each of 32 experts received per
  micro-batch while a GPT-MoE trained without a balancing loss, top-1): the
  hottest expert takes 47.9% of the tokens, 3.8x a GPU's share with four
  experts per GPU, and the hot set drifts over the training run.

Arms (each in training and serving): the example plain (`-R 1`) and with
ShedLB (`-R 2 -f 20 -b 20 -a`); the PyTorch reference static, with EPLB's
packing recomputed every 10 steps (`--lb sync --place lpt`), and with one
copy of the hottest expert from the start (`--replicate`); Megatron-Core
grouped GEMM, dropless and with capacity factor 1.25; FastMoE as shipped and
FasterMoE (smart schedule and shadowing, the patched build). Steady state
steps 23-43 of 40 + 3 warmup; repeats where a band is given. Scale is 4 and
8 GPUs: the experiments are capped at two nodes.

**Results, MoE layer alone** (job 23411075, gpub092+094, 2026-11-01, bf16,
8 GPUs, ms/step, steady state steps 23-43; "ShedLB" is the balancer as of
the morning, "ShedLB, critical path" the rule described below; checksums
identical between the example's arms in every pair):

| Mixtral-8x7B routing, layer 12 (bands: mean +- half-range of 3 runs on gpub001+002) | training | serving |
|-----------------------------------------------|---------:|--------:|
| example plain                                 | 162.0 +- 5.6 | 120.1 +- 1.6 |
| example ShedLB, morning rule (1 copy, 1 run)  |    185.9 |   119.8 (102.5 settled) |
| example ShedLB, critical-path rule (no copy)  | 164.8 +- 1.5 | 125.6 +- 9.5 |
| PyTorch reference, static                     |    334.5 |   197.6 |
| PyTorch reference, EPLB packing every 10 steps|    333.9 |   203.8 |
| PyTorch reference, one static copy            |    673.1 |   227.2 |
| Megatron-Core, grouped GEMM                   | 351.6 +- 16.9 | 182.3 +- 3.4 |
| Megatron-Core, capacity factor 1.25           |    337.3 |         |
| FastMoE as shipped                            |    386.3 |   193.2 |
| FasterMoE (schedule + shadows: none chosen)   | 376.3 +- 8.3 | 254.0 +- 2.2 |

| SmartMoE training routing, layer 2 (mean +- half-range of 3 runs) | training | serving |
|-----------------------------------------------|---------:|--------:|
| example plain                                 | 79.9 +- 0.1 | 33.4 +- 0.4 |
| example ShedLB, critical-path rule (1 copy)   | 50.9 +- 1.5 (47-49 settled) | 27.8 +- 1.2 (23-27 settled) |
| example ShedLB, morning rule (1 copy, 1 run)  |     51.6 |    25.8 |
| PyTorch reference, static                     | 178.4 +- 1.2 | 100.8 +- 1.5 |
| PyTorch reference, EPLB packing every 10 steps| 201.6 +- 2.2 | 127.9 +- 0.5 |
| PyTorch reference, one static copy            | 232.5 +- 1.2 | 73.3 +- 0.9 |
| Megatron-Core, grouped GEMM                   | 164.6 +- 0.5 | 95.4 +- 0.6 |
| Megatron-Core, capacity factor 1.25           | 165.1 +- 0.6 |         |
| FastMoE as shipped                            | 239.8 +- 1.8 | 106.6 +- 0.3 |
| FasterMoE (1.3 shadows per step, loss NaN from step 7-8) | 113.1 +- 6.8 | 130.7 +- 1.1 |

What the two routings say. On Mixtral's real routing every balancer is
idle or harmful: the hottest expert is 1.27x the mean with one expert per
GPU, EPLB's packing cannot change a one-per-GPU placement, a static copy in
PyTorch pays an fp32 partial all-reduce across the node link every step
(2x slower), FasterMoE's model chooses no shadow, and ShedLB's copy lowered
its own GPU but not the fullest one (+15% in training). The example's plain
arm is 2.2x faster than Megatron and 2.4x faster than FastMoE on the same
tokens, which is the runtime's token exchange and pipelining, not
balancing (bands of 3-5% on the example and FasterMoE, 5% on Megatron,
across two node pairs). On SmartMoE's training routing (the hottest expert 3.8x a GPU's
share) the balancer is the story: one copy and the critical-path rule take
the example from 80.1 to 52.2 in training and 33.4 to 28.0 in serving,
against FasterMoE's 113.1 (its shadows, bf16 numerics broken), Megatron
164.6, and the PyTorch reference's EPLB-style packing 201.6 and static copy
232.5; the example with ShedLB is 2.2x faster than FasterMoE and 3.2x faster
than Megatron in training, 3.4x faster than Megatron in serving, with bands
of 1-3% on every arm but FasterMoE's (6%). The copies
cap of two is what bounds it: the hottest expert's half is still 1.9x the
mean.

| OLMoE-1B-7B routing, layer 12 (critical-path rule) | training | serving |
|-----------------------------------------------|---------:|--------:|
| example plain                                 |    237.7 |   243.6 |
| example ShedLB (nothing to decide)            |    262.3 |   248.8 |
| PyTorch reference, static                     |    333.2 |   314.5 |
| PyTorch reference, EPLB packing every 10 steps|    308.5 |   284.0 |
| Megatron-Core, grouped GEMM                   |    455.6 |   280.2 |
| FastMoE as shipped                            |    697.4 |   332.5 |
| FasterMoE (2.8 shadows per step, loss NaN from step 3) | 641.1 | 403.0 |

OLMoE's routing (64 experts of 2048 x 1024, top-8, the hottest 0.46 of a
GPU's share) leaves the balancer nothing to do: per GPU the load is within
the tolerance, and the two balancing rounds in the window cost the example
the lookahead for a few steps each (+10% in training, +2% in serving) for no
decision. With eight slots per token the step is the token exchange: serving
costs what training does. EPLB's packing buys the PyTorch reference 7-10%
there; the example's plain arm is 1.3-1.4x faster than it and 1.2-1.9x
faster than Megatron.

**Results, transformer block** (the same arms with the attention sublayer in
every system, `-w` / `--attn`: q/k/v/o projections of 2048 x 2048, 32 heads,
causal, sequences of 1024, trained on its own loss; SmartMoE routing, job
23411075, critical-path rule):

| SmartMoE training routing, block              | training | serving |
|-----------------------------------------------|---------:|--------:|
| example plain                                 |     98.0 |    35.6 |
| example ShedLB (1 copy)                       |     63.8 |    30.0 (28.2 settled) |
| PyTorch reference, static                     |    185.8 |   103.2 |
| PyTorch reference, EPLB packing every 10 steps|    212.1 |   131.9 |
| PyTorch reference, one static copy            |    241.2 |    73.7 |
| Megatron-Core, grouped GEMM                   |    175.6 |    97.8 |
| Megatron-Core, capacity factor 1.25           |    174.7 |         |
| FastMoE as shipped                            |    249.6 |   109.9 |
| FasterMoE (schedule + shadows, bf16 numerics broken) | 119.2 | 132.5 |

| Mixtral-8x7B routing, block (4096 x 4096 projections) | training | serving |
|-----------------------------------------------|---------:|--------:|
| example plain                                 |    178.9 |   137.9 |
| example ShedLB, critical path (no copy)       |    188.4 |   134.2 |
| PyTorch reference, static                     |    358.3 |   221.6 |
| PyTorch reference, EPLB packing every 10 steps|    362.3 |   205.8 |
| PyTorch reference, one static copy            |    695.9 |   242.8 |
| Megatron-Core, grouped GEMM                   |    368.3 |   202.6 |
| Megatron-Core, capacity factor 1.25           |    369.2 |         |
| FastMoE as shipped                            |    421.0 |   201.4 |
| FasterMoE (schedule + shadows: none chosen)   |    417.9 |   267.4 |

The attention sublayer adds the same local work to every system (about 18
ms of training step to the example at both shapes), so the ratios shrink a
little and the ordering does not change: on SmartMoE's routing the example
with ShedLB is 1.9x faster than FasterMoE and 2.8x faster than Megatron in
training, 3.3x faster than Megatron in serving; on Mixtral's the plain
example is 2.1x faster than Megatron in training and the balancer, with
nothing to decide, costs its two rounds' lookahead pauses (+5%).

**Scale: four GPUs** (one node, the SmartMoE routing, MoE layer alone, eight
experts per GPU, critical-path rule; the 8-GPU numbers are the table above):

| SmartMoE training routing, 4 GPUs             | training | serving |
|-----------------------------------------------|---------:|--------:|
| example plain                                 |     55.4 |    32.9 |
| example ShedLB (1 copy)                       |     50.4 (44.1 settled) | 27.3 (17.2 settled) |
| PyTorch reference, static                     |    105.9 |    52.8 |
| PyTorch reference, EPLB packing every 10 steps|    108.6 |    68.8 |
| PyTorch reference, one static copy            |    102.7 |    42.2 |
| Megatron-Core, grouped GEMM                   |     90.2 |    52.1 |
| Megatron-Core, capacity factor 1.25           |     87.8 |         |
| FastMoE as shipped                            |    121.4 |    52.1 |
| FasterMoE (schedule + shadows, bf16 numerics broken) | 62.8 | 82.3 |

| Mixtral-8x7B routing, 4 GPUs (two experts each) | training | serving |
|-----------------------------------------------|---------:|--------:|
| example plain                                 |    137.8 |   108.7 |
| example ShedLB (no copy; moves)               |    137.3 |   100.7 |
| Megatron-Core, grouped GEMM                   |    270.8 |   138.1 |
| FasterMoE (no shadow chosen)                  |    318.0 |   188.2 |

From four to eight GPUs the example's plain step on the SmartMoE routing
goes 55.4 to 80.1 (the hottest expert is a larger multiple of a GPU's
share, and the exchange crosses the node link) and the balanced step 44.1
to 49.8 once the copy is in place; Megatron goes 90.2 to 165.0 and FasterMoE
62.8 to 107.0, so the gap widens with scale: 1.2x over FasterMoE at four
GPUs, 2.0x at eight. On Mixtral's routing the example is 2.0x faster than
Megatron at four GPUs and 2.2x at eight. Two points are what two nodes
allow.

**Baseline tuning** (job 23414031, SmartMoE routing unless said, MoE layer
alone): FasterMoE's group-wise exchange at group sizes 2 / 4 (default) / 8
trains at 104.7 / 113.1 / 107.2 and serves at 128.2 / 130.7 / 130.5, so its
best setting is 2.1x the example with ShedLB; Megatron's allgather token
dispatcher is slower than the all-to-all one used in the tables (234.2
against 164.6 training, 135.8 against 95.4 serving; on Mixtral 461.4 against
351.6), and its capacity factor 1.25 changes nothing at these skews (the
hottest expert is hot on every GPU's tokens alike). The PyTorch reference
supports one copy, so the redundancy count was not swept.

**The critical-path rule** (ShedLB, 2026-11-01 afternoon): a copy is made
only if it lowers the fullest bin anywhere by more than `eps * mean`, not
only the fullest of the bins it touches. On Mixtral it declines every copy
(the gain would be 0.14 of a 2.23 mean against the 0.22 tolerance) and
training returns to the plain time; on SmartMoE it makes the same copy as
before.

**Copies up to four, validated** (2026-11-01 evening, job 23418061, one A40
node, four GPUs; `-C 4` checksums at every fourth step, the last ones
compared): on the SmartMoE routing the plain arm, `-X` (two copies from the
start) and ShedLB with `-R 2`, `-R 3` and `-R 4` give one set of checksums
(2.576076019366897e+06, 1.091965310301654e+05); the Mixtral routing plain
against `-R 4` in training and serving likewise. A drifting hashed skew
(`-z 1.5 -p 10 -R 2`) made three copies and retired three as the hot set
moved, declined one by the cost rule, and matched its plain arm; a dominant
expert (`-z 3.0`, 32 experts of 2048 x 1024, `-R 4`) went to two, three
and four copies over successive rounds, every copy receiving the balancer's
notice and switching in the same step, and matched its plain arm through
the three-copy and four-copy stretches (steps 20-43 and 44-73). On this
node the default-shape experts' third copy was refused by the memory ledger
("no bin can hold one"), which is a question about the ledger's view of a
16 GB pool, not about the protocol. With experts this small the copies'
per-step cost is dominated by the exchange messages themselves (the `-R 4`
step 4x the plain step at 8 MB weights), which the declared cost, bytes
over bandwidth, does not see; at real shapes the bytes are what counts.

**A kernel bound, found and removed** (2026-11-02): the same ladder on
eight GPUs (job 23419187) gave the SmartMoE routing with three copies
(`-R 3`, `-R 4`) w2 checksums that differed from the plain arm's from step
24 on, the first step with three copies, in copy 0 alone, while every
operand copy 0 summed matched the other copies' bit for bit. `nblocks` in
`moe.cu` caps a launch at 65535 blocks of 256 threads, and four kernels
had no loop over the grid: the tree-node add of the copies' partials
(`addIntoKernel`), the two attention reshapes of `-w` and its weight update
(`toHeadsKernel`, `mergeHeadsKernel`, `sgdPlainKernel`). Past 16,776,960
elements they stopped. A 2048 x 8192 expert's gradient is 16,777,216
elements, so the add left its last 256 unadded, and only copy 0 of three
had the operands in the order that put the missing tail on the landed
side. At the Mixtral shape (4096 x 14336) the add missed 71% of every
block sum in every arm, the plain arm included (the four-partition tree
adds partition sums whether or not an expert is copied), so the arms
agreed with each other while all were wrong; `-w`'s reshapes and its
update were partial at that shape too. The four kernels now loop over the
grid. Measured before the fix, so to be re-measured: every training row
of the four-partition binary (the paragraphs above from "Copies up to
four" on, whose checksums came from that binary; on four GPUs the experts
were below the bound, so those arms agreed among themselves), and every
`-w` row at both shapes. The timing is expected to move little, since the
missing work was the tail of one element-wise pass. The re-measurement
(the checksum ladder on eight GPUs, then the touched rows) was queued on
Delta and cancelled unrun when development moved to Frontier; it is the
first job there (`FRONTIER_START.md`).

**The copy's cost, declared** (2026-11-01 evening): the balancer now prices
every copy as its share plus its exchanges with the other copies, each
exchange at the cost the application declares for a peer on the same node
or on another (`setLBReplica`'s two last arguments). The example measures
the cost at start-up: before the experts initialise, PE 0 sends one partial
(the fp32 gradient pair, 470 MB at the Mixtral shape) to a PE on its node
and to the first PE of the other node, twice each, and the second transfer's
time is printed (`Exchange probe:`) and declared by every expert scaled to
the balancing interval, zero when serving (`-G`), where the copies exchange
nothing. A retire now takes the highest-ranked copy, when the group fits
fewer copies or when the exchanges cost the critical path more than the
sharing saves. The same change lets a round that decides nothing cost
nothing: every PE learns the round's job-wide decision count
(`LBManager::LastRoundDecisions`) and the dispatchers resume the lookahead
inside the round's window when it is zero. Rule and plumbing:
`doc/internals/lb-multiplicity.md`.

## Measured 2026-11-02: the same matrix with NCCL on Slingshot, and a serving fix

**Correction.** Every NCCL baseline of the 2026-11-01 section (the PyTorch
reference, Megatron-Core, FastMoE, FasterMoE) ran without the `aws-ofi-nccl`
plugin, so across the two nodes NCCL did not use Slingshot through libfabric:
its logs at `NCCL_DEBUG=INFO` show `NET/IB : No device found` and no plugin,
which leaves its socket transport. The example's own exchange goes through
LCI over the cxi provider. The baselines' two-node numbers were therefore
too slow and the ratios quoted there too large; the one-node (four-GPU)
rows, where NCCL stays on P2P and shared memory, and the example's own rows
stand. `ref/refwrap.sh` now sets what `module load aws-ofi-nccl/1.19.2` sets
(`NCCL_NET_PLUGIN=ofi`, `FI_PROVIDER=cxi`, `FI_CXI_DISABLE_HOST_REGISTER=1`,
`FI_MR_CACHE_MONITOR=userfaultfd`, the plugin and libfabric 2.3.1 on the
library path) and logs NCCL's INIT and NET lines, so every rank log carries
`Using network Libfabric`. Without the userfaultfd monitor one arm (Mixtral,
EPLB packing, 704 MB experts) hung inside NCCL's tree connection on all
eight ranks, the libfabric memhooks deadlock with CUDA (job 23452305); with
it, the arm ran. The FasterMoE calibration on the plugin: the 470 MB
broadcast to 8 ranks 154 -> 87.8 ms, so its shadow policy ran with
NETBW 5.35 GB/s instead of 3.05.

Jobs 23452305 (gpub002+012, MoE layer alone, SmartMoE and Mixtral) and
23454982 (gpub030+089, OLMoE, the block variants, the re-run arm), bf16, 8
GPUs, steady state steps 23-43 of 40 + 3, one run per arm; the example's
arms in the same jobs. The first column of each pair is the 2026-11-01 value
for reference, the second the re-measurement.

**MoE layer alone** (ms/step; "old | new"):

| SmartMoE training routing, layer 2        | training        | serving         |
|-------------------------------------------|----------------:|----------------:|
| example plain                             | 79.9 \| 81.1    | 33.4 \| 35.3 |
| example ShedLB, critical-path rule (1 copy)| 50.9 \| 52.1   | 27.8 \| 27.4 (23.4 settled) |
| PyTorch reference, static                 | 178.4 \| 158.5  | 100.8 \| 78.4   |
| PyTorch reference, EPLB packing every 10  | 201.6 \| 171.4  | 127.9 \| 97.2   |
| PyTorch reference, one static copy        | 232.5 \| 128.9  | 73.3 \| 53.5    |
| Megatron-Core, grouped GEMM               | 164.6 \| 126.6  | 95.4 \| 74.0    |
| Megatron-Core, capacity factor 1.25       | 165.1 \| 127.0  |                 |
| FastMoE as shipped                        | 239.8 \| 190.2  | 106.6 \| 84.1   |
| FasterMoE (schedule + shadows)            | 113.1 \| 76.4   | 130.7 \| 97.5   |

| Mixtral-8x7B routing, layer 12            | training        | serving         |
|-------------------------------------------|----------------:|----------------:|
| example plain                             | 162.0 \| 161.8  | 120.1 \| 117 (112-130) |
| example ShedLB, critical-path (no copy)   | 164.8 \| 164.2  | 125.6 \| 124.8 |
| PyTorch reference, static                 | 334.5 \| 275.5  | 197.6 \| 140.6  |
| PyTorch reference, EPLB packing every 10  | 333.9 \| 289.3  | 203.8 \| 153.9  |
| PyTorch reference, one static copy        | 673.1 \| 351.9  | 227.2 \| 165.4  |
| Megatron-Core, grouped GEMM               | 351.6 \| 263.0  | 182.3 \| 131.2  |
| Megatron-Core, capacity factor 1.25       | 337.3 \| 262.9  |                 |
| FastMoE as shipped                        | 386.3 \| 300.6  | 193.2 \| 129.9  |
| FasterMoE (no shadow chosen)              | 376.3 \| 259.7  | 254.0 \| 161.9  |

| OLMoE-1B-7B routing, layer 12             | training        | serving         |
|-------------------------------------------|----------------:|----------------:|
| example plain                             | 237.7 \| 243.1  | 243.6 \| 249.9 |
| example ShedLB (nothing to decide)        | 262.3 \| 240.8  | 248.8 \| 269.7 |
| PyTorch reference, static                 | 333.2 \| 194.7  | 314.5 \| 172.8  |
| PyTorch reference, EPLB packing every 10  | 308.5 \| 200.9  | 284.0 \| 178.1  |
| PyTorch reference, one static copy        |   -   \| 215.7  |   -   \| 189.1  |
| Megatron-Core, grouped GEMM               | 455.6 \| 261.0  | 280.2 \| 173.3  |
| Megatron-Core, capacity factor 1.25       |   -   \| 261.4  |                 |
| FastMoE as shipped                        | 697.4 \| 556.5  | 332.5 \| 268.2  |
| FasterMoE                                 | 641.1 \| 350.2  | 403.0 \| 230.9  |

**Transformer block** (`-w` / `--attn`; the example's rows are the first
with the attention kernels of 2026-11-02, which loop over the grid):

| SmartMoE routing, block                   | training        | serving         |
|-------------------------------------------|----------------:|----------------:|
| example plain                             | 98.0 \| 102.7   | 35.6 \| 36.4 |
| example ShedLB (1 copy)                   | 63.8 \| 68.9    | 30.0 \| 28.8 |
| PyTorch reference, static                 | 185.8 \| 166.2  | 103.2 \| 80.4   |
| PyTorch reference, EPLB packing every 10  | 212.1 \| 181.6  | 131.9 \| 100.4  |
| PyTorch reference, one static copy        | 241.2 \| 138.1  | 73.7 \| 55.5    |
| Megatron-Core, grouped GEMM               | 175.6 \| 137.0  | 97.8 \| 76.9    |
| Megatron-Core, capacity factor 1.25       | 174.7 \| 137.3  |                 |
| FastMoE as shipped                        | 249.6 \| 201.4  | 109.9 \| 87.3   |
| FasterMoE                                 | 119.2 \| 86.5   | 132.5 \| 100.0  |

| Mixtral routing, block                    | training        | serving         |
|-------------------------------------------|----------------:|----------------:|
| example plain                             | 178.9 \| 183.7  | 137.9 \| 148.1 |
| example ShedLB, critical path (no copy)   | 188.4 \| 184.0  | 134.2 \| 118.6 |
| PyTorch reference, static                 | 358.3 \| 308.3  | 221.6 \| 149.2  |
| PyTorch reference, EPLB packing every 10  | 362.3 \| 319.0  | 205.8 \| 161.1  |
| PyTorch reference, one static copy        | 695.9 \| 384.9  | 242.8 \| 174.2  |
| Megatron-Core, grouped GEMM               | 368.3 \| 296.1  | 202.6 \| 141.2  |
| Megatron-Core, capacity factor 1.25       | 369.2 \| 294.7  |                 |
| FastMoE as shipped                        | 421.0 \| 333.9  | 201.4 \| 140.0  |
| FasterMoE                                 | 417.9 \| 287.1  | 267.4 \| 172.3  |

What the corrected numbers say. On SmartMoE's training routing the
example with ShedLB is 1.5x faster than FasterMoE and 2.4x faster than
Megatron-Core in training (2.2x and 3.2x were quoted), and 3.6x faster than FasterMoE and 2.7x faster than Megatron-Core in serving (3.4x over Megatron-Core was quoted)
in serving. On Mixtral's routing the plain example is 1.6x faster than
Megatron-Core and FasterMoE in training (2.2x and 2.3x were quoted), and
in serving the plain example (112-130 over six runs) is level with Megatron-Core (131.2) and FastMoE (129.9) and 1.4x faster than FasterMoE (1.5x and 1.6x were quoted). On OLMoE's routing, where the step is the
token exchange (top-8), the PyTorch reference's one all-to-all on the
plugin is now faster than the example's one message per expert, 194.7
against 243.1 in training and 172.8 against 249.9 in serving; the 2026-11-01
text had the example 1.3-1.4x ahead there, which was the socket transport,
not the exchange. The baselines gained 11-48% from the plugin, the most
where fp32 partials crossed the node link every step (the reference's static
copy) and in FasterMoE's shadow broadcasts.

**A serving bug, found and removed** (2026-11-02). In the same jobs the
example's serving arms came out 16-30% slower than on 2026-11-01 while its
training arms matched (SmartMoE 43.3 against 33.4, Mixtral 139.3 against
120.1), and in serving the arm with a copy gave a different out2 than the
plain arm at every checksum step from the copy's first step on, where the
training pairs agreed to the last digit. The same node pair ran this
morning's binary against the 2026-11-01 17:13 one: 42.6 and 43.0 against
33.3 and 33.3 ms/step, with the GPU time per step halved (24 against 40 ms),
the same message and kernel counts, and two to three times the LCI send
retries. The serving branch of `Expert::runStep` still read `for (p = 0;
p < 2; p++)` from before the four-partition tree, and a copy ran partition
`rank()` instead of its block: in serving, the dispatchers of partitions 2
and 3 had their output segments never written and sent on never-recorded
events, ran free and flooded the sends. The forward now runs every partition
(a copy's other partitions hold no tokens). Validated in job 23456024 on
gpub001+002 (and 23465756 on gpub091+099): SmartMoE plain serving 35.3 and 35.3 ms/step (43.3 with the bug; this
morning's binary on the same nodes 42.1), with a copy 27.4 (23.4 settled);
OLMoE 249.9 and 269.7; the block variants 36.4 and 28.8 (SmartMoE), 148.1
and 118.6 (Mixtral); every plain-versus-copy serving pair equal to the last
digit, and the training checksum unchanged by the fix. Mixtral's serving
step spreads 112-130 ms over six runs on two node pairs (one run on
gpub001+002 at 155.0), with or without lookahead (114.2 against 116.8) and
with two or four partitions: its step is the 128 MB slab exchange and the
spread is the exchange's, not the binary's; the 2026-11-01 band (120.1 +-
1.6, three runs on one pair) understated it. The serving checksums of the "Copies up to
four, validated" paragraph came from that binary and covered half the
dispatchers' outputs; the training checksums there were right.


**Two partitions against four** (job 23465756, same source built with
`-DMOE_K_PART=2`, `moe.k2`): SmartMoE plain serving 33.9 and 33.7 ms/step
against 35.3 and 35.3 with four, with a copy 28.6 against 27.4; Mixtral
serving 130.2 and 115.8 against 116.8 and 112.4, inside its spread; the
block variant 150.3 against 148.1. The four-partition build is the default
because copies up to four need it; a serving deployment with at most two
copies per expert gets the 2026-11-01 step back from the two-partition
build. The two builds' checksums differ in the ninth digit (the partition
boundaries set the chunk sequence) and each build's plain and copy arms
agree with each other.

## Measured (2026-09-06, one A40 node, four PEs, `+gpupool`)

Correctness: `out2` and `w2` are bit-identical across no-LB, sync and async
at every checkpoint, at the small size and at the defaults, with SGD and
Adam, top-1 and top-2, CUPTI-measured and estimated loads, pool and non-pool
transport, and 1 or 4 compute lanes. Migrations happened in every LB run.

Step time at the defaults (64 experts of 2048 x 8192, 8192 tokens per PE),
no LB, as the runtime and the app were fixed during the performance work:

| state                                                        | ms/step |
|--------------------------------------------------------------|--------:|
| first version (one stream per PE)                            |     275 |
| runtime: cross-process receives no longer host-synchronized  |     335 |
| app: comm stream + 4 compute lanes                           |     239 |
| app: sends tagged with dedicated data-ready streams          |     206 |
| app: gather on the send stream                               |     198 |

The max-PE GPU time is 153 ms throughout, so the step is now 77% compute.
What was found, in order (runtime changes are in `src/`, see git):

- **Every cross-process device receive was host-synchronized.** The receive
  path's "same process, different device" case compared node-local device
  indices, which differ for every peer process too; with one device per
  process every direct IPC receive waited on the host for the sender's stream
  and then drained its own. Measured as ~3 ms per 1 MB receive, an exchange
  at 0.5 GB/s on 24 GB/s links, and no overlap of transfers with compute.
  Fixed in `ckrdmadevice.C`; pic2d's no-LB run went from 1.65 s to 0.72 s
  with the same checksum.
- **A send is tagged with its stream's tail at send time.** With the
  host waits gone, a landing copy carries a device-side wait on the sender's
  stream. Anything queued on the sender's stream after the data was produced
  (another expert's GEMMs on a shared lane, landings waiting on other PEs on
  a shared comm stream) delays the receiver's stream and everything behind
  it. Hence three stream roles per PE: `comm` for every landing and the
  combine, compute lanes for the experts, and one send stream per sender
  that waits only on the event marking its own data ready.
- **Runtime hygiene found on the way:** plain DEVICE-mode pups did not note
  pool sources as read (a pool free could race the pack copy); the unpack
  copied on the legacy default stream behind every other payload's landing;
  the polled event queue was one FIFO across streams, so a landing recorded
  after a 150 ms compute-done event was not noticed until the kernel ended.
  All three are fixed; none was the bottleneck here.

What is left, from the trace: token exchange 33 ms, output exchange 10 ms,
combine and drain 7 ms per step. Consumption of the 48 remote slabs a PE
receives is paced at 0.3 to 0.5 ms per slab whatever their size (26 MB and
96 MB take the same 30 ms), and the profile is flat across the scheduler,
network progress, pxshm and event queries: per-message host work in the
device zerocopy path, roughly 0.35 ms all-in on the receiver. Fewer, larger
messages (a slab per destination PE instead of per expert) would remove most
of it at the cost of location transparency; a cheaper per-message path in
the runtime would keep it.

Load balancing, final build (DiffusionLB, `-f 5 -b 5`, async `-l 3`), after
the source fusion and the load-model fix below:

| config                      | no LB | sync | async |
|-----------------------------|------:|-----:|------:|
| defaults, 30 steps, ms/step |   191 |  199 |   185 |
| 4096 x 14336, 20 steps      |   560 |  545 |   530 |

Before those two changes the same runs were 205 / 216 / 207 and 631 / 623 /
605. What changed:

- **One chunked pass per expert, not one per source** (the default; `-U`
  restores the old way). A chunk runs two weight-update GEMMs that read and
  rewrite both weight matrices whatever the chunk holds, so a step's cost is
  set by the number of chunks, and per source that is at least one per
  (source, expert) pair: a typical expert receives a few hundred tokens from
  each dispatcher and paid a full weight update for each. At the defaults that
  is 260 chunked passes per step against 78, and 30 ms per GPU of weight
  traffic. The expert's tokens are copied into one run first and the outputs
  copied back, about 0.8 ms per GPU, so the sends and both pup phases are
  unchanged. Concatenating in dispatcher order keeps the chunk boundaries a
  function of the routing alone: the checksums differ from the unfused ones in
  value and are still identical across placements. Compute span 154 -> 131 ms.
- **Seconds per token is pooled, not per expert** (per PE when this was
  written; job-wide since 2026-10-31, below). An expert timed
  itself with events around its own kernels, but up to `-n` experts share the
  device, so the span was inflated by however much company it had -- and a hot
  expert, outliving its lane-mates, had less of it. Per-expert figures
  compressed exactly the skew the balancer has to see. Every expert here does
  identical work per token, so one pooled figure is both the right model and an
  accurate one. `CHARM_MOE_FLATLOAD=1` declares an exact token-proportional
  load instead, which separates a balancer that cannot use a good signal from
  a bad signal.
- **The checksum's copy is waited for before a pack reads it.** `h_chk` is
  filled by an asynchronous device-to-host copy, and `pup_device_order` orders
  only DEVICE-mode copies, so a migration in that window packed whatever was
  there before. It cost one expert's slot in the `w2` reduction, seen once in
  eight async runs at Llama size as a deficit of exactly 1/64. The outputs
  were never affected: `out2` matched in the failing run.

GreedyRefineCentralGPULB, a centralized greedy re-placement of the kind the
reference uses, was tried on the same runs. It reaches the same balance as
DiffusionLB and loses anyway, because it moves four to five times as many
experts:

| balancer, defaults           | ms/step | experts moved | tokens/PE |
|------------------------------|--------:|--------------:|-----------|
| no LB                        |     191 |             0 | 1.67-1.84 |
| DiffusionLB, sync            | 199-201 |         19-23 | 1.07-1.25 |
| DiffusionLB, async           | 189-205 |         18-27 | 1.17-1.31 |
| GreedyRefineCentralGPULB sync| 281-298 |        95-112 | 1.07-1.25 |
| GreedyRefineCentralGPULB async|    212 |         53-55 | 1.13-1.35 |

`+LBPercentMovesAllowed 25` brings its async case to 209 ms, still behind both.
At 134 MB of weights per expert the migration traffic costs more than the
balance buys, which is the same conclusion the move-cap experiment reached from
the other direction. Two runtime bugs had to be fixed before this could be
measured at all; see the git log for `+gpupool`'s null comm buffer and the
memory contract's staging figure.

What the balancer still leaves on the table: between reshuffles DiffusionLB
holds `tokens/PE max/avg` at 1.20 to 1.33, where a greedy re-placement reaches
1.01 (see `ref/`). The per-step move cap is a count applied to objects of very
unequal load, so it can stop a shed part-way -- a node asked to shed 0.187 of
its load stopped after four objects with 0.102 unshed and the job sat at 1.9x
for five steps. Lifting the cap (`+LBDiffusionMaxMoveFrac 1`) does recover in
one step and holds 1.2 to 1.33, but moves 35 to 54 objects where the capped
runs move 6 to 9, and the step time does not improve. At this object size the
migration traffic pays for the balance it buys; a cap expressed in the load
dimension rather than by object count is the open item.

Balancing cuts tokens per PE from 1.9x to 1.3x and the GPU-time imbalance
from 1.5x to 1.15x. In the Llama-sized run the balanced steps take 476 to 497
ms against 575 to 601 ms without balancing, until the hot set reshuffles and
the next LB step catches up; the averages above include the reshuffle steps.
Async now overlaps everything: its first LB step costs nothing visible (228
ms against 529 ms in sync at the defaults).

Tracing: `CHARM_MOE_TRACE=1` prints timestamped LB, migration and step
events per PE, `=2` adds each expert's compute and send events, `=3` each
slab's post and consume. `ranklogs/phases.py <dir>` decomposes a level-2
trace into routing, token exchange, compute, output exchange and drain;
`ranklogs/tracesum.py` summarizes migration costs; `ranklogs/pairs.py`
per-pair latencies from a level-3 trace. `CHARM_MOE_HOSTSYNC=1` restores the
original host-synchronized pack and free for A/B runs. With more than one
lane the per-expert device time is a wall-clock span on a shared device, so
`gpu/PE max` overcounts; its ratio across PEs is still meaningful.

Running a single PE: a one-task `srun` step owns only eight of the
allocation's cores, so the NUMA wrapper's `+pemap` aborts the run
(`CmiSetCPUAffinity failed`) with an empty log. Launch single-PE tests
without the wrapper.

## Reference implementation

`ref/moe_ref.py` is the same layer in PyTorch with NCCL, written the way an
MoE training framework writes it: one `all_to_all_single` per exchange instead
of one message per expert, one chunked pass per expert instead of one per
source, and expert re-placement between steps instead of object migration. Its
routing, initialization and update are ports of this example's, checked bit for
bit by `ref/selftest.py`, so the two produce the same checksums and the same
imbalance and can be compared step for step on the same node.

On four A40s at the defaults the example takes 204 ms per step without
balancing against the reference's 189, and 195 ms balanced against the
reference's 153. The gap is not the migration mechanism: the example's async
balancing beats its own sync, while the reference's does not, because
overlapping NCCL weight transfers costs a step of stale placement and shares
the links with the dispatch. The gap is the placement decision, where greedy
re-placement reaches a token imbalance of 1.01 against DiffusionLB's 1.28, and
the GEMM structure, where one pass per expert is worth 30% of the compute
time. `ref/README.md` has the tables and the derivations.

`ref/megatron_moe.py` runs the same layer through Megatron-Core's MoE layer
(expert parallelism, all-to-all dispatcher, optional grouped GEMM and bf16)
with the example's routing in place of its top-k, as a throughput reference
for a production framework; `ref/run_mg.sh` launches it on one A40 node. Its
numbers are in the 2026-10-31 section above.

## Several PEs per process

`MOE_PPN=N` through `ranklogs/moewrap.sh` (or `+ppn N +pemap <cores>` by
hand) runs N PEs per process on one GPU. The program is the same at any N:
one dispatcher per process, on its first PE, owns the process's `-t` tokens,
routes them and ships one slab per expert exactly as with one PE, so the
work per GPU, the slab count and the routing do not change with N and the
checksums match the one-PE run bit for bit. What N changes is where the
experts run: they spread over all the process's PEs, each with its own
compute context (the lane count defaults to four per GPU divided among the
PEs, `-n` overrides), so their kernel launches, landing copies and output
sends run on N PEs instead of one, and the balancer moves them between the
PEs of a process on host time as well as across processes on device time.
The other PEs' dispatcher branches hold no tokens and run no steps; they
exist for that context.

Measured 12 Sep 2026, one A40 node, 8 cores per process, ms/step:

    PEs per process          1        2        4        6        7        8
    no LB, defaults      188-193    193   180-182    183      181    202-205
    async LB, defaults   215-248     -    216-230     -        -     216-230
    no LB, Llama size      557       -      530       -        -       576
    async LB, Llama        659       -      650       -        -       721

Four PEs per process is the sweet spot, 6% faster than one with no
balancer and 5% at the Llama size. The step's phase trace
(`CHARM_MOE_TRACE=2`, `ranklogs/phases.py`) shows where: route+gather
23.8 -> 10.9 ms, compute span 131 -> 128, combine 14 -> 18, output
exchange 17 -> 12.5. At eight PEs the output exchange -- the time from
the last expert's output sends to the dispatcher having every output
landed -- triples to 46-53 ms and eats the gains; the dispatcher PE consumes
landed outputs in bursts tens of milliseconds apart instead of as they
arrive. Same-process and remote outputs are delayed alike, so it is not the
intra-process transport, and `MOE_ONE_DEVICE=1` (which cuts the driver's
per-GPU threads from four to one) leaves it unchanged, so it is not thread
oversubscription of the eight cores either. Open. Use four until it is
understood.

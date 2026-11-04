# Starting on Frontier: the branch, the AMD build, and the first jobs

Written 2026-11-02, when development of `rate-aware-gpu-lb` moved from Delta's A40 nodes to Frontier's
MI250X. `HIP_PORT.md` is the port itself and the Frontier work of 2026-10-22 (its last section: the build
corrections, the driver findings, the profiler patch, the channel-transport fixes, the balancer figures).
This file is what has been added since, what it needs on Frontier, and what to run first. It replaces the
handover of 2026-10-21.

## What the branch holds now

Commits of 2026-11-02 on `rate-aware-gpu-lb`, made in the Delta clone `/u/bhosale/charm-reconverse`
(pushing them needs a credential from the user; none is stored anywhere):

| Commit | What |
|---|---|
| bfcdc743e | replication as a balancing decision: `setLBReplica`, the clone and retire decision kinds, `ckClone`/`ckRetire` and the notices to the other copies, ShedLB's group pass with the critical-path rule and the declared exchange cost, the memory contract's clone accounting, `LBManager::LastRoundDecisions`; `tests/charm++/load_balancing/replica_test`; `doc/internals/lb-multiplicity.md`; the manual |
| 5488fde06 | hapi: a vmm chunk of exactly 2048 MB is refused off-node (the registration ceiling is 2^31 bytes) |
| 07abb5815 | moe: experts as balancer-made copies (up to four), bf16, serving, real-routing replay, the transformer block, checksums every n steps, ShedLB linked by the Makefile; the FastMoE and Megatron-Core harnesses; the kernel bound found on 2026-11-02 and its fix |
| a4ab7a902 | this file |
| 778631db3, d7a4ad9d4 | the routing traces as a GitHub release; the FastMoE patch and build script |
| 7f3a67e46 | moe/ref: NCCL on Slingshot through the aws-ofi-nccl plugin, and the transport logged (correction 1 below) |
| a2c2a1b23 | moe: serving ran two of four partitions (correction 2); the matrix re-measured; `-DMOE_K_PART` |
| 3bfecb852 | moe/ref: Transformer Engine grouped experts, FasterMoE serving without the per-step collection, fp32 masters (corrections 3 to 5); not pushed at the time of writing |
| uncommitted | moe: cuDNN fused attention for the block variant, with a startup self-check (item 6); committed once job I below has validated it |

Everything in them was measured on A40s only, one and two nodes (the tables in
`examples/charm++/cuda/gpudirect/moe/README.md`). On AMD nothing has run since 2026-10-22, when leanmd,
jacobi3d, chantest and sph3d ran on Frontier and moe only built. The replication code tests `CMK_GPU` and
never `CMK_CUDA`, the MoE example calls only `hapi*` names, its bf16 values are raw 16-bit words with
hand-written conversions, and `hapi_blas.h` has both vendors' GEMMs, so the port is expected to be a build,
not a rewrite.

## Corrections since 2026-11-02, and the mistakes behind them (written 2026-11-03)

Read this before trusting any number in the README or in memory. Between 2026-11-02 and 11-03 the user
asked what a fair comparison must match, and the audit that followed found that several published
numbers were artefacts of how I had run the baselines or the example, every one of them in the example's
favour. The pattern was the same each time: a baseline came out slow, and I wrote the number down without
checking that the baseline had been run the way its authors run it. The README's sections "Measured
2026-11-02" and "Measured 2026-11-03" hold the corrected tables; the earlier sections carry "superseded"
notes. Dated, with the cause and what now prevents it:

1. **The NCCL baselines ran without the Slingshot plugin** (every two-node baseline before 2026-11-02).
   `NCCL_DEBUG=INFO` showed `NET/IB : No device found`: socket transport across the nodes. The baselines
   gained 11 to 48% on the plugin and every published ratio shrank. `ref/refwrap.sh` now sets what
   `module load aws-ofi-nccl` sets and logs NCCL's INIT and NET lines, so a rank log without `Using
   network Libfabric` is known to have run without it. On Frontier the equivalent is RCCL with the
   `aws-ofi-rccl` plugin: make the harness log the transport there too, and read the line before the
   number.
2. **The example served two of its four partitions** (from the four-partition change of 2026-11-01
   17:20 until a2c2a1b23): the serving branch still looped over partitions 0 and 1, half the
   dispatchers got unwritten outputs, the serving step looked 30% slower and the serving checksums of the
   "Copies up to four" paragraph were wrong. Training was right. Now every partition is served and every
   plain-against-copy serving checksum pair agrees. `-DMOE_K_PART=2` builds the two-partition variant.
3. **The Megatron-Core arm never ran grouped GEMM.** Megatron-Core 0.20's local layer spec builds
   `SequentialMLP` whatever `moe_grouped_gemm` says (there is no `GroupedMLP` in 0.20); the grouped path
   is the Transformer Engine spec, Megatron-LM's default. The harness printed "grouped GEMM" from its flag.
   `ref/megatron_moe.py --te` runs TE's grouped experts (7 to 10% faster on SmartMoE and Mixtral, nothing
   on OLMoE) and the header line prints the expert module that ran. On ROCm, Transformer Engine is a
   separate fork; without it the Megatron arm is the sequential one, and the row must say so.
4. **FasterMoE's serving step paid a full garbage collection every step** (every FasterMoE serving row
   before 2026-11-03): my harness collected the autograd graphs FastMoE's fused forward records, and the
   collection walked every long-lived object, about 55 ms a step. `gc.freeze()` before the loop. With
   it FasterMoE serves SmartMoE's routing on four GPUs in 27.0 ms/step, equal to the example with ShedLB
   and ahead of the plain example: its shadow of the hottest expert does in serving what ShedLB's copy
   does. The published "3.6x over FasterMoE in serving" is void; job G (below) re-measures eight GPUs.
5. **The baseline harnesses updated bf16 parameters in place**, less work than Megatron-LM's bf16
   optimizer, which keeps fp32 master weights as the example does. `--master` on both harnesses; job G
   runs every training arm with it and one arm without, for its cost.
6. **The example's attention was unfused** (score GEMM, softmax kernel, PV GEMM, and their backward)
   while the references' attention is PyTorch's fused flash kernel. On SmartMoE's routing the example's
   attention cost twice the references' (21.6 against 10.4 ms a step in training on eight GPUs). The
   fused path is cuDNN's attention graphs (`MOE_CUDNN_ATTN`, the Makefile; bf16 only, the fp32 path and
   the HIP build stay unfused), with a startup self-check that runs one training step on both paths and
   aborts past a relative L2 error of 5e-2. On Frontier there is no cuDNN: the block rows there carry the
   mismatch until a fused HIP attention exists (composable kernels' FMHA is the candidate), and the
   "Block:" header line names the kernels so a row cannot hide it.
7. **Process mistakes**, each now a rule: I submitted one job to two partitions to race the queue (never;
   one job in one partition, and the partition the user names is the standing choice: on Delta that is
   now `gpuA40x4-interactive` by sbatch, after a batch job sat 13 hours); I rebuilt the `moe` binary while
   a job was running it (an arm died with rc=127; build variants under another name or after the job);
   and I passed an unparsed flag to a script and lost a job's first 20 seconds (dry-run the arms' argument
   lines against the scripts' parsers before submitting).

What I check before a baseline number enters a table, from now on: the log names the module and the
transport that ran, never the flag; a serving step is not slower than the same system's training
forward; a system's enhanced variant does not lose to its plain one without a written explanation. The
harness options that make the baselines what their authors run: Megatron `--te --grouped --master`,
FastMoE training `--master`, FasterMoE serving with the freeze (automatic).

**Pending at the time of writing.** Job G (23497275, two A40 nodes): the Megatron arms with TE experts and
masters, FastMoE and FasterMoE training with masters, FasterMoE serving fixed, on the three routings and
the two block variants. Job I (to follow, two nodes): the example's block rows with the fused attention
against the unfused build, plain and ShedLB, training and serving. Their tables go into a "Measured
2026-11-04" section of the README; the headline ratios change again, and the Frontier ladder (below)
should take its A40 reference rows from that section, not from 2026-11-01.

## What Frontier changes for the experiments

| | Delta, gpuA40x4 | Frontier |
|---|---|---|
| GPUs per node | 4 A40 | 4 MI250X = 8 GCDs of 64 GB; one GCD per process (`+gpudevs 1`) |
| the eight-GPU matrix | 2 nodes | 1 node; 2 nodes = 16 GCDs (two nodes stay the cap) |
| pool across nodes | vmm, `+gpupoolsize 16384 +gpupoolchunk 2046` | buddy only (ROCr cannot dma-buf-export `hipMemCreate` memory): `+gpupoolalloc buddy +gpupoolsize 2048 +gpupoolarenas 4`, `CK_GPU_ARENA_TOTAL_MAX_MB=16384` |
| pool on one node | vmm | vmm works (the IPC import keeps the fd open until the grant) |
| a one-node `srun` step | plain | `--network=single_node_vni`, else cxi finds no VNI and LCI aborts |
| transfer-cost table | built in (A40) | `+LBCostConfig lbcost.frontier.conf` (written by `lbcalib`, ported in `apps/lbcalib`) |
| per-rank wrapper | `ranklogs/moewrap.sh` (NUMA node 3 - local rank, 16 cores each) | `frontier_wrap.sh` (a GCD to its L3 region, the region's first core left to the OS); adapt `moewrap.sh` to that map |
| LCI devices | one per process | one per PE when a process has several (`+lci_ndevices 7` for leanmd); moe runs one PE per process |
| completion flags | memset writer | kernel writer (the HIP default; `+gpuflagmemset` restores the event-and-memset one) |
| profiler | CUPTI | rocprofiler-sdk 1.3.2 with the pool-release patch (`ext/rocprofiler_patched`, loaded ahead of ROCm's by `irun.sh`); the legacy intercept queue is preset by the runtime |
| MR cache monitor | `FI_MR_CACHE_MONITOR=uffd` | `kdreg2` or `uffd` (within noise of each other) |
| application streams | one per chare | `+gpustreams 32` (the HIP default) |
| allocation | `sbatch -A mzu-delta-gpu -p gpuA40x4` | `sbatch -A csc710 -p batch` |

The ledger question in the README ("no bin can hold one" for a third copy of a default-shape expert on a
16 GB pool) meets 64 GB GCDs here. The ledger prints what each node advertises at the first balancing
step; a buddy pool advertises arena by arena (one 2048 MB arena advertised 0 MB on 2026-10-22, below the
reserve; two advertised about 1.4 GB).

## Build

Paths on Frontier as of 2026-10-22: `/lustre/orion/csc710/scratch/adityapb/hip` with `env.sh` (the
modules; do not load `craype-accel-amd-gfx90a`), the HIP LCI at `lci_install_hip`, the charm clone,
`irun.sh` and `wrun.sh` (a run, and a run with a stall catcher that dumps every process's stacks),
`frontier_wrap.sh`, `ext/` (the patched profiler, the crash catcher `segvbt.so`), `vmmtest/` (the driver
repros).

1. **Sources.** Charm: fetch `rate-aware-gpu-lb` and fast-forward to the commits above. Reconverse: the
   branch was rebased on 2026-10-31 (`038893f`, forced over the `76fe574` Frontier has), so in the
   reconverse clone `git fetch && git reset --hard origin/rate-aware-gpu-lb`. The recharm merge of
   2026-10-31 (`9327afa2b`) replaced the build system: reconverse is the only network,
   `cmake/fetch_reconverse` is gone, `--with-reconverse-dir` names the clone, and the HIP option is spelled
   `amd` (or `hip`) with a `-amd` tree suffix. The 2026-10-22 tree, `multicore-linux-x86_64-hip`, predates
   all of that: build a new one.
2. **Environment.** `source env.sh`; `export CC=cc CXX=CC FC=ftn` (else the system GCC 7.5 builds it);
   `$ROCM_PATH/lib/llvm/bin` on `PATH` (charmc calls bare `clang`).
3. **LCI.** The HIP install of 2026-10-22 (UIUC-PPL/lci master 662b4736, ofi, no CUDA) still serves;
   `ldd $LCI_HIP/lib64/liblci.so` must show no CUDA library.
4. **Runtime.** The Delta configuration with the AMD option and the HIP LCI:

       CC=cc CXX=CC ./buildcmake charm++ reconverse-linux-x86_64 amd -j16 \
         --with-reconverse-dir=$PWD/reconverse \
         --with-cmake-args="-DLCI_ROOT=$LCI_HIP -DLCI_DIR=$LCI_HIP/lib64/cmake \
           -DRECONVERSE_ENABLE_HIP=ON -DCMAKE_HIP_ARCHITECTURES=gfx90a \
           -DCMK_USE_SHMEM=ON -DLBUSERDATA=1 -DCMAKE_BUILD_TYPE=Release"

   Expect the tree `reconverse-linux-x86_64-amd`. This line has not been run since the merge. The
   2026-10-22 corrections to watch for are in `HIP_PORT.md`: without `-DRECONVERSE_ENABLE_HIP=ON`
   reconverse builds with `CMK_HIP 0`; autofetched non-HIP `liblci*.so` in the tree's `lib/` shadow the HIP
   LCI at run time (move them aside); clang rejected `void*` arithmetic in `buddy_allocator.cpp` (fixed
   then). After any `./buildcmake` the root symlinks `bin include lib` point at the last tree built. The LB
   modules come with the tree (`EveryLB` holds ShedLB; `DiffusionLB` is its own module). Headers of the LB
   database changed in bfcdc743e (`lbdb.h`, `LBManager.h`, `BaseLB.h`, `LBMemoryContract.h`,
   `ckmigratable.h`, `cklocrec.h`); after any later header edit rebuild the tree, a single-target `make`
   leaves ABI skew.
5. **Applications.** As on 2026-10-22, as copies in the tree: rsync `examples/charm++/cuda/gpudirect/moe/`
   to `<tree>/apps/moe/` with `hapi_blas.h` and `hapi_kokkos.h` beside it, remove the A40 binaries (`moe`,
   `moe.*`) and objects, `make CHARM_DIR=<tree> moe`. The Makefile reads `BUILD_HIP` and `HIP_ARCHS` from
   the tree's `conv-mach-opt.mak`, compiles `moe.cu` with `hipcc -x hip` and links `-lhipblas`. New since
   the last AMD build, and checked only by compiling: `hipblasGemmEx` and `hipblasGemmStridedBatchedEx`
   with `HIP_R_16BF` operands and `HIPBLAS_COMPUTE_32F`. Also `tests/charm++/load_balancing/replica_test`
   (`make CHARM_DIR=<tree>`): host-only, but run inside an allocation like everything else (`+p4`).
6. **Traces.** The three routings are 3.3 GB of `.trace` files, ignored by git, in Delta's
   `/u/bhosale/charm-reconverse/examples/charm++/cuda/gpudirect/moe/traces/` (72 layer files and
   `mixtral_tokens.npy`), and published, zstd-packed to 473 MB, as release `traces-2026-11-01` of
   <https://github.com/adityapb/charm-moe-traces> (private; a token with repo scope downloads it). On
   Frontier: `GH_TOKEN=<token> ./fetch_traces.sh apps/moe/traces` with the script from that repository
   (it checks the sums), then `zstd -dc <set>_traces.tar.zst | tar -xf - -C apps/moe/traces` for each of
   the three sets; its README has the format and the generating commands. The README's rows use
   `smartmoe_L02`, `mixtral_L12` and `olmoe_L12`. Regenerating them instead needs the checkpoints (Mixtral-8x7B 87 GB and OLMoE-1B-7B
   13 GB, in Delta's `/scratch/mzu/bhosale/hf`) and SmartMoE-AE's `moe_trace` tables (`smartmoe_src` in
   the rescued scratchpads below); `traces/*_stats.txt` are the per-layer skews that chose the layers.
7. **Baselines**, for the matrix only. `ref/moe_ref.py` (the PyTorch reference, the example's twin) runs
   on ROCm PyTorch with RCCL; `ref/megatron_moe.py` needs Megatron-Core (its ROCm fork); FastMoE's CUDA
   extension has no ROCm build, so `ref/fastmoe_moe.py` and FasterMoE are a port (hipify) or stay A40
   figures.

## Run

A job is one `sbatch` script that runs every arm and ends; nothing is queued without work attached; one
runner per allocation; `squeue -u $USER` before anything else; `ulimit -c 0`; at most one hour and two nodes.

    #!/bin/bash
    #SBATCH -A csc710 -p batch -N 1 -t 01:00:00 -J moe-ladder
    #SBATCH --ntasks-per-node=8 --gpus-per-node=8 --cpus-per-task=8
    #SBATCH -o /lustre/orion/csc710/scratch/adityapb/hip/runs/%x-%j.out
    source /lustre/orion/csc710/scratch/adityapb/hip/env.sh; ulimit -c 0
    export LD_LIBRARY_PATH=$TREE/lib:$LCI_HIP/lib64:$ROCM_PATH/lib:$LD_LIBRARY_PATH
    export LD_PRELOAD=$LCI_HIP/lib64/liblci.so    # irun.sh adds the patched profiler and the crash catcher
    export FI_MR_CACHE_MONITOR=kdreg2 CHARM_LB_HELPER=near PMI_MAX_KVS_ENTRIES=8192
    run_app() { tag=$1; shift; RL=$RUNS/ranklogs/$tag; rm -rf $RL; mkdir -p $RL
      RANKLOG_DIR=$RL timeout 300 srun -N 1 -n 8 --ntasks-per-node=8 --gpus-per-node=8 \
        --cpu-bind=none --exact --kill-on-bad-exit=1 --unbuffered --network=single_node_vni \
        $APP/ranklogs/moewrap.sh stdbuf -oL $APP/moe "$@" +gpudevs 1 \
        +gpushm +gpupool +gpupoolsize 16384 +gpuipceventpool 256 +gpupoolalloc vmm +gpupoolchunk 2046 \
        > $RL/srun.log 2>&1; }

Two nodes take `-N 2 -n 16` without `--network=single_node_vni`, and the buddy pool (`+gpupoolalloc buddy
+gpupoolsize 2048 +gpupoolarenas 4`, `CK_GPU_ARENA_TOTAL_MAX_MB=16384`) in place of the vmm flags. After
any kill remove `/dev/shm/charm-chan-*` and `/dev/shm/charm-hapi-host*` on the node. The map
`frontier_wrap.sh` pins by, from the 2026-10-21 handover: GCD 0 cores 48-55, 1: 56-63, 2: 16-23, 3: 24-31,
4: 0-7, 5: 8-15, 6: 32-39, 7: 40-47, the first core of each region to the OS in low-noise mode; confirm it
with `rocm-smi --showtopo` before trusting it (an unpinned run was 18-60x slower on Delta).

The three routings and their shapes (`$SM`, `$MIX`, `$OL` below):

| Routing | Model arguments | Trace |
|---|---|---|
| SmartMoE's training routing, layer 2 (hottest expert 48% of the tokens) | `-e 32 -m 2048 -h 8192 -k 1 -t 8192` | `traces/smartmoe_L02.trace` |
| Mixtral-8x7B on WikiText-103, layer 12 (15.9%) | `-e 8 -m 4096 -h 14336 -k 2 -t 8192` | `traces/mixtral_L12.trace` |
| OLMoE-1B-7B on WikiText-103, layer 12 (5.7%) | `-e 64 -m 2048 -h 1024 -k 8 -t 8192` | `traces/olmoe_L12.trace` |

Common: `-i 40 -u 3 -B`, steady state steps 23-43; plain `-R 1 -f 99999`; ShedLB `-R 2 -f 20 -b 20 -a`;
serving `-G`; the transformer block `-w`; checksums `-C 4`. The per-rank log `rank_0.log` carries the
`Step n:` lines with `out2` and `w2`; `ShedLB: replicate` and `ShedLB: retire` lines name the decisions.

## The first job: the validation ladder

*Done 2026-11-02 (job 5833510, interactive): Parts 0-2 pass after three fixes (the runtime's
imported-pointer copies through the batch API on HIP, a new copy's landing buffers made before its state,
the serving branch over all four partitions); the rows are in the README's "Measured 2026-11-02:
Frontier" section. Open: one of seven `-R 4` runs of the fixed app still aborted at a new copy's first step
("a block sum from copy 1 for step 24 but this copy has 1 copies", logs/moe/sm_R4 of job 5833510); the six others,
five of them traced (`CHARM_MOE_TRACE=1`), passed bit-identical to plain. Part 3 waits on the baselines. The scripts: `moe_run.sh`, `moe_diag.sh`,
`ladder_int.sh <1|2>` under the scratch root, `JID=` the interactive allocation.*

*Done 2026-11-02, later (job 5837505, two nodes, frontier02930+03188, one hour): the three-way comparison
on 16 GCDs, every shape, training and serving, plus the `-w` rows: the README's "Measured 2026-11-02, later:
Frontier, two nodes" section. Scripts: `two_node2.sh` (the chain that ran: arms, checksum pairs, `scancel` at
the end; `two_node.sh` is its first draft), `n2_table.sh` (the README rows from `logs/moe/two_node.txt` and
`logs/ref/results_2n.txt`), `node_clean.sh` (leftover ranks and `/dev/shm` segments after a kill),
`moe_run.sh`/`ref_run.sh` take `NODES=`. The baselines' RCCL across nodes needs OLCF's aws-ofi-nccl plugin
(`/sw/frontier/rccl-plugins/aws-ofi-nccl/1.19.2/rocm/7.1.1/lib`, the `rccl-net-plugin` module's variables), set by
`ref_wrap.sh` when the step spans nodes; the test is `SLURM_STEP_NUM_NODES`, because `SLURM_JOB_NUM_NODES` is
unset in a step of a `--no-shell` allocation (the first three baseline runs went over TCP sockets for that and are
kept under `_socket` tags). A two-hour two-node debug request had a next-morning estimate; cut to one hour it
started in 35 minutes. Open from the run: (1) the reference's EPLB arm hangs in RCCL's channel setup before its
fourth step on 16 ranks (twice, both transports; `logs/ref/n2_sm_ref_eplb`); (2) three of four Mixtral ShedLB arms
segfault on PE 14 at exit, a device-channel receive completing inside the LCI backend's exit drain whose handler
sends (`logs/moe/n2_mix_train_R2/rank_14.log`; steps and checksums complete); (3) OLMoE's exchange crosses the
link at one NIC's bandwidth where RCCL's plugin uses four: whether LCI's endpoints spread over a node's NICs;
(4) ShedLB's one-copy-per-round rule cannot use an empty node when eight bins are loaded alike (Mixtral, 8
experts on 16 GCDs).*

*Pulled 2026-11-04 (five commits, d7a4ad9d4..e6f9b8aa9, the corrections section above; the nested reconverse is
unchanged at 038893f; the local changes were stashed, fast-forwarded and reapplied, two conflicts resolved by hand:
the serving loop of `moe.C` is upstream's, the README's Frontier sections sit before the Delta "Measured
2026-11-02" section). The merged tree's HIP `moe` builds (16 s; the attention unfused, which the "Block:" line now
names). What the corrections mean for the Frontier rows: (1) the two-node baselines already ran on the RCCL plugin
and the one-node ones cross no link; `ref_wrap.sh` now logs the transport by default (`NCCL_DEBUG=INFO`,
`INIT,NET`); (2) every Frontier serving row came from the fixed binary (built 14:54 on 11-02, "fixed app" in
`logs/moe/ladder_serve.txt`; the two-node job ran it), so none is invalid; (3) the Megatron rows say "sequential
experts" in the log header and in the tables, and Transformer Engine does not reach MI250X: AMD's prebuilt
`transformer_engine_rocm` 2.1.0 (repo.radeon.com, rocm-rel-7.0) carries gfx942 and gfx950 code only, a source
build of ROCm/TransformerEngine would need `NVTE_ROCM_ARCH=gfx90a` against the venv's torch, and its
single-launch grouped-GEMM backends are gfx942/gfx950 only, so TE's grouped experts on MI250X would be one
hipBLASLt GEMM per expert in any case; (4) no FasterMoE here; (5) the seven Megatron training rows (one node:
SmartMoE, SmartMoE cap 1.25, Mixtral, OLMoE; two nodes: SmartMoE, SmartMoE cap 1.25, OLMoE) ran without fp32
masters and were re-measured with `--master` on job 5847934 (`mg_master.sh`, tags `mgm_*`, `n2_mgm_*`; masters
cost 0 to 5 ms a step: SmartMoE 76.9 against 73.8 on one node, 142.0 against 137.4 on two); the serving rows do
not update weights and stand; (6) the HIP
build's attention is unfused and no baseline block row exists on Frontier, so nothing measured is affected, but a
block comparison here waits for a fused HIP attention (open item 9). The README's one-node section now compares
against the corrected A40 rows, and the two-node section's OLMoE paragraph no longer calls the Delta order
"the opposite": with the plugin the baselines are ahead on OLMoE there too.*

*2026-11-04, the fused attention on Frontier (correction 6 above, open item 9). The HIP build has it now:
aotriton, the library PyTorch's `scaled_dot_product_attention` dispatches to on ROCm, ships inside the torch
2.13+rocm7.1 wheel (`torch/lib/libaotriton_v2.so.0.12.0`, the kernel images under
`torch/lib/aotriton.images/amd-gfx90a`, the headers under `torch/include/aotriton`; the library exports the v3
API only, the params structs). `$W/aotriton/{lib,include}` holds copies, because the library's `NEEDED
libamdhip64.so` with `RPATH $ORIGIN` would, used in place, load the wheel's HIP runtime beside the runtime's
/opt/rocm-7.14.1 one; staged without it, the name resolves through the library path to
`/opt/rocm-7.14.1/lib/libamdhip64.so`, the same file as the already loaded `libamdhip64.so.7` (ldd shows one).
`moe.cu`: `MOE_AOTRITON_ATTN`, the same strided views of the packed qkv and dqkv arrays as the cuDNN path, the
causal forward with its persistent atomic counter zeroed per call, the backward with eager delta rows and an
fp32 dQ accumulator (zeroed per call), dropout off, as PyTorch 2.13's `mha_all_aot.hip` calls it. The Makefile's
HIP branch sets it when `$(AOTRITON_DIR)/include/aotriton/flash.h` exists (`AOTRITON_DIR`, default
`$W/aotriton`; `MOE_AOTRITON_ATTN=0` builds the unfused kernels). The startup self-check (one training step,
fused against unfused, abort past a relative L2 error of 5e-2) runs here as on CUDA, and the "Block:" line
names the kernels. Binaries: `apps/moe/moe` (fused), `apps/moe/moe.attn_unfused` (the merged tree's unfused
build, kept under its own name); `moe_run.sh` takes `BIN=`. The chain `fused_attn.sh` (the block rows plain
and ShedLB, training and serving, one node and two, the unfused build's plain arms on the same nodes, then
`mg_master.sh` with its `scancel`): job 5847934 (frontier04265 and 04266, 01:03 to 01:29), results
`logs/moe/fused_attn.txt`, the README section "Measured 2026-11-04: Frontier, the block rows with the fused
attention". Outcome: aotriton 0.12 (built for ROCm 7.1) runs under the 7.14.1 runtime; the self-check error is
the A40 value (3.6e-3 on the output, 1.1e-3 to 2.1e-3 on the weight gradients, both head sizes); the
attention's cost in training halves on one node (SmartMoE 13.3 to 6.4 ms, Mixtral 40.1 to 29.5), the serving
step on two nodes is exchange-bound either way; the Mixtral ShedLB two-node arms still take the exit segfault
(open item 2, PE 10 this time). The baselines' block rows then ran on the same allocation (`block_ref.sh`,
`--attn`, tags `smb_ref_*`, `mixb_ref_*`, `mgm_smb_*`, `mgm_mixb_*`, `n2_*`; every two-node log says "Using
network Libfabric", every Megatron header "SequentialMLP (local spec)" with the masters line), so the block
comparison exists on Frontier now, the same fixture on both sides: the README section has the tables.
Transformer Engine does not reach MI250X (the pull note above), so the Megatron rows stay the sequential ones.*

This is the job cancelled unrun on Delta (`/scratch/mzu/bhosale/moe_runs/v4b_fix.sbatch`), the one the
kernel-bound fix of 2026-11-02 still owes (the README's "A kernel bound, found and removed").

- **Part 0.** `replica_test +p4 +balancer ShedLB +LBDebug 1` (the flags its Makefile runs it with; without the
  balancer it aborts "expected the balancer to replicate worker 0"). It prints no PASS line: it passes when it
  exits 0 after `worker 0 cloned at step 10` and `retire decision at step 40`, and aborts otherwise.
- **Part 1, the kill criterion** (the last step's `out2 w2` checksums must be one set per shape):

  | Tag | Arguments |
  |---|---|
  | sm_plain | `$SM -i 40 -u 3 -B -C 4 -E $TS -R 1 -f 99999` (the reference) |
  | sm_X | `... -R 2 -f 99999 -X` |
  | sm_R4 | `... -R 4 -f 10 -b 10 -a` |
  | sm_R3 | `... -R 3 -f 10 -b 10 -a` |
  | mix_plain | `$MIX -i 40 -u 3 -B -C 4 -E $TM -R 1 -f 99999` (the reference) |
  | mix_X | `... -R 2 -f 99999 -X` |

  A difference stops the job: on the A40s the pre-fix binary differed from step 24, the first step with
  three copies, in copy 0 alone.
- **Part 2, the rows measured before the fix.** For SmartMoE and Mixtral: `-w` in training and serving,
  plain and ShedLB `-R 2`; for all three routings: the MoE layer alone in training and serving, plain and
  ShedLB `-R 2`; for SmartMoE also ShedLB `-R 4`. About 27 arms, 15 minutes on the A40s. These become the
  first rows of a "Measured on Frontier" section of the README, beside the A40 rows they re-measure.
- **Part 3.** The matrix (`mat8.sh` in the rescued scratchpads: example plain and ShedLB, the reference
  static, EPLB-style packing and one copy, Megatron-Core with and without a capacity factor, FastMoE and
  FasterMoE, training and serving, bands of three) once the baselines run on ROCm.

## Open questions, in order

1. The ladder above: checksums at three and four copies on eight GCDs, and the `-w` rows.
2. The memory ledger refusing the third copy of a default-shape expert on a 16 GB pool: what it advertises
   per node and why, now against 64 GB GCDs.
3. The cost model has no per-message latency: on eight A40s SmartMoE with `-R 4` ran slower than with
   `-R 2` (the copies' exchanges dominate at 8 MB weights), and the start-up probe sends one 128 MB partial;
   a small-message probe and a latency term beside the bytes term in the declared cost.
4. The idle-round path (`LastRoundDecisions == 0`, the dispatchers keep the lookahead through the round)
   has never been exercised: every OLMoE round made two or three decisions. `+LBShedEps 1` on OLMoE makes
   the threshold twice the mean, so no bin sheds and the round decides nothing; the step time should then
   equal the plain arm's.
5. The Kokkos variant against the four-partition tree: `moe.kokkos` has no add kernel and may still be the
   two-partition scheme; its `-X` and `-R 3` checksums against `moe`'s say so.
6. Copies in serving: the declared exchange cost is zero when serving, so ShedLB should copy the Mixtral
   expert in serving (a 10% gain on the A40s before the critical-path rule); verify the copy is made.
7. Platform items from `HIP_PORT.md`: the vmm pool cannot be NIC-registered on ROCr (dma-buf export),
   `hapi_memory_daemon` is CUDA-only, `tests/util/streamorder` and `benchmarks/charm++/cuda/gpudirect/*`
   spell CUDA names, the rocprofiler pool-release patch belongs upstream, the channel transport's remaining
   hold under the balancer.
8. FasterMoE's bf16 numerics (NaN from step 3 to 8; its harness, documented in `ref/README.md`), A40-only
   until FastMoE builds on ROCm.
9. Done 2026-11-04: the fused attention for the HIP build is aotriton (the paragraph under the ladder), and
   `ref_wrap.sh` logs the RCCL transport by default (correction 1). Left: the fp32 path is unfused on both
   vendors by design.

## Delta artefacts and where they are

- `/scratch/mzu/bhosale/moe_runs/`: the run scripts and console logs of 2026-11-01 and 11-02
  (`v4b_fix.sbatch`, `v2a.sh`, `cmp_dump.py`, `mat8_table.py`), and `scratchpad_dt-login01/` and
  `scratchpad_dt-login04/` (166 MB each, copied on 2026-11-02 from the node-local session scratchpads:
  `mat8.sh`, `bands_table.py`, `fm8*.sh`, `alloc_*.sh`, `perf*.sh`, `trace1.sh`, `build_fastmoe.sh`,
  `fastmoe_src/`, `smartmoe_src/`). Delta's scratch is purged; copy what is needed to Frontier.
- `examples/charm++/cuda/gpudirect/moe/ranklogs/` in the Delta clone: the per-rank logs of every arm in
  the README; `traces/` beside it (above, and on GitHub as a release).
- The checkpoint cache `/scratch/mzu/bhosale/hf` (100 GB; ungated downloads, so re-downloadable).
- The agent's memory, `/u/bhosale/.claude/projects/-u-bhosale-charm-reconverse/memory/` (facts, rules,
  the measured history). Claude Code keys it by the clone's path, so on Frontier copy it to
  `~/.claude/projects/<clone path with / written as ->/memory/`, and link the practitioner notes as the
  skill (`AGENTS.md`).

## Working rules that carry over

- Batch only, at most one hour and two nodes; the sbatch script runs the work and the job ends with it;
  nothing pending at the end of a turn; `squeue -u $USER` first; one runner per allocation; never release
  an allocation with open work.
- Read the run's own log in the call that launches it; steady state only (steps 23-43); results in
  tables, arm by metric, in the README and in chat; a repeat needs a stated purpose (three for a band).
- A hang is a bug to root-cause (stacks; `wrun.sh`'s catcher), never run past; a mitigation is not a fix.
- Every run: `CHARM_LB_HELPER=near`, `+gpushm +gpupool`, `ulimit -c 0`; no new environment variables for
  one-off experiments; no balancer flags (the runtime self-configures), `+LBShedEps` only for item 4.
- One job in one partition, the partition the user named; never rebuild a binary a running job uses; a
  baseline row is written only after its log names what ran (module, transport, attention kernels) and
  its serving step is no slower than its training forward (the section above).

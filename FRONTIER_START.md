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
| the one after | this file |

Everything in them was measured on A40s only, one and two nodes (the tables in
`examples/charm++/cuda/gpudirect/moe/README.md`). On AMD nothing has run since 2026-10-22, when leanmd,
jacobi3d, chantest and sph3d ran on Frontier and moe only built. The replication code tests `CMK_GPU` and
never `CMK_CUDA`, the MoE example calls only `hapi*` names, its bf16 values are raw 16-bit words with
hand-written conversions, and `hapi_blas.h` has both vendors' GEMMs, so the port is expected to be a build,
not a rewrite.

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
   `mixtral_tokens.npy`). Copy the three the README uses (`smartmoe_L02`, `mixtral_L12`, `olmoe_L12`, 22 to
   25 MB each) or all of them, by Globus (the Delta and OLCF DTN endpoints) or scp through the DTNs, into
   `apps/moe/traces/`. Regenerating them instead needs the checkpoints (Mixtral-8x7B 87 GB and OLMoE-1B-7B
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

This is the job cancelled unrun on Delta (`/scratch/mzu/bhosale/moe_runs/v4b_fix.sbatch`), the one the
kernel-bound fix of 2026-11-02 still owes (the README's "A kernel bound, found and removed").

- **Part 0.** `replica_test +p4`: expect its PASS lines.
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

## Delta artefacts and where they are

- `/scratch/mzu/bhosale/moe_runs/`: the run scripts and console logs of 2026-11-01 and 11-02
  (`v4b_fix.sbatch`, `v2a.sh`, `cmp_dump.py`, `mat8_table.py`), and `scratchpad_dt-login01/` and
  `scratchpad_dt-login04/` (166 MB each, copied on 2026-11-02 from the node-local session scratchpads:
  `mat8.sh`, `bands_table.py`, `fm8*.sh`, `alloc_*.sh`, `perf*.sh`, `trace1.sh`, `build_fastmoe.sh`,
  `fastmoe_src/`, `smartmoe_src/`). Delta's scratch is purged; copy what is needed to Frontier.
- `examples/charm++/cuda/gpudirect/moe/ranklogs/` in the Delta clone: the per-rank logs of every arm in
  the README; `traces/` beside it (above).
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

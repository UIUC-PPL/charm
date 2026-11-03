#!/bin/bash
# Launch wrapper for moe_ref.py under srun: rank r on GPU r, pinned to the
# cores of NUMA node (3-r), which hosts GPU r on gpuA40x4 (PCI bus order).
# One log per rank in $RANKLOG_DIR.
R=${SLURM_PROCID:-0}; L=${SLURM_LOCALID:-$R}; NUMA=$((3 - L)); FIRST=$((NUMA * 16))
export CUDA_DEVICE_ORDER=PCI_BUS_ID CUDA_VISIBLE_DEVICES=$L
export RANK=$R WORLD_SIZE=${SLURM_NTASKS:-1} LOCAL_RANK=$L
export MASTER_ADDR=${MASTER_ADDR:-$(scontrol show hostnames "$SLURM_JOB_NODELIST" 2>/dev/null | head -1)} MASTER_PORT=${MASTER_PORT:-29517}
[ -z "$MASTER_ADDR" ] && export MASTER_ADDR=$(hostname)
export NCCL_P2P_LEVEL=${NCCL_P2P_LEVEL:-SYS} NCCL_DEBUG=${NCCL_DEBUG:-INFO} NCCL_DEBUG_SUBSYS=${NCCL_DEBUG_SUBSYS:-INIT,NET} OMP_NUM_THREADS=8
# Across nodes NCCL reaches Slingshot only through the aws-ofi-nccl plugin (what `module load
# aws-ofi-nccl/1.19.2` sets: the plugin, libfabric 2.3.1, the cxi provider); without it NCCL falls back to
# its own transports. INIT,NET at INFO writes the transport chosen ("NET/OFI", "Using network") into every
# rank log, so a run that lacks the line is known to have run without it.
if [ -z "${NCCL_NET_PLUGIN:-}" ]; then
  OFI=/sw/rh9.4/user/aws-ofi-nccl-1.19.2-lf2.3.1-cu13.2/lib
  [ -f $OFI/libnccl-net-ofi.so ] && export NCCL_NET_PLUGIN=ofi FI_PROVIDER=cxi FI_CXI_DISABLE_HOST_REGISTER=1 \
    FI_MR_CACHE_MONITOR=userfaultfd LD_LIBRARY_PATH=$OFI:/opt/cray/libfabric/2.3.1/lib64:${LD_LIBRARY_PATH:-}
  # FI_MR_CACHE_MONITOR: libfabric's default monitor (memhooks) deadlocks with CUDA (one of eight ranks hung in
  # NCCL's tree connection, job 23452305); the module sets userfaultfd, as the example's own runners do.
fi
MB=""; numactl --membind=$NUMA true 2>/dev/null && MB="numactl --membind=$NUMA"
exec $MB taskset -c $FIRST-$((FIRST + 7)) "$@" > "$RANKLOG_DIR/rank_${R}.log" 2>&1

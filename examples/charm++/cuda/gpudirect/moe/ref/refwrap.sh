#!/bin/bash
# Launch wrapper for moe_ref.py under srun: rank r on GPU r, pinned to the
# cores of NUMA node (3-r), which hosts GPU r on gpuA40x4 (PCI bus order).
# One log per rank in $RANKLOG_DIR.
R=${SLURM_PROCID:-0}; L=${SLURM_LOCALID:-$R}; NUMA=$((3 - L)); FIRST=$((NUMA * 16))
export CUDA_DEVICE_ORDER=PCI_BUS_ID CUDA_VISIBLE_DEVICES=$L
export RANK=$R WORLD_SIZE=${SLURM_NTASKS:-1} LOCAL_RANK=$L
export MASTER_ADDR=${MASTER_ADDR:-$(scontrol show hostnames "$SLURM_JOB_NODELIST" 2>/dev/null | head -1)} MASTER_PORT=${MASTER_PORT:-29517}
[ -z "$MASTER_ADDR" ] && export MASTER_ADDR=$(hostname)
export NCCL_P2P_LEVEL=${NCCL_P2P_LEVEL:-SYS} NCCL_DEBUG=${NCCL_DEBUG:-WARN} OMP_NUM_THREADS=8
MB=""; numactl --membind=$NUMA true 2>/dev/null && MB="numactl --membind=$NUMA"
exec $MB taskset -c $FIRST-$((FIRST + 7)) "$@" > "$RANKLOG_DIR/rank_${R}.log" 2>&1

#!/bin/bash
# run_mg.sh <jobid> <tag> [megatron_moe.py arguments...]
# One A40 node, four ranks, one per GPU; logs in ranklogs/mg_<tag>/; prints the rank-0 summary.
# The conda environment's own libstdc++ and NCCL are put first on the library path: the login
# node's are too old for Transformer Engine, which megatron.core imports.
J=$1; TAG=$2; shift 2
P=/u/bhosale/charm-reconverse/examples/charm++/cuda/gpudirect/moe; R=$P/ref; cd $P || exit 1; ulimit -c 0
ENV=${ENV:-/sw/rh9.4/user/python/conda-env/pytorch-2.12.1-cu130}
export LD_LIBRARY_PATH=$ENV/lib:$ENV/lib/python3.12/site-packages/nvidia/nccl/lib:$ENV/lib/python3.12/site-packages/nvidia/cudnn/lib:${LD_LIBRARY_PATH:-}
export RANKLOG_DIR=$P/ranklogs/mg_$TAG; mkdir -p $RANKLOG_DIR; rm -f $RANKLOG_DIR/*
NODES=${NODES:-1}; NT=$((NODES * 4))
timeout ${TMO:-600} srun --jobid=$J --mpi=cray_shasta -N $NODES -n $NT --ntasks-per-node=4 --cpus-per-task=8 --cpu-bind=none --exact --unbuffered $R/refwrap.sh $ENV/bin/python $R/megatron_moe.py "$@"; rc=$?
sleep 2; echo "rc=$rc  logs: $RANKLOG_DIR"
grep -h "^\[Megatron\|^Experts\|^Tokens\|^Routing\|^Steps\|^Step \|^Total\|^Average\|loss is not" $RANKLOG_DIR/rank_0.log
if [ $rc != 0 ]; then for f in $RANKLOG_DIR/rank_*.log; do echo "== $f"; grep -n "Error\|error\|Traceback" $f | head -5; tail -12 $f | cut -c1-200; done; fi

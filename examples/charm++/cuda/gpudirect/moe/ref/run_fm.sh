#!/bin/bash
# run_fm.sh <jobid> <tag> <plain|sched|faster> [fastmoe_moe.py arguments...]
#   plain   FastMoE as shipped: two all-to-alls around the experts, static placement
#   sched   FasterMoE's smart schedule (group-wise exchange overlapped with the experts)
#   faster  smart schedule + expert shadowing: the PPoPP'22 FasterMoE system
# One A40 node, four ranks, one per GPU (NODES=2 for eight); logs in ranklogs/fm_<tag>/; prints the rank-0
# summary. FastMoE is built into ~/.local for the same conda environment the Megatron harness uses.
# The shadow policy's machine parameters come from ranklogs/fm_calib.txt (written by `run_fm.sh <jobid>
# calib calib -m .. -H .. -t ..`); without that file FastMoE's defaults (6.25 GB/s, 11.5 TFLOP/s) apply.
J=$1; TAG=$2; MODE=$3; shift 3
P=/u/bhosale/charm-reconverse/examples/charm++/cuda/gpudirect/moe; R=$P/ref; cd $P || exit 1; ulimit -c 0
ENV=${ENV:-/sw/rh9.4/user/python/conda-env/pytorch-2.12.1-cu130}
export LD_LIBRARY_PATH=$ENV/lib:$ENV/lib/python3.12/site-packages/nvidia/nccl/lib:$ENV/lib/python3.12/site-packages/nvidia/cudnn/lib:${LD_LIBRARY_PATH:-}
export RANKLOG_DIR=$P/ranklogs/fm_$TAG; mkdir -p $RANKLOG_DIR; rm -f $RANKLOG_DIR/*
NODES=${NODES:-1}; NT=$((NODES * 4))
SRUN="timeout ${TMO:-600} srun --jobid=$J --mpi=cray_shasta -N $NODES -n $NT --ntasks-per-node=4 --cpus-per-task=8 --cpu-bind=none --exact --unbuffered $R/refwrap.sh $ENV/bin/python"
if [ "$MODE" = calib ]; then
  $SRUN $R/fm_calib.py "$@"; rc=$?
  grep -h "^#\|^NETBW\|^GPUTP" $RANKLOG_DIR/rank_0.log | tee $P/ranklogs/fm_calib.txt
  echo "rc=$rc"; exit $rc
fi
[ -n "${GROUP:-}" ] && export FMOE_FASTER_GROUP_SIZE=$GROUP
case $MODE in
  plain)  ;;
  sched)  export FMOE_FASTER_SCHEDULE_ENABLE=1 ;;
  faster) export FMOE_FASTER_SCHEDULE_ENABLE=1 FMOE_FASTER_SHADOW_ENABLE=1 ;;
  *) echo "mode must be plain, sched, faster or calib"; exit 2 ;;
esac
if [ "$MODE" = faster ] && [ -f $P/ranklogs/fm_calib.txt ]; then
  export FMOE_FASTER_GLBPLC_NETBW=$(sed -n 's/^NETBW=//p' $P/ranklogs/fm_calib.txt)
  if printf '%s\n' "$@" | grep -qx -- "--bf16"; then
    export FMOE_FASTER_GLBPLC_GPUTP=$(sed -n 's/^GPUTP_BF16=//p' $P/ranklogs/fm_calib.txt)
  else
    export FMOE_FASTER_GLBPLC_GPUTP=$(sed -n 's/^GPUTP_FP32=//p' $P/ranklogs/fm_calib.txt)
  fi
fi
$SRUN $R/fastmoe_moe.py "$@"; rc=$?
sleep 2; echo "rc=$rc  logs: $RANKLOG_DIR"
grep -h "^\[FastMoE\|^Experts\|^Tokens\|^Routing\|^Steps\|^Policy\|^Step \|^Shadow\|^Loss\|^Total\|^Average\|loss is not" $RANKLOG_DIR/rank_0.log
if [ $rc != 0 ]; then for f in $RANKLOG_DIR/rank_*.log; do echo "== $f"; grep -n "Error\|error\|Traceback" $f | head -5; tail -12 $f | cut -c1-200; done; fi

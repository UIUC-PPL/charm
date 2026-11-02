#!/bin/bash
# Builds FastMoE (github.com/laekov/fastmoe, master 55af4f9 of 2025-02-10) with fastmoe-55af4f9.patch
# into the user site of Delta's pytorch-2.12.1-cu130 environment, for run_fm.sh. The compile is CPU-only,
# so it runs on a login node; ninja (in the environment) compiles the units in parallel. Plain gcc/g++
# rather than the Cray wrapper the shell exports, and an explicit -L for the environment's libnccl.so.2
# through the patch's NCCL_LIBDIR: the conda linker ignores LIBRARY_PATH and the environment ships no
# libnccl.so link, so a stub directory holds one.
# Usage: ./build_fastmoe.sh <work dir>     (the clone and the stub are made under it; ENV overrides the environment)
set -u
W=${1:?work dir}
ENV=${ENV:-/sw/rh9.4/user/python/conda-env/pytorch-2.12.1-cu130}
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$W/ncclstub" && cd "$W" || exit 1
[ -d fastmoe ] || git clone -q https://github.com/laekov/fastmoe fastmoe || exit 1
cd fastmoe && git checkout -q 55af4f9 || exit 1
if git apply --check "$HERE/fastmoe-55af4f9.patch" 2>/dev/null; then git apply "$HERE/fastmoe-55af4f9.patch" && echo "patch applied"
else echo "patch does not apply cleanly (already applied?): $(git diff --stat | tail -1)"; fi
ln -sf "$ENV/lib/python3.12/site-packages/nvidia/nccl/lib/libnccl.so.2" "$W/ncclstub/libnccl.so"
export CC=gcc CXX=g++ CUDA_HOME=/usr/local/cuda-13
export CPATH=$ENV/lib/python3.12/site-packages/nvidia/nccl/include
export NCCL_LIBDIR=$W/ncclstub LDFLAGS="-L$W/ncclstub"
export TORCH_CUDA_ARCH_LIST="8.6" MAX_JOBS=16 USE_NCCL=1
export PATH=$ENV/bin:$PATH
echo "=== build_ext $(date +%T)"
$ENV/bin/python setup.py build_ext > "$W/build_ext.log" 2>&1; rc=$?; echo "build_ext rc=$rc $(date +%T)"; tail -3 "$W/build_ext.log" | cut -c1-300
[ $rc = 0 ] || exit $rc
$ENV/bin/python -m pip install --user --no-build-isolation --no-deps . > "$W/pip.log" 2>&1; rc=$?; echo "pip rc=$rc $(date +%T)"; tail -2 "$W/pip.log" | cut -c1-200
cd /; $ENV/bin/python -c "import fmoe, fmoe_cuda; from fmoe.fastermoe import schedule; print('fastmoe import ok:', fmoe.__file__)"

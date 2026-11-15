#!/usr/bin/env bash
# Push igdeg lower (128 -> 96 -> 64) to find the true lossless floor, plus
# retry 128-dim niter=30 now that igdeg=128 is active (the combination may be
# lossless even though niter=30 alone was slightly lossy at igdeg=256).
set -u
cd /workspace/work
export LD_LIBRARY_PATH=/workspace/work:/seekdb_workspace/bridge:/opt/hipvs/lib:/opt/rocm/lib

run() {  # run <dim> <niter> <igdeg>
  local dim=$1 niter=$2 igdeg=$3
  CAGRA_ALGO=2 CAGRA_NITER="$niter" CAGRA_IGDEG="$igdeg" ./bridge_test ./libalgo.so 250000 "$dim" 100 100 2>/dev/null \
    | grep -oE "BUILD_MS=[0-9.]+ .*RECALL10=[0-9.]+ RECALLK=[0-9.]+" \
    | sed "s/^/dim=$dim niter=$niter igdeg=$igdeg /"
}

echo "=== 256-dim (niter=70), igdeg 128/96/64 ==="
export BASE_F32=/workspace/work/d256/base.f32 QUERY_F32=/workspace/work/d256/query.f32 GT_I32=/workspace/work/d256/gt.i32 GT_Q=100 GT_K=100
run 256 70 128
run 256 70 96
run 256 70 64

echo "=== 128-dim (niter=40), igdeg 128/96/64 ==="
export BASE_F32=/workspace/work/d128/base.f32 QUERY_F32=/workspace/work/d128/query.f32 GT_I32=/workspace/work/d128/gt.i32 GT_Q=100 GT_K=100
run 128 40 128
run 128 40 96
run 128 40 64

echo "=== 128-dim niter=30 igdeg=128 (retry with igdeg cut) ==="
run 128 30 128
run 128 30 128
run 128 30 128
echo DONE

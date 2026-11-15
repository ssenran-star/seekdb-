#!/usr/bin/env bash
# igdeg=128 is the lossless floor (96/64 collapse recall). But with igdeg=128,
# niter can go MORE aggressive than before. Probe:
#   256-dim: niter 60 / 50  (was 70 lossless at igdeg=128)
#   128-dim: niter 25 / 20  (niter=30 was already lossless at igdeg=128)
set -u
cd /workspace/work
export LD_LIBRARY_PATH=/workspace/work:/seekdb_workspace/bridge:/opt/hipvs/lib:/opt/rocm/lib

run() {  # run <dim> <niter> <igdeg>
  local dim=$1 niter=$2 igdeg=$3
  CAGRA_ALGO=2 CAGRA_NITER="$niter" CAGRA_IGDEG="$igdeg" ./bridge_test ./libalgo.so 250000 "$dim" 100 100 2>/dev/null \
    | grep -oE "BUILD_MS=[0-9.]+ .*RECALL10=[0-9.]+ RECALLK=[0-9.]+" \
    | sed "s/^/dim=$dim niter=$niter igdeg=$igdeg /"
}

echo "=== 256-dim igdeg=128, niter 60 x3 ==="
export BASE_F32=/workspace/work/d256/base.f32 QUERY_F32=/workspace/work/d256/query.f32 GT_I32=/workspace/work/d256/gt.i32 GT_Q=100 GT_K=100
run 256 60 128; run 256 60 128; run 256 60 128

echo "=== 256-dim igdeg=128, niter 50 x3 ==="
run 256 50 128; run 256 50 128; run 256 50 128

echo "=== 128-dim igdeg=128, niter 25 x3 ==="
export BASE_F32=/workspace/work/d128/base.f32 QUERY_F32=/workspace/work/d128/query.f32 GT_I32=/workspace/work/d128/gt.i32 GT_Q=100 GT_K=100
run 128 25 128; run 128 25 128; run 128 25 128

echo "=== 128-dim igdeg=128, niter 20 x3 ==="
run 128 20 128; run 128 20 128; run 128 20 128
echo DONE

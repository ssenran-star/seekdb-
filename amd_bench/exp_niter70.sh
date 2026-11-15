#!/usr/bin/env bash
# Locate the lossless niter boundary precisely: niter=70 and 75 are unmeasured
# and sit between lossless 80 and dangerous 60. Repeated runs to beat the noise.
set -u
cd /workspace/work
export LD_LIBRARY_PATH=/workspace/work:/seekdb_workspace/bridge:/opt/hipvs/lib:/opt/rocm/lib
export BASE_F32=/workspace/work/d256/base.f32
export QUERY_F32=/workspace/work/d256/query.f32
export GT_I32=/workspace/work/d256/gt.i32
export GT_Q=100
export GT_K=100

run() {
  CAGRA_ALGO=2 CAGRA_NITER="$1" ./bridge_test ./libalgo.so 250000 256 100 100 2>/dev/null \
    | grep -oE "BUILD_MS=[0-9.]+ .*RECALL10=[0-9.]+ RECALLK=[0-9.]+" \
    | sed "s/^/niter=$1 /"
}

echo "=== niter=70 x5 ==="
run 70; run 70; run 70; run 70; run 70
echo "=== niter=75 x3 ==="
run 75; run 75; run 75
echo "=== niter=80 x5 (re-confirm lossless) ==="
run 80; run 80; run 80; run 80; run 80
echo "DONE"

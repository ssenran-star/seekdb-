#!/usr/bin/env bash
# Stability check for the aggressive niter candidates (50/55/60).
# The single-shot scan showed niter=50 RECALLK drop ~0.003 vs baseline; we need
# repeated runs to confirm the drop is stable and stays <= 0.005.
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

echo "=== niter=50 x5 (stability) ==="
run 50; run 50; run 50; run 50; run 50
echo "=== niter=55 x3 ==="
run 55; run 55; run 55
echo "=== niter=60 x3 ==="
run 60; run 60; run 60
echo "DONE"

#!/usr/bin/env bash
# Find the "lossless" nn_descent_niter boundary on 256-dim data.
#
# The frozen baseline uses nn_descent_niter=100. If the graph quality is
# already converged at some lower niter (recall identical to niter=100, drop=0),
# then that lower niter is a *lossless* speedup -- it does NOT "trade recall
# for performance" and stays within the contest rules. We scan niter downward
# to locate the point where recall actually starts to drop.
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

echo "=== baseline niter=100 x3 (noise reference) ==="
run 100; run 100; run 100
echo "=== candidates (scan downward) ==="
run 80; run 60; run 50; run 40; run 30
echo "DONE"

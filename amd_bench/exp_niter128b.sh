#!/usr/bin/env bash
# 128-dim aggressive niter boundary: niter=40 was lossless (RECALLK == baseline).
# Probe 30/25/20 to find how far we can push SIFT (128-dim) without recall drop.
set -u
cd /workspace/work
export LD_LIBRARY_PATH=/workspace/work:/seekdb_workspace/bridge:/opt/hipvs/lib:/opt/rocm/lib
export BASE_F32=/workspace/work/d128/base.f32
export QUERY_F32=/workspace/work/d128/query.f32
export GT_I32=/workspace/work/d128/gt.i32
export GT_Q=100
export GT_K=100

run() {
  CAGRA_ALGO=2 CAGRA_NITER="$1" ./bridge_test ./libalgo.so 250000 128 100 100 2>/dev/null \
    | grep -oE "BUILD_MS=[0-9.]+ .*RECALL10=[0-9.]+ RECALLK=[0-9.]+" \
    | sed "s/^/niter=$1 /"
}

echo "=== niter=40 x3 (re-confirm) ==="
run 40; run 40; run 40
echo "=== niter=30 x3 ==="
run 30; run 30; run 30
echo "=== niter=25 x3 ==="
run 25; run 25; run 25
echo "=== niter=20 x3 ==="
run 20; run 20; run 20
echo DONE

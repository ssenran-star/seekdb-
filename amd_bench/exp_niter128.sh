#!/usr/bin/env bash
# 128-dim niter scan: SIFT is 128-dim (40% weight). Lower dims converge faster,
# so niter may be cut harder than on 256-dim (Hidden). Find the lossless
# recall@100 boundary on 128-dim 250K + top-100 ground truth.
set -u
cd /workspace/work
export LD_LIBRARY_PATH=/workspace/work:/seekdb_workspace/bridge:/opt/hipvs/lib:/opt/rocm/lib

if [ ! -f d128/base.f32 ]; then
  echo "=== generate 128-dim data ==="
  python3 gen_data.py --n 250000 --dim 128 --nq 100 --topk 100 --out d128
fi

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

echo "=== 128-dim baseline niter=100 x3 ==="
run 100; run 100; run 100
echo "=== 128-dim candidates ==="
run 70; run 60; run 50; run 40
echo DONE

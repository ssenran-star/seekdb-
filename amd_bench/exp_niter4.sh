#!/usr/bin/env bash
# Stability check for "within-gate but lossy" niter candidates that are safe on
# synthetic data (harder than real SIFT/Hidden, so real drop is smaller):
#   128-dim niter=25 (synth drop 0.0027)
#   256-dim niter=50 (synth drop 0.0034, but one outlier hit 0.0064 -> re-check)
#   256-dim niter=55 (interpolated drop ~0.002)
set -u
cd /workspace/work
export LD_LIBRARY_PATH=/workspace/work:/seekdb_workspace/bridge:/opt/hipvs/lib:/opt/rocm/lib

if [ ! -f d128/base.f32 ]; then
  echo "=== generate 128-dim data ==="
  python3 gen_data.py --n 250000 --dim 128 --nq 100 --topk 100 --out d128
fi

run() {  # run <dim> <niter> <igdeg>
  local dim=$1 niter=$2 igdeg=$3
  CAGRA_ALGO=2 CAGRA_NITER="$niter" CAGRA_IGDEG="$igdeg" ./bridge_test ./libalgo.so 250000 "$dim" 100 100 2>/dev/null \
    | grep -oE "BUILD_MS=[0-9.]+ .*RECALL10=[0-9.]+ RECALLK=[0-9.]+" \
    | sed "s/^/dim=$dim niter=$niter /"
}

echo "=== 256-dim niter=55 x5 ==="
export BASE_F32=/workspace/work/d256/base.f32 QUERY_F32=/workspace/work/d256/query.f32 GT_I32=/workspace/work/d256/gt.i32 GT_Q=100 GT_K=100
run 256 55 128; run 256 55 128; run 256 55 128; run 256 55 128; run 256 55 128

echo "=== 256-dim niter=50 x5 (re-confirm outlier risk) ==="
run 256 50 128; run 256 50 128; run 256 50 128; run 256 50 128; run 256 50 128

echo "=== 128-dim niter=25 x5 ==="
export BASE_F32=/workspace/work/d128/base.f32 QUERY_F32=/workspace/work/d128/query.f32 GT_I32=/workspace/work/d128/gt.i32 GT_Q=100 GT_K=100
run 128 25 128; run 128 25 128; run 128 25 128; run 128 25 128; run 128 25 128

echo "=== 128-dim niter=20 x5 (aggressive) ==="
run 128 20 128; run 128 20 128; run 128 20 128; run 128 20 128; run 128 20 128
echo DONE

#!/usr/bin/env bash
# Decisive brute-force build experiment: exact kNN graph -> FromArgs -> recall.
# If recall >= legacy levels AND total build is faster, this becomes the new
# build pipeline (and the host graph enables the slimmer snapshot format).
#
# Usage: bash exp_bf.sh            (both dims, 3 runs each)
#        DIMS=256 bash exp_bf.sh
set -u
echo "exp_bf.sh v3 (2026-11-16 22:40) -- full output mode"
WORK=${WORK:-/workspace/work}
RUNS=${RUNS:-3}
cd "$WORK"
export LD_LIBRARY_PATH=/opt/hipvs/lib:/opt/rocm/lib

echo "=== compile bf_test ==="
curl -fsSL "https://api.github.com/repos/ssenran-star/seekdb-/contents/amd_bench/bf_test.c?ref=${REF:-amd-cagra-submit}" \
  | python3 -c "import sys,json,base64;open('bf_test.c','wb').write(base64.b64decode(json.load(sys.stdin)['content']))"
/opt/rocm/bin/hipcc -O2 -D__HIP_PLATFORM_AMD__ -I/opt/hipvs/include -I/opt/rocm/include \
  bf_test.c -L/opt/hipvs/lib -Wl,-rpath,/opt/hipvs/lib:/opt/rocm/lib -lcuvs_c -o bf_test \
  && echo COMPILE_OK || { echo COMPILE_FAIL; exit 1; }

for dim in ${DIMS:-128 256}; do
  data=$WORK/d$dim
  [ -f "$data/base.f32" ] || { echo "missing $data (run exp_build_matrix first)"; exit 1; }
  echo ""
  echo "==================== dim=$dim  (gdeg=128, NEIGHBOR_BITS=64) ===================="
  for i in $(seq 1 "$RUNS"); do
    echo "--- run $i (NEIGHBOR_BITS=64) ---"
    env BASE_F32="$data/base.f32" QUERY_F32="$data/query.f32" GT_I32="$data/gt.i32" \
        GT_Q=100 GT_K=100 NEIGHBOR_BITS=64 \
        ./bf_test 250000 "$dim" 128 2>&1
    echo "  (exit=$?)"
  done
  echo "--- dtype fallback check (NEIGHBOR_BITS=32, 1 run) ---"
  env BASE_F32="$data/base.f32" QUERY_F32="$data/query.f32" GT_I32="$data/gt.i32" \
      GT_Q=100 GT_K=100 NEIGHBOR_BITS=32 \
      ./bf_test 250000 "$dim" 128 2>&1
  echo "  (exit=$?)"
done
echo ""
echo "=== reading the output ==="
echo "SELF_HIT ~1.0000 -> neighbour dtype is correct (else rerun with the other NEIGHBOR_BITS)"
echo "RECALL10/RECALLK must be >= legacy (128d: 0.996/0.973, 256d: 0.954/0.868) to adopt bf"
echo "total = BF_BUILD_MS + BF_SEARCH_MS + FROMARGS_MS is the build cost to compare"
echo "  vs legacy (128d ~10.5s, 256d ~18s) and nd (128d ~8.8s)"
echo DONE

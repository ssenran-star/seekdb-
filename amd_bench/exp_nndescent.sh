#!/usr/bin/env bash
# nn_descent + FromArgs experiment: compare against cuvsCagraBuild baseline.
set -u
cd /workspace/work
export LD_LIBRARY_PATH=/workspace/work:/seekdb_workspace/bridge:/opt/hipvs/lib:/opt/rocm/lib

echo "=== compile nn_descent_test ==="
/opt/rocm/bin/hipcc -O2 -D__HIP_PLATFORM_AMD__ -I/opt/hipvs/include -I/opt/rocm/include \
  nn_descent_test.c -L/opt/hipvs/lib -Wl,-rpath,/opt/hipvs/lib:/opt/rocm/lib -lcuvs_c -o nn_descent_test \
  && echo COMPILE_OK

export BASE_F32=/workspace/work/d256/base.f32 QUERY_F32=/workspace/work/d256/query.f32 \
       GT_I32=/workspace/work/d256/gt.i32 GT_Q=100 GT_K=100

echo ""
echo "=== baseline: cuvsCagraBuild niter=60 igdeg=128 (expect RECALLK ~0.871) ==="
CAGRA_ALGO=2 CAGRA_NITER=60 CAGRA_IGDEG=128 ./bridge_test ./libalgo.so 250000 256 100 100 2>/dev/null \
  | grep -oE "BUILD_MS=[0-9.]+ .*RECALLK=[0-9.]+"

echo ""
echo "=== nn_descent + FromArgs: max_iterations sweep (term_th=0.0001) ==="
for mi in 20 30 40 60; do
  ./nn_descent_test 250000 256 "$mi" 0.0001 2>&1 | grep -E "MAX_ITER=|FAIL"
done

echo ""
echo "=== nn_descent + FromArgs: termination_threshold sweep (max_iter=100) ==="
for tt in 0.0001 0.001 0.01; do
  ./nn_descent_test 250000 256 100 "$tt" 2>&1 | grep -E "MAX_ITER=|FAIL"
done
echo DONE

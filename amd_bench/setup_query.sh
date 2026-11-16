#!/usr/bin/env bash
# Self-heal after instance reset: rebuild work dir, pull files, compile,
# regenerate 256-dim data, then run the itopk/sw sweep.
set -u
# repo is public: fetch anonymously (embedded PAT expired)
mkdir -p /workspace/work
cd /workspace/work

echo "=== 1. pull files ==="
for f in bridge_test.c seekdb_cuvs_bridge_algo.c query_bench.c gen_data.py; do
  curl -fsSL \
    "https://api.github.com/repos/ssenran-star/seekdb-/contents/amd_bench/$f?ref=amd-cagra-submit" \
    | python3 -c "import sys,json,base64;open('$f','wb').write(base64.b64decode(json.load(sys.stdin)['content']))"
done
ls -la *.c *.py

echo "=== 2. compile ==="
gcc -O2 -o bridge_test bridge_test.c -ldl && echo GCC_OK
gcc -O2 -o query_bench query_bench.c -ldl && echo QGCC_OK
/opt/rocm/bin/hipcc -shared -fPIC -O2 -D__HIP_PLATFORM_AMD__ -I/opt/hipvs/include -I/opt/rocm/include \
  seekdb_cuvs_bridge_algo.c -L/opt/hipvs/lib -Wl,-rpath,/opt/hipvs/lib:/opt/rocm/lib -lcuvs_c -o libalgo.so \
  && echo HIPCC_OK

echo "=== 3. data (256-dim) ==="
if [ ! -f d256/base.f32 ]; then
  python3 gen_data.py --n 250000 --dim 256 --nq 100 --topk 100 --out d256
fi

echo "=== 4. recall vs search params (niter=60 igdeg=128 fixed) ==="
export LD_LIBRARY_PATH=/workspace/work:/seekdb_workspace/bridge:/opt/hipvs/lib:/opt/rocm/lib
export BASE_F32=/workspace/work/d256/base.f32 QUERY_F32=/workspace/work/d256/query.f32 \
       GT_I32=/workspace/work/d256/gt.i32 GT_Q=100 GT_K=100
for itopk in 512 256 128; do
  for sw in 4 2 1; do
    CAGRA_ALGO=2 CAGRA_NITER=60 CAGRA_IGDEG=128 CAGRA_ITOPK="$itopk" CAGRA_SW="$sw" \
      ./bridge_test ./libalgo.so 250000 256 100 100 2>/dev/null \
      | grep -oE "SEARCH_MS=[0-9.]+ .*RECALL10=[0-9.]+ RECALLK=[0-9.]+" \
      | sed "s/^/itopk=$itopk sw=$sw /"
  done
done

echo "=== 5. single-query latency ==="
for itopk in 512 256 128; do
  for sw in 4 2 1; do
    CAGRA_ITOPK="$itopk" CAGRA_SW="$sw" ./query_bench ./libalgo.so 250000 256 1000 10 2>/dev/null \
      | grep -oE "AVG_US=[0-9.]+" | sed "s/^/itopk=$itopk sw=$sw /"
  done
done
echo DONE

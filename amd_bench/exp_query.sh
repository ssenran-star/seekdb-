#!/usr/bin/env bash
# Scan search params (itopk_size / search_width): these directly set per-query
# latency (the contest issues ~4096 single queries) and recall. Never tuned before.
set -u
cd /workspace/work
export LD_LIBRARY_PATH=/workspace/work:/seekdb_workspace/bridge:/opt/hipvs/lib:/opt/rocm/lib

echo "=== rebuild algo bridge (now supports CAGRA_ITOPK / CAGRA_SW) ==="
/opt/rocm/bin/hipcc -shared -fPIC -O2 -D__HIP_PLATFORM_AMD__ -I/opt/hipvs/include -I/opt/rocm/include \
  seekdb_cuvs_bridge_algo.c -L/opt/hipvs/lib -Wl,-rpath,/opt/hipvs/lib:/opt/rocm/lib -lcuvs_c -o libalgo.so \
  && echo REBUILD_OK

echo "=== recall vs search params (256-dim, niter=60 igdeg=128 fixed) ==="
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

echo "=== single-query latency vs search params ==="
for itopk in 512 256 128; do
  for sw in 4 2 1; do
    CAGRA_ITOPK="$itopk" CAGRA_SW="$sw" ./query_bench ./libalgo.so 250000 256 1000 10 2>/dev/null \
      | grep -oE "AVG_US=[0-9.]+" | sed "s/^/itopk=$itopk sw=$sw /"
  done
done
echo DONE

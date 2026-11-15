#!/usr/bin/env bash
# Isolate intermediate_graph_degree (igdeg) impact on recall@100, at the
# already-lossless niter per dim (256-dim niter=70, 128-dim niter=40).
# If igdeg can drop without recall@100 loss, it is a second free lever that
# multiplies with the niter cut.
set -u
cd /workspace/work
export LD_LIBRARY_PATH=/workspace/work:/seekdb_workspace/bridge:/opt/hipvs/lib:/opt/rocm/lib

run() {  # run <dim> <niter> <igdeg>
  local dim=$1 niter=$2 igdeg=$3
  CAGRA_ALGO=2 CAGRA_NITER="$niter" CAGRA_IGDEG="$igdeg" ./bridge_test ./libalgo.so 250000 "$dim" 100 100 2>/dev/null \
    | grep -oE "BUILD_MS=[0-9.]+ .*RECALL10=[0-9.]+ RECALLK=[0-9.]+" \
    | sed "s/^/dim=$dim niter=$niter igdeg=$igdeg /"
}

echo "=== 256-dim (niter=70 fixed), igdeg scan ==="
export BASE_F32=/workspace/work/d256/base.f32 QUERY_F32=/workspace/work/d256/query.f32 GT_I32=/workspace/work/d256/gt.i32 GT_Q=100 GT_K=100
run 256 70 256
run 256 70 192
run 256 70 160
run 256 70 128

echo "=== 128-dim (niter=40 fixed), igdeg scan ==="
export BASE_F32=/workspace/work/d128/base.f32 QUERY_F32=/workspace/work/d128/query.f32 GT_I32=/workspace/work/d128/gt.i32 GT_Q=100 GT_K=100
run 128 40 256
run 128 40 192
run 128 40 128
echo DONE

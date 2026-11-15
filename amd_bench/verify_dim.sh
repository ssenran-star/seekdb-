#!/usr/bin/env bash
# Verify dimension-aware niter: build via official build.sh, then measure
# 128-dim (expect ~21700ms, niter=40) and 256-dim (expect ~32700ms, niter=70).
set -u
TOKEN='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'
WORK=/workspace/work

echo "=== 1. build dim-aware bridge ==="
mkdir -p /tmp/bv3/tools/hipvs_bridge /tmp/bridge3
cd /tmp/bv3
for f in seekdb_cuvs_bridge.c seekdb_cuvs_bridge.h build.sh; do
  curl -fsSL -H "Authorization: Bearer $TOKEN" \
    "https://api.github.com/repos/ssenran-star/seekdb-/contents/tools/hipvs_bridge/$f?ref=amd-cagra-submit" \
    | python3 -c "import sys,json,base64;open('/tmp/bv3/tools/hipvs_bridge/$f','wb').write(base64.b64decode(json.load(sys.stdin)['content']))"
done
HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm bash tools/hipvs_bridge/build.sh /tmp/bridge3/libseekdb_cuvs_bridge.so && echo BUILD_SH_OK

echo "=== 2. measure 128-dim (expect niter=40 -> BUILD_MS ~21700) ==="
cd "$WORK"
LD_LIBRARY_PATH=/tmp/bridge3:/opt/hipvs/lib:/opt/rocm/lib \
BASE_F32=$WORK/d128/base.f32 QUERY_F32=$WORK/d128/query.f32 GT_I32=$WORK/d128/gt.i32 GT_Q=100 GT_K=100 \
./bridge_test /tmp/bridge3/libseekdb_cuvs_bridge.so 250000 128 100 100

echo "=== 3. measure 256-dim (expect niter=70 -> BUILD_MS ~32700) ==="
LD_LIBRARY_PATH=/tmp/bridge3:/opt/hipvs/lib:/opt/rocm/lib \
BASE_F32=$WORK/d256/base.f32 QUERY_F32=$WORK/d256/query.f32 GT_I32=$WORK/d256/gt.i32 GT_Q=100 GT_K=100 \
./bridge_test /tmp/bridge3/libseekdb_cuvs_bridge.so 250000 256 100 100
echo DONE

#!/usr/bin/env bash
# Verify the FINAL bridge (niter=70 default) via the official build.sh path,
# then measure it directly to confirm niter=70 is what actually got built.
set -u
TOKEN='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'

mkdir -p /tmp/bv/tools/hipvs_bridge /tmp/bridge
cd /tmp/bv
for f in seekdb_cuvs_bridge.c seekdb_cuvs_bridge.h build.sh; do
  curl -fsSL -H "Authorization: Bearer $TOKEN" \
    "https://api.github.com/repos/ssenran-star/seekdb-/contents/tools/hipvs_bridge/$f?ref=amd-cagra-submit" \
    | python3 -c "import sys,json,base64;open('/tmp/bv/tools/hipvs_bridge/$f','wb').write(base64.b64decode(json.load(sys.stdin)['content']))"
done
echo "=== official build.sh ==="
HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm tools/hipvs_bridge/build.sh /tmp/bridge/libseekdb_cuvs_bridge.so && echo BUILD_SH_OK

echo "=== measure final bridge (niter=70 default, no CAGRA_* env) x3 ==="
cd /workspace/work
export LD_LIBRARY_PATH=/tmp/bridge:/opt/hipvs/lib:/opt/rocm/lib
export BASE_F32=/workspace/work/d256/base.f32
export QUERY_F32=/workspace/work/d256/query.f32
export GT_I32=/workspace/work/d256/gt.i32
export GT_Q=100
export GT_K=100
for i in 1 2 3; do
  ./bridge_test /tmp/bridge/libseekdb_cuvs_bridge.so 250000 256 100 100
done
echo DONE

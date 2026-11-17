#!/usr/bin/env bash
# Verify the search-buffer reuse: build the NEW bridge (reuse) to /tmp/bridge2,
# compile query_bench, then compare per-query latency OLD (/tmp/bridge, no
# reuse) vs NEW (/tmp/bridge2, reuse). Same niter=70 build on both.
set -u
# repo is public: fetch anonymously (embedded PAT expired)
WORK=/workspace/work

echo "=== 1. build NEW bridge (reuse) ==="
mkdir -p /tmp/bv2/tools/hipvs_bridge /tmp/bridge2
cd /tmp/bv2
for f in seekdb_cuvs_bridge.c seekdb_cuvs_bridge.h build.sh; do
  curl -fsSL \
    "https://api.github.com/repos/ssenran-star/seekdb-/contents/tools/hipvs_bridge/$f?ref=amd-cagra-submit" \
    | python3 -c "import sys,json,base64;open('/tmp/bv2/tools/hipvs_bridge/$f','wb').write(base64.b64decode(json.load(sys.stdin)['content']))"
done
HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm bash tools/hipvs_bridge/build.sh /tmp/bridge2/libseekdb_cuvs_bridge.so && echo BUILD_NEW_OK

echo "=== 2. compile query_bench ==="
cd "$WORK"
curl -fsSL \
  "https://api.github.com/repos/ssenran-star/seekdb-/contents/amd_bench/query_bench.c?ref=amd-cagra-submit" \
  | python3 -c "import sys,json,base64;open('query_bench.c','wb').write(base64.b64decode(json.load(sys.stdin)['content']))"
gcc -O2 -o query_bench query_bench.c -ldl && echo GCC_OK

echo "=== 3. per-query latency: OLD (no reuse) vs NEW (reuse) ==="
export BASE_F32=$WORK/d256/base.f32
export QUERY_F32=$WORK/d256/query.f32
echo "--- OLD ---"
LD_LIBRARY_PATH=/tmp/bridge:/opt/hipvs/lib:/opt/rocm/lib ./query_bench /tmp/bridge/libseekdb_cuvs_bridge.so 250000 256 3000 10
echo "--- NEW ---"
LD_LIBRARY_PATH=/tmp/bridge2:/opt/hipvs/lib:/opt/rocm/lib ./query_bench /tmp/bridge2/libseekdb_cuvs_bridge.so 250000 256 3000 10
echo DONE

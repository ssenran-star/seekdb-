#!/usr/bin/env bash
# Build seekdb from the contest-branch source with the cuVS bridge linked in.
# The stock observer shipped in the image rejects lib=cuvs ("this type of
# vector index lib is not supported") -- e2e/qps measurements need OUR build.
#
# Source comes from the GitHub tarball API (the instance cannot git clone).
# Build follows the contest's fixed compile flow (rules section 3.2).
#
# Usage: bash build_seekdb.sh        (~30-60 min, 64 cores)
set -euo pipefail
WORK=/workspace/work
SRC=/workspace/seekdb-src
REF=${REF:-amd-cagra-submit}
JOBS=${JOBS:-64}

mkdir -p "$WORK"
if [ ! -f "$SRC/CMakeLists.txt" ]; then
  echo "=== 1. fetch source tarball (ref=$REF) ==="
  rm -rf "$SRC" "$WORK"/ssenran-star--seekdb-*
  curl -fsSL "https://api.github.com/repos/ssenran-star/seekdb-/tarball/$REF" \
    | tar xz -C "$WORK"
  mv "$WORK"/ssenran-star--seekdb-* "$SRC"
  echo "source at $SRC ($(find "$SRC" -name '*.cpp' | wc -l) cpp files)"
fi

echo "=== 2. build hipVS bridge (official build.sh) ==="
HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm \
  bash "$SRC/tools/hipvs_bridge/build.sh" "$WORK/libseekdb_cuvs_bridge.so"
ls -la "$WORK/libseekdb_cuvs_bridge.so"

echo "=== 3. cmake configure + build seekdb (contest flags) ==="
cd "$SRC"
cmake -S . -B build_release -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DOB_ENABLE_UNITY=ON \
  -DOB_USE_LLD=ON \
  -DOB_BUILD_CUVS=ON \
  -DOB_BUILD_CUVS_TRACE=OFF \
  -DCUVS_BRIDGE_LIB="$WORK/libseekdb_cuvs_bridge.so"
cmake --build build_release --target seekdb -j"$JOBS"

echo ""
echo "=== done ==="
ls -la "$SRC/build_release/src/observer/seekdb"
echo "now run: bash $WORK/start_seekdb.sh   (it prefers this build automatically)"

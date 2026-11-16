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
echo "build_seekdb.sh v3 (2026-11-16 22:40) -- strip-components tarball"
WORK=/workspace/work
SRC=/workspace/seekdb-src
REF=${REF:-amd-cagra-submit}
JOBS=${JOBS:-64}
# token for api.github.com (anonymous rate limit is 60/h per IP and the
# tarball is one of the heavier calls); override with GITHUB_TOKEN=...
GITHUB_TOKEN=${GITHUB_TOKEN:-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}

mkdir -p "$WORK"
if [ ! -f "$SRC/CMakeLists.txt" ]; then
  echo "=== 1. fetch source tarball (ref=$REF) ==="
  rm -rf "$SRC"
  mkdir -p "$SRC"
  AUTH=()
  [ -n "${GITHUB_TOKEN:-}" ] && AUTH=(-H "Authorization: Bearer $GITHUB_TOKEN")
  # tarball root dir is "<owner>-<repo>-<sha>" and the repo name ends in '-',
  # so extract straight into $SRC instead of globbing the directory name
  curl -fsSL "${AUTH[@]}" "https://api.github.com/repos/ssenran-star/seekdb-/tarball/$REF" \
    | tar xz -C "$SRC" --strip-components=1
  echo "source at $SRC ($(find "$SRC" -name '*.cpp' | wc -l) cpp files)"
fi

echo "=== 2. build hipVS bridge (official build.sh) ==="
HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm \
  bash "$SRC/tools/hipvs_bridge/build.sh" "$WORK/libseekdb_cuvs_bridge.so"
ls -la "$WORK/libseekdb_cuvs_bridge.so"

echo "=== 2b. deps/3rd devtools (clang-17 toolchain; eval provides this frozen) ==="
DT="$SRC/deps/3rd/usr/local/oceanbase/devtools"
if [ ! -x "$DT/bin/clang-17" ] && [ ! -x "$DT/bin/clang" ]; then
  if bash "$SRC/deps/init/dep_create.sh"; then
    echo "dep_create.sh populated deps/3rd"
  else
    echo "dep_create.sh failed; searching the image for an existing devtools tree ..."
    found=$(find /seekdb_workspace /usr/local /opt /root /home -maxdepth 8 -type f \( -name 'clang-17' -o -name 'clang++-17' \) -path '*devtools*' 2>/dev/null | head -1)
    if [ -n "$found" ]; then
      mkdir -p "$SRC/deps/3rd/usr/local/oceanbase"
      ln -sfn "$(dirname "$(dirname "$found")")" "$DT"
      echo "linked devtools -> $(readlink "$DT")"
    else
      echo "WARN: no devtools tree found; cmake may fail with 'can't find suitable compiler'"
    fi
  fi
fi
ls "$DT/bin" 2>/dev/null | head -5

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

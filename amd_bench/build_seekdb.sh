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
CACHE_ROOT="${DEP_CACHE_DIR:-/opt/seekdb-deps-cache}"
if [ ! -x "$DT/bin/clang-17" ] && [ ! -x "$DT/bin/clang" ]; then
  echo "--- dep cache at $CACHE_ROOT ---"
  find "$CACHE_ROOT" -maxdepth 3 2>/dev/null | head -20 || true
  # (a) cache may already hold an extracted devtools tree under a hash dir
  found=$(find "$CACHE_ROOT" -maxdepth 10 -type f -name 'clang-17' -path '*devtools*bin*' 2>/dev/null | head -1; true)
  if [ -n "$found" ]; then
    DT_SRC=$(dirname "$(dirname "$found")")
    mkdir -p "$SRC/deps/3rd/usr/local/oceanbase"
    ln -sfn "$DT_SRC" "$DT"
    echo "linked devtools from cache -> $DT_SRC"
  fi
fi
if [ ! -x "$DT/bin/clang-17" ] && [ ! -x "$DT/bin/clang" ]; then
  # (b) cache may hold downloaded rpms that were never extracted
  rpms=$(find "$CACHE_ROOT" -name '*.rpm' 2>/dev/null | head -200; true)
  if [ -n "$rpms" ] && command -v rpm2cpio >/dev/null 2>&1; then
    mkdir -p "$SRC/deps/3rd"
    n=$(echo "$rpms" | wc -l)
    echo "extracting $n cached rpms into deps/3rd ..."
    (cd "$SRC/deps/3rd" && for r in $rpms; do rpm2cpio "$r" | cpio -di -u --quiet || true; done) || true
    found=$(find "$SRC/deps/3rd" -type f -name 'clang-17' -path '*devtools*bin*' 2>/dev/null | head -1; true)
    if [ -n "$found" ]; then
      echo "extracted devtools -> $(dirname "$(dirname "$found")")"
    fi
  elif [ -n "$rpms" ]; then
    echo "cache has rpms but rpm2cpio is missing; apt-get install -y rpm2cpio cpio?"
  fi
fi
if [ ! -x "$DT/bin/clang-17" ] && [ ! -x "$DT/bin/clang" ]; then
  # (c) official downloader (profile lookup is CWD-relative)
  echo "--- dep_create.sh ---"
  (cd "$SRC/deps/init" && bash dep_create.sh) || echo "dep_create.sh failed"
fi
if [ ! -x "$DT/bin/clang-17" ] && [ ! -x "$DT/bin/clang" ]; then
  # (d) last resort: shim devtools out of the ROCm toolchain + system gcc layout
  echo "--- shim devtools from ROCm clang + system gcc ---"
  ROCM_LLVM=$(ls -d /opt/rocm*/lib/llvm 2>/dev/null | head -1)
  [ -n "$ROCM_LLVM" ] || ROCM_LLVM=$(dirname "$(dirname "$(command -v clang 2>/dev/null || echo /nonexistent)")")
  mkdir -p "$DT/bin"
  for tool in clang clang++ ld.lld llvm-objcopy; do
    src="$ROCM_LLVM/bin/$tool"
    [ -x "$src" ] && ln -sfn "$src" "$DT/bin/$tool" && echo "  $tool -> $src"
  done
  [ -e "$DT/bin/clang-17" ] || ln -sfn "$DT/bin/clang" "$DT/bin/clang-17"
  [ -e "$DT/bin/clang++-17" ] || ln -sfn "$DT/bin/clang++" "$DT/bin/clang++-17"
  ln -sfn /usr/bin/gcc "$DT/bin/gcc" 2>/dev/null || true
  ln -sfn /usr/bin/g++ "$DT/bin/g++" 2>/dev/null || true
  mkdir -p "$DT/lib"
  ln -sfn /usr/lib/gcc "$DT/lib/gcc" 2>/dev/null || true
  ls "$DT/bin" 2>/dev/null | head -8 || true
fi
echo "--- devtools status ---"
ls "$DT/bin" 2>/dev/null | head -8 || echo "no devtools bin dir"
[ -x "$DT/bin/clang-17" ] || [ -x "$DT/bin/clang" ] || echo "WARN: no compiler in devtools; cmake will fail"

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

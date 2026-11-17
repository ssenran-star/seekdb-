#!/usr/bin/env bash
# try_full_adaptor.sh v1 (2026-11-17 15:20) -- A/B the full adaptor on a warm build
# The full adaptor (branch amd-cagra-full) adds OBCUVS03 overlapped snapshot +
# persistent query worker + vid-bound cache on top of the validated bf+FP16
# bridge. Only ob_vsag_adaptor.cpp differs from the validated base, so an
# incremental rebuild is minutes, not half an hour.
#
# Decision rule: adopt iff
#   e2e CREATE INDEX window shrinks (F cut) AND qps does not drop AND the
#   first query still returns 10 ids. Otherwise restore the backup file.
set -u
echo "try_full_adaptor.sh v1 (2026-11-17 15:20)"
WORK=/workspace/work
SRC=/workspace/seekdb-src
REF=${REF:-amd-cagra-full}
API=https://api.github.com/repos/ssenran-star/seekdb-/contents
F=src/oblib/lib/vector/ob_vsag_adaptor.cpp

[ -d "$SRC/build_release" ] || { echo "no warm build at $SRC/build_release (run build_seekdb.sh first)"; exit 1; }

echo "=== 1. fetch full adaptor ($REF) ==="
curl -fsSL "$API/$F?ref=$REF" \
  | python3 -c "import sys,json,base64;open('$SRC/$F','wb').write(base64.b64decode(json.load(sys.stdin)['content']))" \
  || { echo FETCH_FAIL; exit 1; }
cp -n "$SRC/$F" "$WORK/ob_vsag_adaptor.full.bak" 2>/dev/null || true
# keep a pristine copy of the validated version on first run
[ -f "$WORK/ob_vsag_adaptor.valid.bak" ] || git -C "$SRC" show HEAD:$F > "$WORK/ob_vsag_adaptor.valid.bak" 2>/dev/null \
  || curl -fsSL "$API/$F?ref=amd-cagra-submit" \
     | python3 -c "import sys,json,base64;open('$WORK/ob_vsag_adaptor.valid.bak','wb').write(base64.b64decode(json.load(sys.stdin)['content']))"
echo "adaptor swapped; rebuilding seekdb (incremental) ..."

echo "=== 2. incremental build ==="
JOBS=${JOBS:-24}
cmake --build "$SRC/build_release" --target seekdb -j"$JOBS" 2>&1 | tail -4
[ -x "$SRC/build_release/src/observer/seekdb" ] || { echo BUILD_FAIL; exit 1; }

echo "=== 3. restart observer ==="
bash "$WORK/start_seekdb.sh" || exit 1

echo "=== 4. e2e 250K x 256 (compare window vs 24.42s) ==="
N=250000 DIM=256 bash "$WORK/e2e_measure.sh" || exit 1

echo "=== 5. qps 250K x 256 (compare vs 125.7 QPS) ==="
N=250000 DIM=256 bash "$WORK/qps_measure.sh" || exit 1

echo ""
echo "=== decision ==="
echo "adopt if: window < 24.4s AND QPS >= 125 AND first query returned 10 ids."
echo "to restore the validated adaptor:"
echo "  cp $WORK/ob_vsag_adaptor.valid.bak $SRC/$F && cmake --build $SRC/build_release --target seekdb -j$JOBS"
echo "DONE"

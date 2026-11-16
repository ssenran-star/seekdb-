#!/usr/bin/env bash
# Probe the fixed hipVS library for graph-construction APIs we can call from
# the bridge (rules allow custom GPU graph building; only SEARCH must stay on
# the organizer-fixed cuvsCagraSearch). Also runs a quick dense-graph check.
#
# Run after boot_r2.sh (needs /workspace/work/hipvs_bridge + bridge_test +
# d128/d256 data). Output is small — paste it back as-is.
set -u
WORK=/workspace/work
cd "$WORK"

echo "=== 1. exported symbols (build side) ==="
for so in /opt/hipvs/lib/libcuvs_c.so /opt/hipvs/lib/libcuvs.so; do
  [ -f "$so" ] || continue
  echo "--- $so ---"
  nm -D "$so" 2>/dev/null | grep -iE "cuvs(NNDescent|Cagra|BruteForce)[A-Za-z]*" | awk '{print $NF}' | sort -u
done

echo ""
echo "=== 2. headers ==="
ls /opt/hipvs/include/cuvs/neighbors/ 2>/dev/null
echo "--- cagra.h: FromArgs / optimize / prune signatures ---"
grep -nE "FromArgs|ptimize|rune" /opt/hipvs/include/cuvs/neighbors/cagra.h 2>/dev/null | head -20
echo "--- brute_force.h exists? ---"
ls /opt/hipvs/include/cuvs/neighbors/brute_force.h 2>/dev/null && grep -nE "cuvsBruteForce[A-Za-z]*\(" /opt/hipvs/include/cuvs/neighbors/brute_force.h | head -20

echo ""
echo "=== 3. dense-graph check (nd with CAGRA_GDEG=256, both dims) ==="
echo "    hypothesis: FromArgs on a 256-degree NN-Descent graph restores the"
echo "    ~0.002 recall that skipping RobustPrune costs (search hops get slower)."
export LD_LIBRARY_PATH=$WORK:/opt/hipvs/lib:/opt/rocm/lib
SO=/tmp/bridge_r2/libseekdb_cuvs_bridge.so
[ -f "$SO" ] || SO=$WORK/libbridge_r2.so
run() {  # run <dim> <label> <env...>
  local dim="$1" label="$2"; shift 2
  local data=$WORK/d$dim
  env CAGRA_BUILD_MODE=nd CAGRA_GDEG=256 "$@" \
    BASE_F32="$data/base.f32" QUERY_F32="$data/query.f32" GT_I32="$data/gt.i32" \
    GT_Q=100 GT_K=100 SNAP_PATH=/tmp/snap_probe.bin \
    "$WORK/bridge_test" "$SO" 250000 "$dim" 100 100 2>/dev/null | tail -1 \
    | grep -oE "BUILD_MS=[0-9.]+|SEARCH_MS=[0-9.]+|RECALL10=[0-9.]+|RECALLK=[0-9.]+" \
    | tr '\n' ' ' | sed "s/^/$label /"
  echo ""
}
run 128 "gdeg256/mi=25/th=1e-4" CAGRA_MAX_ITER=25 CAGRA_TERM_TH=0.0001
run 256 "gdeg256/mi=50/th=1e-3" CAGRA_MAX_ITER=50 CAGRA_TERM_TH=0.001
echo ""
echo "=== reference (selected defaults, gdeg=128) for comparison ==="
run 128 "gdeg128/mi=25/th=1e-4" CAGRA_MAX_ITER=25 CAGRA_TERM_TH=0.0001 CAGRA_GDEG=128
run 256 "gdeg128/mi=50/th=1e-3" CAGRA_MAX_ITER=50 CAGRA_TERM_TH=0.001 CAGRA_GDEG=128
echo DONE

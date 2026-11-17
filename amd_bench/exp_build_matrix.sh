#!/usr/bin/env bash
# Round-2 decisive build experiment: standalone NN-Descent (early-stop) +
# cuvsCagraIndexFromArgs ("nd") versus the tuned legacy cuvsCagraBuild path.
#
# What it answers:
#   1. Does the nd pipeline (early-stop + FromArgs) keep recall@100 within the
#      0.005 hard-gate budget versus the legacy pipeline on BOTH dimensions?
#   2. How much build time does early termination actually save per dim?
#   3. Which (CAGRA_MAX_ITER, CAGRA_TERM_TH) pair is the lossless optimum?
#
# Method (same as RESULTS.md):
#   - 250K float32, 128-dim and 256-dim, exact top-100 ground truth
#   - median of >=3 runs per config (GPU build noise can reach 1.8x)
#   - recall@100 is the reliable metric (10000 samples, noise ~0.0004)
#
# Usage: bash exp_build_matrix.sh            (runs both dims)
#        DIMS=128 bash exp_build_matrix.sh   (single dim)
set -u
WORK=${WORK:-/workspace/work}
SO=${SO:-$WORK/libbridge_r2.so}
TEST=${TEST:-$WORK/bridge_test}
RUNS=${RUNS:-3}

cd "$WORK"
export LD_LIBRARY_PATH=$WORK:/opt/hipvs/lib:/opt/rocm/lib

# ---- prep: probe binary + datasets + exact top-100 ground truth ----
if [ ! -x "$TEST" ]; then
  [ -f "$WORK/bridge_test.c" ] || curl -fsSL \
    "https://api.github.com/repos/ssenran-star/seekdb-/contents/amd_bench/bridge_test.c?ref=${REF:-amd-cagra-submit}" \
    | python3 -c "import sys,json,base64;open('bridge_test.c','wb').write(base64.b64decode(json.load(sys.stdin)['content']))"
  gcc -O2 -o "$TEST" "$WORK/bridge_test.c" -ldl || { echo GCC_FAIL; exit 1; }
fi
if [ ! -f "$WORK/gen_data.py" ]; then
  curl -fsSL \
    "https://api.github.com/repos/ssenran-star/seekdb-/contents/amd_bench/gen_data.py?ref=${REF:-amd-cagra-submit}" \
    | python3 -c "import sys,json,base64;open('gen_data.py','wb').write(base64.b64decode(json.load(sys.stdin)['content']))"
fi
for d in 128 256; do
  if [ ! -f "$WORK/d$d/base.f32" ]; then
    echo "generating d$d ..."
    python3 "$WORK/gen_data.py" --n 250000 --dim "$d" --nq 100 --topk 100 --out "$WORK/d$d" || exit 1
  fi
done

echo "=== build round-2 bridge (default mode = auto) ==="
# repo is public: anonymous api.github.com fetch (the old embedded PAT is dead
# and a bad Authorization header makes GitHub reject even public reads)
REF=${REF:-amd-cagra-submit}
mkdir -p /tmp/bv_r2/tools/hipvs_bridge /tmp/bridge_r2
cd /tmp/bv_r2
for f in seekdb_cuvs_bridge.c seekdb_cuvs_bridge.h build.sh; do
  [ -s "$WORK/hipvs_bridge/$f" ] && cp "$WORK/hipvs_bridge/$f" "tools/hipvs_bridge/$f" && continue
  curl -fsSL \
    "https://api.github.com/repos/ssenran-star/seekdb-/contents/tools/hipvs_bridge/$f?ref=$REF" \
    | python3 -c "import sys,json,base64;open('tools/hipvs_bridge/$f','wb').write(base64.b64decode(json.load(sys.stdin)['content']))" \
    || { echo "FETCH_FAIL $f"; exit 1; }
done
HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm \
  bash tools/hipvs_bridge/build.sh /tmp/bridge_r2/libseekdb_cuvs_bridge.so \
  && echo BRIDGE_OK || { echo BRIDGE_BUILD_FAIL; exit 1; }
cp /tmp/bridge_r2/libseekdb_cuvs_bridge.so "$SO"

if [ ! -x "$TEST" ]; then
  echo "bridge_test missing at $TEST"; exit 1
fi

median3() {  # median3 v1 v2 v3 ...
  printf '%s\n' "$@" | sort -n | awk '{a[NR]=$1} END{print a[int((NR+1)/2)]}'
}

run_cfg() {  # run_cfg <dim> <n> <label> <env...>
  local dim="$1" n="$2" label="$3"; shift 3
  local data
  if [ "$dim" = 128 ]; then data=$WORK/d128; else data=$WORK/d256; fi
  local builds="" r10s="" rks=""
  for i in $(seq 1 "$RUNS"); do
    local out
    out=$(env "$@" \
      BASE_F32="$data/base.f32" QUERY_F32="$data/query.f32" GT_I32="$data/gt.i32" \
      GT_Q=100 GT_K=100 SNAP_PATH=/tmp/snap_probe.bin \
      "$TEST" "$SO" "$n" "$dim" 100 100 2>/dev/null | tail -1)
    local b r10 rk
    b=$(echo "$out" | grep -oE 'BUILD_MS=[0-9.]+' | cut -d= -f2)
    r10=$(echo "$out" | grep -oE 'RECALL10=[0-9.]+' | cut -d= -f2)
    rk=$(echo "$out" | grep -oE 'RECALLK=[0-9.]+' | cut -d= -f2)
    [ -n "$b" ] && { builds="$builds $b"; r10s="$r10s $r10"; rks="$rks $rk"; }
    [ -z "$b" ] && echo "  !! run failed: $label ($out)"
  done
  [ -z "$builds" ] && { echo "$label FAILED"; return; }
  echo "$label build_ms=$(median3 $builds) recall10=$(median3 $r10s) recall100=$(median3 $rks)"
}

for dim in ${DIMS:-128 256}; do
  if [ "$dim" = 128 ]; then n=250000; else n=250000; fi
  echo ""
  echo "==================== dim=$dim  n=$n ===================="
  echo "--- reference: legacy (tuned 2c76a2f defaults) ---"
  run_cfg "$dim" "$n" "legacy/ref" CAGRA_BUILD_MODE=legacy

  echo "--- nd mode: early-stop sweep (max_iter x term_th) ---"
  for mi in $( [ "$dim" = 128 ] && echo 30 25 20 15 || echo 60 50 40 30 ); do
    for th in 0.0001 0.001 0.01; do
      run_cfg "$dim" "$n" "nd/mi=$mi/th=$th" \
        CAGRA_BUILD_MODE=nd CAGRA_MAX_ITER=$mi CAGRA_TERM_TH=$th
    done
  done
done
echo ""
echo "=== selection rule ==="
echo "Pick per dim the FASTEST nd config whose recall100 drop vs 'legacy/ref'"
echo "is < 0.003 (noise floor, 1.5x safety margin to the 0.005 gate)."
echo "Then set its (CAGRA_MAX_ITER, CAGRA_TERM_TH) as the dim-aware defaults"
echo "in tools/hipvs_bridge/seekdb_cuvs_bridge.c (build_nd) and re-verify."
echo DONE

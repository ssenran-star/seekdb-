#!/usr/bin/env bash
# exp_delta.sh v3 (2026-11-17 09:15) -- interleaved same-session delta calibration
# The gate is candidate-vs-frozen-baseline in the SAME round. v2 ran each
# config's RUNS back to back, so later configs (bf) ran on a heat-soaked GPU
# while frozen ran cold -- that biased build_ms against bf. v3 round-robins
# the configs so every one sees the same thermal state.
#
#   frozen  = CAGRA_BUILD_MODE=legacy CAGRA_NITER=100 CAGRA_IGDEG=256
#   current = CAGRA_BUILD_MODE=legacy (defaults: niter 25/50, igdeg 128)
#   bf      = CAGRA_BUILD_MODE=bf     (exact kNN + FromArgs, self-drop)
#   nd      = CAGRA_BUILD_MODE=nd     (experiment only)
# Gate = 0.005 drop vs frozen on recall@100; aim <= 0.003.
set -u
echo "exp_delta.sh v3 (2026-11-17 09:15) -- interleaved"
WORK=${WORK:-/workspace/work}
SO=${SO:-/tmp/bridge_r2/libseekdb_cuvs_bridge.so}
RUNS=${RUNS:-5}
cd "$WORK"
export LD_LIBRARY_PATH=$WORK:/opt/hipvs/lib:/opt/rocm/lib
if [ ! -f "$SO" ]; then
  echo "bridge .so missing at $SO -- building from $WORK/hipvs_bridge"
  mkdir -p "$(dirname "$SO")"
  HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm bash "$WORK/hipvs_bridge/build.sh" "$SO" || exit 1
fi
[ -x "$WORK/bridge_test" ] || gcc -O2 -o "$WORK/bridge_test" "$WORK/bridge_test.c" -ldl || exit 1
# datasets: generate if missing (instance resets wipe them)
for dim in 128 256; do
  if [ ! -f "$WORK/d$dim/base.f32" ]; then
    echo "generating d$dim ..."
    python3 "$WORK/gen_data.py" --n 250000 --dim "$dim" --nq 100 --topk 100 --out "$WORK/d$dim" || exit 1
  fi
done

median() { printf '%s\n' "$@" | sort -n | awk '{a[NR]=$1} END{print a[int((NR+1)/2)]}'; }

LABELS=("frozen(100/256) " "current(25/50)  " "bf(default)     " "nd(experiment)  ")
ENVS=(
  "CAGRA_BUILD_MODE=legacy CAGRA_NITER=100 CAGRA_IGDEG=256"
  "CAGRA_BUILD_MODE=legacy"
  "CAGRA_BUILD_MODE=bf"
  "CAGRA_BUILD_MODE=nd"
)

run_one() {  # run_one <dim> <cfg-idx> -> echoes "BUILD RECALL10 RECALL100"
  local dim="$1" idx="$2"
  local data=$WORK/d$dim out
  out=$(env ${ENVS[$idx]} \
    BASE_F32="$data/base.f32" QUERY_F32="$data/query.f32" GT_I32="$data/gt.i32" \
    GT_Q=100 GT_K=100 SNAP_PATH=/tmp/snap_probe.bin \
    "$WORK/bridge_test" "$SO" 250000 "$dim" 100 100 2>/dev/null | tail -1)
  echo "$out" | grep -oE 'BUILD_MS=[0-9.]+' | cut -d= -f2 | tr -d '\n'
  echo -n " "
  echo "$out" | grep -oE 'RECALL10=[0-9.]+' | cut -d= -f2 | tr -d '\n'
  echo -n " "
  echo "$out" | grep -oE 'RECALLK=[0-9.]+' | cut -d= -f2
}

for dim in 128 256; do
  echo "==================== dim=$dim ===================="
  B=("" "" "" ""); R=("" "" "" ""); R10=("" "" "" "")
  for i in $(seq 1 "$RUNS"); do
    for idx in 0 1 2 3; do
      line=$(run_one "$dim" "$idx")
      b=$(echo "$line" | cut -d' ' -f1); r=$(echo "$line" | cut -d' ' -f3); r10=$(echo "$line" | cut -d' ' -f2)
      if [ -n "$b" ]; then
        B[$idx]="${B[$idx]} $b"; R[$idx]="${R[$idx]} $r"; R10[$idx]="${R10[$idx]} $r10"
      else
        echo "  !! failed run $i cfg=$idx"
      fi
    done
    echo "  (round $i done)"
  done
  for idx in 0 1 2 3; do
    echo "${LABELS[$idx]} recall10=$(median ${R10[$idx]}) recall100=$(median ${R[$idx]}) build_ms=$(median ${B[$idx]})"
  done
  FROZEN=$(median ${R[0]}); CUR=$(median ${R[1]}); BF=$(median ${R[2]}); ND=$(median ${R[3]})
  python3 - "$FROZEN" "$CUR" "$BF" "$ND" <<'PY'
import sys
f, c, b, n = (float(x) for x in sys.argv[1:5])
def verdict(d):
    return "OK" if d <= 0.003 else ("IN-GATE" if d <= 0.005 else "OVER")
print("  delta current-vs-frozen = %.4f   %s" % (f - c, verdict(f - c)))
print("  delta bf-vs-frozen      = %.4f   %s" % (f - b, verdict(f - b)))
print("  delta nd-vs-frozen      = %.4f   %s" % (f - n, verdict(f - n)))
print("  => ship bf default if bf <= 0.003 (or <= 0.005 with known synthetic->real shrinkage)")
PY
done
echo DONE

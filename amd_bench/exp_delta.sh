#!/usr/bin/env bash
# Same-session delta calibration. The gate is candidate-vs-frozen-baseline in
# the SAME round, and build-to-build recall variance (~0.0015) makes
# cross-session absolute numbers unreliable. This measures, on one session:
#   frozen  = CAGRA_BUILD_MODE=legacy CAGRA_NITER=100 CAGRA_IGDEG=256
#   current = CAGRA_BUILD_MODE=legacy (defaults: niter 25/50, igdeg 128)
#   nd      = CAGRA_BUILD_MODE=nd     (defaults: 25/1e-4, 50/1e-3)
# and prints recall@100 deltas versus frozen. Gate = 0.005; stay <= 0.003.
set -u
WORK=${WORK:-/workspace/work}
SO=${SO:-/tmp/bridge_r2/libseekdb_cuvs_bridge.so}
RUNS=${RUNS:-5}
cd "$WORK"
export LD_LIBRARY_PATH=$WORK:/opt/hipvs/lib:/opt/rocm/lib
[ -f "$SO" ] || { echo "bridge .so missing at $SO (run boot_r2.sh first)"; exit 1; }
[ -x "$WORK/bridge_test" ] || gcc -O2 -o "$WORK/bridge_test" "$WORK/bridge_test.c" -ldl || exit 1

median() { printf '%s\n' "$@" | sort -n | awk '{a[NR]=$1} END{print a[int((NR+1)/2)]}'; }

probe() {  # probe <dim> <label> <env...>
  local dim="$1" label="$2"; shift 2
  local data=$WORK/d$dim rs="" r10s=""
  for i in $(seq 1 "$RUNS"); do
    local out b r rk
    out=$(env "$@" \
      BASE_F32="$data/base.f32" QUERY_F32="$data/query.f32" GT_I32="$data/gt.i32" \
      GT_Q=100 GT_K=100 SNAP_PATH=/tmp/snap_probe.bin \
      "$WORK/bridge_test" "$SO" 250000 "$dim" 100 100 2>/dev/null | tail -1)
    b=$(echo "$out" | grep -oE 'BUILD_MS=[0-9.]+' | cut -d= -f2)
    r=$(echo "$out" | grep -oE 'RECALLK=[0-9.]+' | cut -d= -f2)
    r10=$(echo "$out" | grep -oE 'RECALL10=[0-9.]+' | cut -d= -f2)
    [ -n "$b" ] && rs="$rs $r" && r10s="$r10s $r10" && builds="$builds $b"
    [ -z "$b" ] && echo "  !! failed run $i: $label"
  done
  echo "$label recall10=$(median $r10s) recall100=$(median $rs) build_ms=$(median $builds)"
  LAST100=$(median $rs)
}

for dim in 128 256; do
  echo "==================== dim=$dim ===================="
  builds=""
  probe "$dim" "frozen(100/256) " CAGRA_BUILD_MODE=legacy CAGRA_NITER=100 CAGRA_IGDEG=256
  FROZEN=$LAST100
  builds=""
  probe "$dim" "current(25/50)  " CAGRA_BUILD_MODE=legacy
  CUR=$LAST100
  builds=""
  probe "$dim" "nd(default)     " CAGRA_BUILD_MODE=nd
  ND=$LAST100
  python3 - "$FROZEN" "$CUR" "$ND" <<'PY'
import sys
f, c, n = (float(x) for x in sys.argv[1:4])
print("  delta current-vs-frozen = %.4f   %s" % (f - c, "OK" if f - c <= 0.003 else ("IN-GATE" if f - c <= 0.005 else "OVER")))
print("  delta nd-vs-frozen      = %.4f   %s" % (f - n, "OK" if f - n <= 0.003 else ("IN-GATE" if f - n <= 0.005 else "OVER")))
PY
done
echo DONE

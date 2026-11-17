#!/usr/bin/env bash
# exp_itopk_bf.sh v1 (2026-11-17 14:10) -- itopk sweep ON THE BF GRAPH
# Why: itopk 512->256 on the legacy NN-Descent graph collapsed recall@100
# 0.87 -> 0.71 (exp_query.sh). The bf exact-kNN graph has strictly better
# connectivity, so 256 may hold inside the gate. If it does, the GPU walk
# (3.65ms/query at itopk=512) drops to ~2ms -- the only route to the 1.5x
# query speedup (= 2 full query points).
# Gate: bf-itopk recall100 delta vs same-session frozen <= 0.003 (aim) /
# 0.005 (hard), and absolute floors SIFT@100>=0.95 / Hidden@100>=0.85.
set -u
echo "exp_itopk_bf.sh v1 (2026-11-17 14:10)"
WORK=${WORK:-/workspace/work}
SO=${SO:-/tmp/bridge_r2/libseekdb_cuvs_bridge.so}
RUNS=${RUNS:-3}
cd "$WORK"
export LD_LIBRARY_PATH=$WORK:/opt/hipvs/lib:/opt/rocm/lib
if [ ! -f "$SO" ]; then
  echo "building bridge -> $SO"
  mkdir -p "$(dirname "$SO")"
  HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm bash "$WORK/hipvs_bridge/build.sh" "$SO" || exit 1
fi
[ -x "$WORK/bridge_test" ] || gcc -O2 -o "$WORK/bridge_test" "$WORK/bridge_test.c" -ldl || exit 1
for dim in 128 256; do
  if [ ! -f "$WORK/d$dim/base.f32" ]; then
    echo "generating d$dim ..."
    python3 "$WORK/gen_data.py" --n 250000 --dim "$dim" --nq 100 --topk 100 --out "$WORK/d$dim" || exit 1
  fi
done

median() { printf '%s\n' "$@" | sort -n | awk '{a[NR]=$1} END{print a[int((NR+1)/2)]}'; }

run() {  # run <dim> <env...> -> "BUILD SEARCH R10 R100"
  local dim="$1"; shift
  local data=$WORK/d$dim out
  out=$(env "$@" \
    BASE_F32="$data/base.f32" QUERY_F32="$data/query.f32" GT_I32="$data/gt.i32" \
    GT_Q=100 GT_K=100 SNAP_PATH=/tmp/snap_probe.bin \
    timeout --kill-after=30 600 "$WORK/bridge_test" "$SO" 250000 "$dim" 100 100 2>/dev/null | tail -1)
  for k in BUILD_MS SEARCH_MS RECALL10 RECALLK; do
    echo "$out" | grep -oE "$k=[0-9.]+" | cut -d= -f2 | tr -d '\n'
    echo -n " "
  done
  echo
}

for dim in 128 256; do
  echo "==================== dim=$dim ===================="
  bs=""; ss=""; r10s=""; r100s=""
  for i in $(seq 1 "$RUNS"); do
    line=$(run "$dim" CAGRA_BUILD_MODE=legacy CAGRA_NITER=100 CAGRA_IGDEG=256)
    bs="$bs $(echo "$line" | cut -d' ' -f1)"; ss="$ss $(echo "$line" | cut -d' ' -f2)"
    r10s="$r10s $(echo "$line" | cut -d' ' -f3)"; r100s="$r100s $(echo "$line" | cut -d' ' -f4)"
  done
  F=$(median $r100s)
  echo "frozen(100/256) itopk=default build=$(median $bs)ms search=$(median $ss)ms recall10=$(median $r10s) recall100=$F"
  for itopk in 512 384 256 192; do
    bs=""; ss=""; r10s=""; r100s=""
    for i in $(seq 1 "$RUNS"); do
      echo "  [$(date +%T)] dim=$dim itopk=$itopk run=$i/$RUNS"
      line=$(run "$dim" CAGRA_BUILD_MODE=bf CAGRA_ITOPK=$itopk CAGRA_SW=4)
      bs="$bs $(echo "$line" | cut -d' ' -f1)"; ss="$ss $(echo "$line" | cut -d' ' -f2)"
      r10s="$r10s $(echo "$line" | cut -d' ' -f3)"; r100s="$r100s $(echo "$line" | cut -d' ' -f4)"
    done
    R=$(median $r100s)
    awk -v f="$F" -v r="$R" -v it="$itopk" -v b="$(median $bs)" -v s="$(median $ss)" -v r10="$(median $r10s)" \
      'BEGIN{d=f-r; v=(d<=0.003)?"OK":((d<=0.005)?"IN-GATE":"OVER");
             printf "bf itopk=%s build=%sms search=%sms recall10=%s recall100=%.4f delta=%.4f %s\n", it, b, s, r10, r, d, v}'
  done
done
echo ""
echo "=== decision ==="
echo "adopt the LOWEST itopk whose delta is OK/IN-GATE on BOTH dims as the"
echo "bridge default (CAGRA_ITOPK in seekdb_cuvs_search); re-check query QPS"
echo "with qps_measure.sh afterwards."
echo "DONE"

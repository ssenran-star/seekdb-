#!/usr/bin/env bash
# Full parameter sweep on the AMD instance.
#   - builds the tunable bridge once (hipcc)
#   - optionally generates an evaluation-scale dataset
#   - sweeps nn_descent_niter / graph_degree, printing BUILD_MS + RECALL
#
# Usage: bash sweep.sh [dataset_dir]      (dataset_dir empty => use the 10K image dataset)
set -u

WORK=${WORK:-/workspace/work}
DATA=${1:-}
BRIDGE=${BRIDGE:-/workspace/work/libtune.so}
TEST=${TEST:-/workspace/work/bridge_test}
TOKEN=${TOKEN:?set TOKEN}

export LD_LIBRARY_PATH=$WORK:/seekdb_workspace/bridge:/opt/hipvs/lib:/opt/rocm/lib

if [ -n "$DATA" ]; then
  DIM=$(cat "$DATA/dim.txt" 2>/dev/null || echo 128)
  N=$(python3 -c "import os;print(os.path.getsize('$DATA/base.f32')//4//$DIM)")
  # Point the probe at the generated dataset. Without these the test program
  # keeps reading the 10K image files and fails with BASE_SHORT.
  export BASE_F32="$DATA/base.f32"
  export QUERY_F32="$DATA/query.f32"
  export GT_I32="$DATA/gt.i32"
  echo "=== dataset: $DATA  N=$N DIM=$DIM ==="
  echo "    BASE_F32=$BASE_F32"
else
  N=10000; DIM=128
  echo "=== dataset: image 10K x 128 ==="
fi

echo ""
echo "=== baseline (params = frozen defaults) ==="
"$TEST" /seekdb_workspace/bridge/libseekdb_cuvs_bridge.so "$N" "$DIM" 100 10

REPS=${REPS:-3}

med() {  # median of the BUILD_MS values printed by the probe
  sort -g | awk '{a[NR]=$1} END{ if(NR%2) print a[(NR+1)/2]; else printf "%.1f\n",(a[NR/2]+a[NR/2+1])/2 }';
}

run_sweep() {  # run_sweep <label> <env assignments...>
  local label="$1"; shift
  local i
  for i in $(seq 1 $REPS); do
    env "$@" "$TEST" "$BRIDGE" "$N" "$DIM" 100 10
  done | grep -o 'BUILD_MS=[0-9.]*' | cut -d= -f2 | med |     xargs -I{} printf '%-22s median BUILD_MS={}\n' "$label"
}

echo ""
echo "=== sweep: nn_descent_niter (median of $REPS runs) ==="
for n in 100 80 60 40 30 20 15 10 5; do
  run_sweep "niter=$n" CAGRA_NITER=$n
done

echo ""
echo "=== sweep: graph_degree (niter=100, median of $REPS runs) ==="
for g in 128 96 64 48 32 24 16; do
  run_sweep "gdeg=$g" CAGRA_GDEG=$g
done

echo ""
echo "=== sweep: intermediate_graph_degree (niter=100, gdeg=128, median of $REPS runs) ==="
for ig in 256 192 128 96 64; do
  run_sweep "igdeg=$ig" CAGRA_IGDEG=$ig
done

echo ""
echo "=== raw repeats (noise visibility) ==="
for i in $(seq 1 $REPS); do
  printf 'rep%s ' "$i"
  "$TEST" "$BRIDGE" "$N" "$DIM" 100 10
done
echo ""
echo "DONE"

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
  echo "=== dataset: $DATA  N=$N DIM=$DIM ==="
else
  N=10000; DIM=128
  echo "=== dataset: image 10K x 128 ==="
fi

echo ""
echo "=== baseline (params = frozen defaults) ==="
"$TEST" /seekdb_workspace/bridge/libseekdb_cuvs_bridge.so "$N" "$DIM" 100 10

echo ""
echo "=== sweep: nn_descent_niter ==="
for n in 100 80 60 40 30 20 15 10 5; do
  printf 'niter=%-4s ' "$n"
  CAGRA_NITER=$n "$TEST" "$BRIDGE" "$N" "$DIM" 100 10
done

echo ""
echo "=== sweep: graph_degree (niter=100) ==="
for g in 128 96 64 48 32 24 16; do
  printf 'gdeg=%-4s ' "$g"
  CAGRA_GDEG=$g "$TEST" "$BRIDGE" "$N" "$DIM" 100 10
done

echo ""
echo "=== sweep: intermediate_graph_degree (niter=100, gdeg=128) ==="
for ig in 256 192 128 96 64; do
  printf 'igdeg=%-4s ' "$ig"
  CAGRA_IGDEG=$ig "$TEST" "$BRIDGE" "$N" "$DIM" 100 10
done

echo ""
echo "=== repeat baseline 3x (noise estimate) ==="
for i in 1 2 3; do
  printf 'rep%s ' "$i"
  "$TEST" "$BRIDGE" "$N" "$DIM" 100 10
done
echo ""
echo "DONE"

#!/usr/bin/env bash
# Experiment: compare cuVS CAGRA graph-build algorithms at evaluation scale.
#
# Context: two earlier submissions were rejected by the hard gate
# (hidden250k:recall@10/100_baseline_delta) because they lowered
# nn_descent_niter to trade recall for speed, which section 11 of the contest
# rules explicitly forbids. This experiment therefore keeps every build
# parameter at the frozen baseline value and only varies the build *algorithm*,
# which is an organizer-provided implementation.
#
# Measurement: 250K x 256 float32 with an exact top-100 ground truth, so both
# recall@10 and recall@100 are available and the drop versus the baseline
# (NN_DESCENT, i.e. the frozen baseline) can be checked directly.
#
# Usage: bash exp_algo.sh <dataset_dir>
set -u

WORK=${WORK:-/workspace/work}
DATA=${1:-/workspace/work/d256}
SO=${SO:-$WORK/libalgo.so}
TEST=${TEST:-$WORK/bridge_test}

export LD_LIBRARY_PATH=$WORK:/seekdb_workspace/bridge:/opt/hipvs/lib:/opt/rocm/lib
export BASE_F32="$DATA/base.f32"
export QUERY_F32="$DATA/query.f32"
export GT_I32="$DATA/gt.i32"
export GT_Q=100
export GT_K=100

N=$(python3 -c "import os;print(os.path.getsize('$DATA/base.f32')//4//$(cat "$DATA/dim.txt"))")
DIM=$(cat "$DATA/dim.txt")
echo "=== dataset: $DATA  N=$N DIM=$DIM  (top-100 ground truth) ==="
echo ""

run() {  # run <label> <env assignments...>
  local label="$1"; shift
  env "$@" "$TEST" "$SO" "$N" "$DIM" 100 100 2>&1 \
    | grep -oE "BUILD_MS=[0-9.]+ .*RECALL10=[0-9.]+ RECALLK=[0-9.]+" \
    | sed "s/^/$label /"
}

echo "--- baseline: NN_DESCENT (algo=2), frozen parameters ---"
run "algo=2_NN_DESCENT" CAGRA_ALGO=2
echo ""
echo "--- candidates ---"
run "algo=0_AUTO" CAGRA_ALGO=0
run "algo=1_IVF_PQ" CAGRA_ALGO=1
run "algo=3_ITER" CAGRA_ALGO=3
echo ""
echo "--- repeat baseline for noise reference ---"
run "algo=2_NN_DESCENT" CAGRA_ALGO=2
run "algo=2_NN_DESCENT" CAGRA_ALGO=2
echo ""
echo "DONE"
echo ""
echo "Interpret against the algo=2 rows:"
echo "  BUILD_MS   lower is better"
echo "  RECALL10   must not drop more than 0.005 vs the algo=2 baseline"
echo "  RECALLK    must not drop more than 0.005 vs the algo=2 baseline"
echo "Keep at least 2x margin (target drop < 0.0025)."

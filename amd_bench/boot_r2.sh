#!/usr/bin/env bash
# Round-2 bootstrap for the AMD GPU instance (run from anywhere):
#   curl -fsSL "https://api.github.com/repos/ssenran-star/seekdb-/contents/amd_bench/boot_r2.sh?ref=amd-cagra-submit" \
#     | python3 -c "import sys,json,base64;open('boot_r2.sh','wb').write(base64.b64decode(json.load(sys.stdin)['content']))"
#   bash boot_r2.sh
#
# Pulls everything via api.github.com (raw.githubusercontent.com and git clone
# are blocked on the instance; the repo is PUBLIC so no token is needed),
# compiles, generates the 250K datasets once, then runs the decisive build
# matrix. After it finishes, run in /workspace/work:
#   bash exp_query.sh                # itopk/sw sweep
#   N=250000 DIM=256 bash qps_measure.sh
#   bash e2e_measure.sh
set -u
WORK=/workspace/work
REF=${REF:-amd-cagra-submit}
API=https://api.github.com/repos/ssenran-star/seekdb-/contents
mkdir -p "$WORK/hipvs_bridge"
cd "$WORK"

fetch() {  # fetch <api-path> <local-path> [force]
  local src="$1" dst="$2" force="${3:-}"
  if [ -z "$force" ] && [ -s "$dst" ]; then echo "have  $dst"; return 0; fi
  curl -fsSL "$API/$src?ref=$REF" \
    | python3 -c "import sys,json,base64;open('$dst','wb').write(base64.b64decode(json.load(sys.stdin)['content']))" \
    && echo "got   $dst" || { echo "FETCH_FAIL $src"; return 1; }
}

echo "=== 1. fetch bench tools (anonymous, repo is public) ==="
# scripts/sources always refresh: they change between rounds and a stale copy
# silently produces wrong experiments (only the heavy datasets are cached)
for f in exp_build_matrix.sh exp_query.sh exp_delta.sh exp_bf.sh probe_api.sh \
         exp_opt.sh opt_test.cpp \
         qps_measure.sh e2e_measure.sh start_seekdb.sh build_seekdb.sh \
         dump_hipvs_headers.sh \
         bridge_test.c bf_test.c query_bench.c gen_data.py seekdb_cuvs_bridge_algo.c; do
  fetch "amd_bench/$f" "$WORK/$f" force || exit 1
done
# bridge source always refreshes: it is the artifact that changes between rounds
for f in seekdb_cuvs_bridge.c seekdb_cuvs_bridge.h build.sh; do
  fetch "tools/hipvs_bridge/$f" "$WORK/hipvs_bridge/$f" force || exit 1
done
chmod +x "$WORK"/*.sh

echo ""
echo "=== 2. compile probe ==="
gcc -O2 -o "$WORK/bridge_test" "$WORK/bridge_test.c" -ldl && echo GCC_OK || { echo GCC_FAIL; exit 1; }
gcc -O2 -o "$WORK/query_bench" "$WORK/query_bench.c" -ldl && echo QGCC_OK

echo ""
echo "=== 3. optional: full build matrix (heavy; set RUN_MATRIX=1 to enable) ==="
if [ "${RUN_MATRIX:-0}" = "1" ]; then
  echo "    (generates d128/d256 250K datasets on first run; ~40-60 min)"
  bash "$WORK/exp_build_matrix.sh"
else
  echo "    skipped. run once per instance if needed:  bash $WORK/exp_build_matrix.sh"
  echo "    (datasets d128/d256 are generated on first use by any exp script)"
fi
echo ""
echo "=== next steps (in $WORK) ==="
echo "  bash /workspace/work/exp_bf.sh      # ★ brute-force exact kNN build (recall+time)"
echo "  bash /workspace/work/exp_delta.sh   # same-session frozen-baseline calibration"
echo "  bash /workspace/work/build_seekdb.sh   # build seekdb w/ cuVS (needed for SQL e2e, ~30-60 min)"
echo "  bash /workspace/work/start_seekdb.sh && bash /workspace/work/e2e_measure.sh && N=250000 DIM=256 bash /workspace/work/qps_measure.sh"

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

fetch() {  # fetch <api-path> <local-path>
  local src="$1" dst="$2"
  if [ -s "$dst" ]; then echo "have  $dst"; return 0; fi
  curl -fsSL "$API/$src?ref=$REF" \
    | python3 -c "import sys,json,base64;open('$dst','wb').write(base64.b64decode(json.load(sys.stdin)['content']))" \
    && echo "got   $dst" || { echo "FETCH_FAIL $src"; return 1; }
}

echo "=== 1. fetch bench tools (anonymous, repo is public) ==="
for f in exp_build_matrix.sh exp_query.sh qps_measure.sh e2e_measure.sh \
         bridge_test.c query_bench.c gen_data.py seekdb_cuvs_bridge_algo.c; do
  fetch "amd_bench/$f" "$WORK/$f" || exit 1
done
for f in seekdb_cuvs_bridge.c seekdb_cuvs_bridge.h build.sh; do
  fetch "tools/hipvs_bridge/$f" "$WORK/hipvs_bridge/$f" || exit 1
done
chmod +x "$WORK"/*.sh

echo ""
echo "=== 2. compile probe ==="
gcc -O2 -o "$WORK/bridge_test" "$WORK/bridge_test.c" -ldl && echo GCC_OK || { echo GCC_FAIL; exit 1; }
gcc -O2 -o "$WORK/query_bench" "$WORK/query_bench.c" -ldl && echo QGCC_OK

echo ""
echo "=== 3. run the decisive build matrix (nd early-stop vs legacy) ==="
echo "    (generates d128/d256 250K datasets on first run; ~40-60 min)"
bash "$WORK/exp_build_matrix.sh"
echo ""
echo "=== next steps (in $WORK) ==="
echo "  bash exp_query.sh                # itopk/sw sweep (recall + per-query latency)"
echo "  N=250000 DIM=256 bash qps_measure.sh   # end-to-end 4096-query QPS"
echo "  bash e2e_measure.sh              # CREATE INDEX window / F breakdown"

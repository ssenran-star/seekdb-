#!/usr/bin/env bash
# run_next.sh v2 (2026-11-17 15:05) -- current battle chain
# refresh tools/bridge -> delta validation -> fp16 -> 250K e2e (F/B split)
# -> 250K qps. Every stage prints a banner.
#
# Usage (two lines):
#   curl -fsSL "https://api.github.com/repos/ssenran-star/seekdb-/contents/amd_bench/run_next.sh?ref=amd-cagra-submit" \
#     | python3 -c "import sys,json,base64;open('run_next.sh','wb').write(base64.b64decode(json.load(sys.stdin)['content']))"
#   bash /workspace/work/run_next.sh
set -u
echo "run_next.sh v2 (2026-11-17 15:05)"
WORK=/workspace/work
mkdir -p "$WORK"; cd "$WORK"

echo ""
echo "########## 0. refresh tools + bridge sources ##########"
if [ -f "$WORK/boot_r2.sh" ]; then
  bash "$WORK/boot_r2.sh" || true
else
  curl -fsSL "https://api.github.com/repos/ssenran-star/seekdb-/contents/amd_bench/boot_r2.sh?ref=amd-cagra-submit" \
    | python3 -c "import sys,json,base64;open('boot_r2.sh','wb').write(base64.b64decode(json.load(sys.stdin)['content']))"
  bash "$WORK/boot_r2.sh" || true
fi
curl -fsSL "https://api.github.com/repos/ssenran-star/seekdb-/contents/amd_bench/run_next.sh?ref=amd-cagra-submit" \
  | python3 -c "import sys,json,base64;open('run_next.sh','wb').write(base64.b64decode(json.load(sys.stdin)['content']))" || true

echo ""
echo "########## A. exp_delta (bf go/no-go + bridge FP16 revalidation) ##########"
RUNS=2 bash "$WORK/exp_delta.sh" || echo "[run_next] exp_delta FAILED -- continuing"

echo ""
echo "########## B. exp_bf (fp16 vs fp32 distance pass) ##########"
bash "$WORK/exp_bf.sh" || echo "[run_next] exp_bf FAILED -- continuing"

echo ""
echo "########## C. e2e 250K x 256 (real F/B split) ##########"
N=250000 DIM=256 bash "$WORK/e2e_measure.sh" || echo "[run_next] e2e FAILED -- continuing"

echo ""
echo "########## D. qps 250K x 256 (query ratio) ##########"
N=250000 DIM=256 bash "$WORK/qps_measure.sh" || echo "[run_next] qps FAILED -- continuing"

echo ""
echo "########## DONE -- paste this whole log back ##########"

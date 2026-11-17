#!/usr/bin/env bash
# One-shot chain: refresh tools -> exp_bf -> build_seekdb -> e2e + qps.
# Every stage prints a version banner; if you do not see "exp_bf.sh v3",
# a stale copy is being executed.
#
# Usage (two lines):
#   curl -fsSL "https://api.github.com/repos/ssenran-star/seekdb-/contents/amd_bench/run_next.sh?ref=amd-cagra-submit" \
#     | python3 -c "import sys,json,base64;open('run_next.sh','wb').write(base64.b64decode(json.load(sys.stdin)['content']))"
#   bash /workspace/work/run_next.sh
set -u
echo "run_next.sh v1 (2026-11-16 22:40)"
WORK=/workspace/work
mkdir -p "$WORK"; cd "$WORK"

echo ""
echo "########## 0. refresh tools ##########"
if [ -f "$WORK/boot_r2.sh" ]; then
  bash "$WORK/boot_r2.sh" || true
else
  curl -fsSL "https://api.github.com/repos/ssenran-star/seekdb-/contents/amd_bench/boot_r2.sh?ref=amd-cagra-submit" \
    | python3 -c "import sys,json,base64;open('boot_r2.sh','wb').write(base64.b64decode(json.load(sys.stdin)['content']))"
  bash "$WORK/boot_r2.sh" || true
fi
# boot_r2 caches nothing now (force refresh), but re-fetch this chain script too
curl -fsSL "https://api.github.com/repos/ssenran-star/seekdb-/contents/amd_bench/run_next.sh?ref=amd-cagra-submit" \
  | python3 -c "import sys,json,base64;open('run_next.sh','wb').write(base64.b64decode(json.load(sys.stdin)['content']))" || true

echo ""
echo "########## A. exp_bf (brute-force build probe) ##########"
bash "$WORK/exp_bf.sh" || echo "[run_next] exp_bf FAILED -- continuing"

echo ""
echo "########## B. build_seekdb (30-60 min) ##########"
if bash "$WORK/build_seekdb.sh"; then
  echo ""
  echo "########## C. e2e + qps ##########"
  bash "$WORK/start_seekdb.sh" || true
  bash "$WORK/e2e_measure.sh" || true
  N=250000 DIM=256 bash "$WORK/qps_measure.sh" || true
else
  echo "[run_next] build_seekdb FAILED -- skipping SQL e2e"
fi

echo ""
echo "########## DONE -- paste this whole log back ##########"

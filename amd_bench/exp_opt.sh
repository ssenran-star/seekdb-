#!/usr/bin/env bash
# exp_opt.sh v1 (2026-11-17 09:40) -- RobustPrune probe for the bf pipeline
# bf exact kNN -> cagra::helpers::optimize -> FromArgs -> recall, with a
# no-optimize reference in the same run. The gate for adopting optimize:
#   OPT_RK >= NOOPT_RK + 0.002   (visible recall win)
# and OPT_MS should stay under ~3s for 250K (it runs inside the timed build).
set -u
echo "exp_opt.sh v1 (2026-11-17 09:40)"
WORK=${WORK:-/workspace/work}
cd "$WORK"
export LD_LIBRARY_PATH=$WORK:/opt/hipvs/lib:/opt/rocm/lib

CXX="${CXX:-}"
if [ -z "$CXX" ]; then
  CXX=$(ls /opt/rocm*/lib/llvm/bin/clang++ 2>/dev/null | head -1)
fi
[ -n "$CXX" ] || CXX=clang++
echo "CXX=$CXX"

if [ ! -f /opt/hipvs/include/raft/core/resources.hpp ] && \
   ! echo | "$CXX" -x c++ - -E -include raft/core/resources.hpp >/dev/null 2>&1; then
  # cheap existence check failed -- still try compiling, the error is the signal
  echo "NOTE: raft headers not found in /opt/hipvs/include/raft; compile will tell"
fi

echo "=== compile opt_test ==="
"$CXX" -O2 -std=c++17 -o opt_test opt_test.cpp \
  -I/opt/hipvs/include -I/opt/rocm/include \
  -L/opt/hipvs/lib -lcuvs_c -lcuvs -Wl,-rpath,/opt/hipvs/lib -ldl 2>opt_compile.log
rc=$?
if [ $rc -ne 0 ]; then
  echo "COMPILE_FAIL rc=$rc -- last 30 lines:"
  tail -30 opt_compile.log
  if grep -q "raft/core" opt_compile.log; then
    echo "=> RAFT_HEADERS_MISSING: run dump_hipvs_headers.sh and paste section 7"
  fi
  exit 1
fi
echo "COMPILE_OK"

for dim in 128 256; do
  data=$WORK/d$dim
  [ -f "$data/base.f32" ] || { echo "missing $data (run exp_build_matrix first)"; exit 1; }
  echo ""
  echo "==================== dim=$dim ===================="
  for run in 1 2 3; do
    env BASE_F32="$data/base.f32" QUERY_F32="$data/query.f32" GT_I32="$data/gt.i32" \
        GT_Q=100 GT_K=100 \
        timeout --kill-after=30 420 ./opt_test 250000 "$dim" 256 128 2>&1 | tail -2
    echo "  (exit=$?)"
  done
done

echo ""
echo "=== reading the output ==="
echo "SELF_HIT ~1.0000 -> neighbour dtype correct"
echo "adopt optimize if OPT_RK >= NOOPT_RK + 0.002 and OPT_MS is small;"
echo "then build_bf uses pruned graph (delta vs frozen should drop well under 0.003)"
echo "DONE"

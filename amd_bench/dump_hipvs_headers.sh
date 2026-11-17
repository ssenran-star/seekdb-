#!/usr/bin/env bash
# dump_hipvs_headers.sh v1 (2026-11-17 09:15) -- signatures for the optimize/RobustPrune path
# Prints the hipVS headers needed to call cagra::optimize (the prune stage our
# FromArgs pipeline currently skips) plus resource/mdspan interop helpers.
set -u
echo "dump_hipvs_headers.sh v1 (2026-11-17 09:15)"
INC="${HIPVS_INC:-/opt/hipvs/include}"
echo "=== 0. layout ==="
ls "$INC" 2>/dev/null
ls "$INC/cuvs" 2>/dev/null
ls "$INC/cuvs/neighbors" 2>/dev/null | head -30
ls "$INC/cuvs/core" 2>/dev/null
ls "$INC/raft" 2>/dev/null | head -5
ls "$INC/raft/core" 2>/dev/null | head -10

echo ""
echo "=== 1. cagra_optimize.hpp ==="
cat "$INC/cuvs/neighbors/cagra_optimize.hpp" 2>/dev/null | head -120

echo ""
echo "=== 2. cagra_index_wrapper.hpp ==="
cat "$INC/cuvs/neighbors/cagra_index_wrapper.hpp" 2>/dev/null | head -120

echo ""
echo "=== 3. index_wrappers.hpp ==="
cat "$INC/cuvs/neighbors/index_wrappers.hpp" 2>/dev/null | head -120

echo ""
echo "=== 4. core headers (resource + mdspan interop) ==="
for f in "$INC/cuvs/core"/*.h "$INC/cuvs/core"/*.hpp "$INC/cuvs/core/detail"/*.hpp; do
  [ -f "$f" ] || continue
  echo "--- $f ---"
  grep -n "resources\|to_raft\|from_dlpack\|mdspan\|namespace" "$f" | head -25
done

echo ""
echo "=== 5. exported optimize/related symbols in libcuvs_c.so / libcuvs.so ==="
for so in /opt/hipvs/lib/libcuvs_c.so /opt/hipvs/lib/libcuvs.so; do
  echo "--- $so ---"
  nm -D --defined-only "$so" 2>/dev/null | grep -i "optimize\|prune\|IndexFromArgs\|GetGraph\|GetDataset" | head -20
done

echo ""
echo "=== 6. cagra.h FromArgs + optimize hints ==="
grep -n "optimize\|FromArgs\|prune" "$INC/cuvs/neighbors/cagra.h" 2>/dev/null | head -15
echo "DONE"

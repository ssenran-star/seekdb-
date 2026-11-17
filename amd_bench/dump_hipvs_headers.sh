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

echo ""
echo "=== 7. raft headers + resources ABI (needed by the optimize wrapper) ==="
echo "--- raft header search ---"
find /opt/hipvs /opt/rocm /usr/include /usr/local/include -maxdepth 6 \
  \( -name 'host_mdspan.hpp' -o -name 'device_mdspan.hpp' -o -path '*raft/core/resources.hpp' \) \
  2>/dev/null | head -10
ls "$INC/raft" 2>/dev/null | head -5
ls /opt/hipvs/ 2>/dev/null
echo "--- cuvsResources_t typedef ---"
grep -n "cuvsResources_t\|typedef" "$INC/cuvs/core/c_api.h" | head -12
echo "--- raft::resources ctor/dtor exported from libcuvs.so? ---"
nm -D /opt/hipvs/lib/libcuvs.so 2>/dev/null | grep -E 'raft9resourcesC|raft16device_resourcesC|raft9resourcesD' | head -8
echo "--- make_host_matrix_view / helpers::optimize mangled ---"
nm -D /opt/hipvs/lib/libcuvs.so 2>/dev/null | grep -E 'make_host_matrix_view|helpers8optimize' | head -6
echo "DONE"

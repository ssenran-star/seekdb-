#!/usr/bin/env bash
# End-to-end query QPS through SQL: 4096 single-row approximate queries,
# matching the contest's measurement shape ("nq=4096 是 4096 条单行近似查询 SQL").
#
# Baseline reference (frozen V5, AMD eval box):
#   SIFT250K  top-10 ~147 QPS / top-100 ~143 QPS
#   Hidden250K top-10 ~87 QPS / top-100 ~85 QPS
# Query score = 2 * clamp(ln(Q_geo / Q_base_geo) / ln(1.5), 0, 1);
# the geo-mean is over (top-10 ratio, top-100 ratio) per dataset.
#
# Usage:
#   bash qps_measure.sh                # 10k x 128 (quick check)
#   N=250000 DIM=256 bash qps_measure.sh
set -euo pipefail
MYSQL="mysql -h127.0.0.1 -P2881 -uroot -A -N -B"
WORK=${WORK:-/workspace/work}
N=${N:-10000}
DIM=${DIM:-128}
NQ=${NQ:-4096}
DB=${DB:-hipvs_gate}

BASE=/seekdb_workspace/datasets/base.f32
QUERY=/seekdb_workspace/datasets/query.f32
[ "$DIM" = 256 ] && { BASE=$WORK/d256/base.f32; QUERY=$WORK/d256/query.f32; }

echo "=== 0. check index exists in $DB.items (n=$N dim=$DIM) ==="
$MYSQL -e "use $DB; show index from items;" | head -5 || {
  echo "run e2e_measure.sh first (creates table + loads data + builds index)"; exit 1; }

echo "=== 1. generate $NQ single-row query SQL (cycle 100 probe vectors) ==="
python3 - "$QUERY" "$DIM" "$NQ" "$DB" > "$WORK/q.sql" <<'PY'
import struct, sys
qry_path, dim, nq, db = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
raw = open(qry_path, "rb").read()
nprobe = len(raw) // (4 * dim)
probes = struct.unpack("<%df" % (nprobe * dim), raw)
print("use %s;" % db)
for i in range(nq):
    p = (i % nprobe) * dim
    vals = probes[p:p + dim]
    print("SELECT id FROM items ORDER BY l2_distance(v, '[%s]') APPROXIMATE LIMIT %d;"
          % (",".join("%.7f" % v for v in vals), int(sys.argv[5]) if len(sys.argv) > 5 else 10))
PY
echo "SQL lines: $(wc -l < "$WORK/q.sql")"

echo "=== 2. warmup (200 queries, discarded) ==="
head -n 201 "$WORK/q.sql" > "$WORK/q_warm.sql"   # +1 for 'use db'
time $MYSQL < "$WORK/q_warm.sql" > /dev/null

for K in 10 100; do
  python3 - "$QUERY" "$DIM" "$NQ" "$DB" "$K" > "$WORK/q$K.sql" <<'PY'
import struct, sys
qry_path, dim, nq, db, k = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4], int(sys.argv[5])
raw = open(qry_path, "rb").read()
nprobe = len(raw) // (4 * dim)
probes = struct.unpack("<%df" % (nprobe * dim), raw)
print("use %s;" % db)
for i in range(nq):
    p = (i % nprobe) * dim
    vals = probes[p:p + dim]
    print("SELECT id FROM items ORDER BY l2_distance(v, '[%s]') APPROXIMATE LIMIT %d;"
          % (",".join("%.7f" % v for v in vals), k))
PY
  echo "=== 3. Top-$K: $NQ queries ==="
  start=$(date +%s.%N)
  $MYSQL < "$WORK/q$K.sql" | grep -c '[0-9]' > "$WORK/q$K.rowcount" || true
  end=$(date +%s.%N)
  python3 - "$start" "$end" "$NQ" "$K" <<'PY'
import sys
s, e, nq, k = float(sys.argv[1]), float(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
el = e - s
print("Top-%s: total=%.3fs  QPS=%.2f  avg=%.3f ms/query  rows=%s"
      % (k, el, nq / el, el * 1000.0 / nq, open("/workspace/work/q%s.rowcount" % k).read().strip()))
PY
done
echo "=== DONE (compare against baseline QPS above; ratio >=1.5 for full query score) ==="

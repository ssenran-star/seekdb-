#!/usr/bin/env bash
# End-to-end timing to isolate the fixed overhead F in the CREATE INDEX window.
# F = table-scan + H2D + snapshot + first-query, i.e. everything except the
# pure GPU graph build (which bridge_test measures directly).
set -euo pipefail

MYSQL="mysql -h127.0.0.1 -P2881 -uroot -A -N -B"
BASE=/seekdb_workspace/datasets/base.f32
QUERY=/seekdb_workspace/datasets/query.f32
N=${N:-10000}
DIM=128
WORK=/workspace/work

echo "=== 1. generate $N-row insert SQL from base.f32 ==="
python3 - "$BASE" "$N" "$DIM" > "$WORK/items.sql" <<'PY'
import struct, sys
base_path, n, dim = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
data = struct.unpack("<%df" % (n * dim), open(base_path, "rb").read())
chunk = 250
def vec(i):
    vals = data[i * dim:(i + 1) * dim]
    return "[" + ",".join("%.7f" % v for v in vals) + "]"
print("use hipvs_gate;")
for s in range(0, n, chunk):
    rows = ["(%d,'%s')" % (i, vec(i)) for i in range(s, min(s + chunk, n))]
    print("insert into items values " + ",".join(rows) + ";")
PY
echo "SQL lines: $(wc -l < "$WORK/items.sql"), bytes: $(wc -c < "$WORK/items.sql")"

echo ""
echo "=== 2. create table (no index) ==="
$MYSQL -e "alter system set ob_vector_memory_limit_percentage=30; create database if not exists hipvs_gate; use hipvs_gate; drop table if exists items; create table items(id int primary key, v vector(128));"

echo ""
echo "=== 3. load data ($N rows) ==="
time $MYSQL < "$WORK/items.sql"

echo ""
echo "=== 4. CREATE VECTOR INDEX (timer start) ==="
time $MYSQL -e "use hipvs_gate; CREATE VECTOR INDEX idx_items ON items(v) WITH (distance=l2, type=hnsw, lib=cuvs, m=16, ef_construction=200, ef_search=64);"

echo ""
echo "=== 5. first query (timer end) ==="
Q=$(python3 -c "import struct; d=struct.unpack('<128f', open('$QUERY','rb').read(512)); print('['+','.join('%.7f'%v for v in d)+']')")
time $MYSQL -e "use hipvs_gate; SELECT id FROM items ORDER BY l2_distance(v, '$Q') APPROXIMATE LIMIT 10;"
echo "first-query result (expect 10 ids):"
$MYSQL -e "use hipvs_gate; SELECT id FROM items ORDER BY l2_distance(v, '$Q') APPROXIMATE LIMIT 10;"

echo ""
echo "=== DONE ==="

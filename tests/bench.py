import json
import random
import subprocess
import sys
import tempfile
import time

BIN = sys.argv[1] if len(sys.argv) > 1 else "./build/minivectordb"
N = 10000
DIM = 128
QUERIES = 1000
K = 10

random.seed(42)


class Proc:
    def __init__(self, data):
        self.p = subprocess.Popen(
            [BIN, "--data-dir", data],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            bufsize=1,
        )
        assert json.loads(self.p.stdout.readline()) == {"type": "ready"}
        self.id = 0

    def req(self, sql, mode="exact"):
        self.id += 1
        self.p.stdin.write(
            json.dumps({"id": self.id, "op": "execute", "sql": sql, "search_mode": mode}) + "\n"
        )
        self.p.stdin.flush()
        r = json.loads(self.p.stdout.readline())
        assert r.get("ok"), r
        return r

    def close(self):
        self.id += 1
        self.p.stdin.write(json.dumps({"id": self.id, "op": "shutdown"}) + "\n")
        self.p.stdin.flush()
        assert json.loads(self.p.stdout.readline())["ok"]
        self.p.wait(timeout=30)


def vec_str(v):
    return "[" + ",".join(f"{x:.6f}" for x in v) + "]"


rows = []
for i in range(N):
    v = [random.uniform(-1, 1) for _ in range(DIM)]
    rows.append((i, f"c{i % 10}", v))

queries = []
for q in range(QUERIES):
    v = [random.uniform(-1, 1) for _ in range(DIM)]
    filt = f" WHERE category='c{q % 10}'" if q % 2 == 0 else ""
    queries.append((v, filt))

with tempfile.TemporaryDirectory() as d:
    p = Proc(d)
    p.req(f"CREATE TABLE bench (id BIGINT PRIMARY KEY, category VARCHAR(8), embedding VECTOR({DIM}));")
    t0 = time.perf_counter()
    for b in range(N // 100):
        chunk = rows[b * 100 : (b + 1) * 100]
        values = ",".join(f"({i},'{c}','{vec_str(v)}')" for i, c, v in chunk)
        p.req(f"INSERT INTO bench VALUES {values};")
    t_write = time.perf_counter() - t0

    t0 = time.perf_counter()
    exact_ids = []
    for v, filt in queries:
        r = p.req(
            f"SELECT id, L2_DISTANCE(embedding,'{vec_str(v)}') AS distance FROM bench{filt} ORDER BY distance LIMIT {K};"
        )
        exact_ids.append([row[0] for row in r["rows"]])
    t_query = time.perf_counter() - t0

    t0 = time.perf_counter()
    ann_ids = []
    for v, filt in queries:
        r = p.req(
            f"SELECT id, L2_DISTANCE(embedding,'{vec_str(v)}') AS distance FROM bench{filt} ORDER BY distance LIMIT {K};",
            mode="ann",
        )
        ann_ids.append([row[0] for row in r["rows"]])
    t_ann = time.perf_counter() - t0

    v0, filt0 = queries[0]
    cand = [(i, c, v) for i, c, v in rows if not filt0 or c == filt0.strip().split("'")[1]]
    truth = sorted(cand, key=lambda r: (sum((a - b) ** 2 for a, b in zip(r[2], v0)), r[0]))[:K]
    assert [r[0] for r in truth] == exact_ids[0], "exact result differs from brute force"

    recall = sum(len(set(a) & set(b)) / K for a, b in zip(exact_ids, ann_ids)) / QUERIES
    p.req("CREATE VECTOR INDEX bench_vec ON bench(embedding);")
    t0 = time.perf_counter()
    ann2_ids = []
    for v, filt in queries[:200]:
        r = p.req(
            f"SELECT id, L2_DISTANCE(embedding,'{vec_str(v)}') AS distance FROM bench{filt} ORDER BY distance LIMIT {K};",
            mode="ann",
        )
        ann2_ids.append([row[0] for row in r["rows"]])
    t_ann_indexed = time.perf_counter() - t0
    recall2 = sum(len(set(a) & set(b)) / K for a, b in zip(exact_ids[:200], ann2_ids)) / 200

    p.close()
    p = Proc(d)
    r = p.req("SELECT id FROM bench ORDER BY id LIMIT 10001;")
    assert len(r["rows"]) == N, "persistence count mismatch"
    p.close()

print(f"T_write={t_write:.3f}s T_query_exact={t_query:.3f}s T_ann_noindex={t_ann:.3f}s")
print(f"recall_ann_noindex={recall:.4f} recall_ann_indexed={recall2:.4f} T_ann_indexed_200q={t_ann_indexed:.3f}s")
print("BENCH OK")
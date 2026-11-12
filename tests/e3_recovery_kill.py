import json
import subprocess
import sys
import tempfile
import time

BIN = sys.argv[1] if len(sys.argv) > 1 else "./build/minivectordb"
N = 20000
DIM = 16


class Proc:
    def __init__(self, data, wait_ready=True):
        self.p = subprocess.Popen(
            [BIN, "--data-dir", data],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            bufsize=1,
        )
        if wait_ready:
            assert json.loads(self.p.stdout.readline()) == {"type": "ready"}
        self.id = 0

    def req(self, sql):
        self.id += 1
        self.p.stdin.write(
            json.dumps({"id": self.id, "op": "execute", "sql": sql, "search_mode": "exact"}) + "\n"
        )
        self.p.stdin.flush()
        r = json.loads(self.p.stdout.readline())
        assert r.get("ok"), r
        return r

    def kill9(self):
        self.p.kill()
        self.p.wait()

    def close(self):
        self.id += 1
        self.p.stdin.write(json.dumps({"id": self.id, "op": "shutdown"}) + "\n")
        self.p.stdin.flush()
        self.p.wait(timeout=30)


with tempfile.TemporaryDirectory() as d:
    p = Proc(d)
    p.req(f"CREATE TABLE big (id BIGINT PRIMARY KEY, e VECTOR({DIM}));")
    t0 = time.perf_counter()
    for b in range(N // 100):
        vals = ",".join(
            f"({i},'[" + ",".join(f"{((i * 7 + j * 13) % 100) / 10.0:.1f}" for j in range(DIM)) + "]')"
            for i in range(b * 100, (b + 1) * 100)
        )
        p.req(f"INSERT INTO big VALUES {vals};")
    p.req("CREATE VECTOR INDEX bi ON big(e);")
    print(f"loaded {N} rows + index in {time.perf_counter()-t0:.2f}s")
    p.close()

    for attempt in range(1, 4):
        p = Proc(d, wait_ready=False)
        time.sleep(0.15)
        p.kill9()
        print(f"kill during recovery #{attempt} (150ms after start, before ready)")

    t0 = time.perf_counter()
    p = Proc(d)
    ready_time = time.perf_counter() - t0
    r = p.req("SELECT id FROM big ORDER BY id LIMIT 20001;")
    n = len(r["rows"])
    first, last = r["rows"][0][0], r["rows"][-1][0]
    print(f"final recovery ready in {ready_time:.2f}s; rows={n} first={first} last={last}")
    assert n == N and first == 0 and last == N - 1, "DATA LOSS after repeated recovery kills"
    r = p.req("SELECT id, L2_DISTANCE(e,'[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]') AS d FROM big ORDER BY d LIMIT 3;")
    print("top3 after recovery:", json.dumps(r["rows"]))
    p.close()
print("E3 PASS: repeated mid-recovery kills lose no committed data")
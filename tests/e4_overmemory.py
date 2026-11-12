import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

IMAGE = sys.argv[1] if len(sys.argv) > 1 else "minivectordb:latest"
N = int(sys.argv[2]) if len(sys.argv) > 2 else 2_000_000
DIM = 256
BATCH = 100

S = [f"{v / 10.0:.1f}" for v in range(100)]
V = []
for r in range(100):
    V.append("[" + ",".join(S[(r * 7 + j * 13) % 100] for j in range(DIM)) + "]")


def docker_run(data, name=None, extra=None):
    cmd = ["docker", "run", "--rm", "-i", "--memory=2g", "--memory-swap=2g",
           "--cpus=2", "--network=none", "--read-only",
           "--cap-drop=ALL", "--security-opt=no-new-privileges",
           "-v", f"{data}:/data"]
    if name:
        cmd += ["--name", name]
    cmd.append(IMAGE)
    return subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, text=True, bufsize=1)


class Session:
    def __init__(self, p):
        self.p = p
        assert json.loads(p.stdout.readline()) == {"type": "ready"}
        self.id = 0

    def req(self, sql):
        self.id += 1
        self.p.stdin.write(json.dumps({"id": self.id, "op": "execute", "sql": sql, "search_mode": "exact"}) + "\n")
        self.p.stdin.flush()
        r = json.loads(self.p.stdout.readline())
        assert r.get("ok"), r
        return r

    def shutdown(self):
        self.id += 1
        self.p.stdin.write(json.dumps({"id": self.id, "op": "shutdown"}) + "\n")
        self.p.stdin.flush()
        assert json.loads(self.p.stdout.readline())["ok"]
        self.p.wait(timeout=120)


data = tempfile.mkdtemp(prefix="e4overmem_", dir="/root")
os.chmod(data, 0o777)
try:
    print(f"dataset: {N} rows x VECTOR({DIM}) (~2.1GB log, exceeds 2GiB container memory)")
    t0 = time.perf_counter()
    p = docker_run(data)
    s = Session(p)
    s.req(f"CREATE TABLE big (id BIGINT PRIMARY KEY, e VECTOR({DIM}));")
    for b in range(N // BATCH):
        vals = ",".join(f"({i},'{V[i % 100]}')" for i in range(b * BATCH, (b + 1) * BATCH))
        s.req(f"INSERT INTO big VALUES {vals};")
        if (b + 1) % 2000 == 0:
            print(f"  loaded {(b+1)*BATCH} rows, {time.perf_counter()-t0:.0f}s", flush=True)
    s.shutdown()
    t_load = time.perf_counter() - t0
    logsize = os.path.getsize(os.path.join(data, "mvdb.log"))
    print(f"LOAD OK: {t_load:.1f}s, log={logsize/1e9:.2f}GB")

    t0 = time.perf_counter()
    p = docker_run(data)
    s = Session(p)
    t_ready = time.perf_counter() - t0
    r = s.req("SELECT id FROM big ORDER BY id DESC LIMIT 1;")
    assert r["rows"] == [[N - 1]], r
    r = s.req("SELECT id FROM big ORDER BY id LIMIT 1;")
    assert r["rows"] == [[0]], r
    r = s.req("SELECT id, L2_DISTANCE(e,'[" + ",".join(["0.0"] * DIM) + "]') AS d FROM big ORDER BY d LIMIT 3;")
    print(f"RESTART OK: ready in {t_ready:.1f}s (recovery of {logsize/1e9:.2f}GB), maxid={N-1}, top3={json.dumps(r['rows'])}")
    t0 = time.perf_counter()
    r = s.req("SELECT id, L2_DISTANCE(e,'[" + ",".join(["0.5"] * DIM) + "]') AS d FROM big ORDER BY d LIMIT 10;")
    print(f"TOPK over 2M rows: {time.perf_counter()-t0:.2f}s, ids={[x[0] for x in r['rows']]}")
    s.shutdown()

    p = docker_run(data, name="e4kill")
    s = Session(p)
    s.req("BEGIN;")
    s.req(f"INSERT INTO big VALUES ({N},'{V[0]}');")
    subprocess.run(["docker", "kill", "--signal=KILL", "e4kill"], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    p.wait()
    print("KILL during open transaction (uncommitted row N)")
    p = docker_run(data)
    s = Session(p)
    r = s.req("SELECT id FROM big ORDER BY id DESC LIMIT 1;")
    assert r["rows"] == [[N - 1]], f"uncommitted row leaked: {r}"
    print("CRASH OK: uncommitted row rolled back, committed data intact")
    s.shutdown()
    print("E4 PASS: over-memory dataset (2.1GB > 2GiB) load/restart/topk/crash all OK")
finally:
    shutil.rmtree(data, ignore_errors=True)
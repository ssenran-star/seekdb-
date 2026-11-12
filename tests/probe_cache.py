import json
import os
import subprocess
import sys
import tempfile

BIN = sys.argv[1] if len(sys.argv) > 1 else "./build/minivectordb"
N = 300000
DIM = 256

d = tempfile.mkdtemp(prefix="cacheprobe_", dir="/root")
errf = open("/root/probe_stderr.log", "w")
p = subprocess.Popen([BIN, "--data-dir", d], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=errf, text=True, bufsize=1)
assert json.loads(p.stdout.readline()) == {"type": "ready"}
cid = 0


def req(sql):
    global cid
    cid += 1
    p.stdin.write(json.dumps({"id": cid, "op": "execute", "sql": sql, "search_mode": "exact"}) + "\n")
    p.stdin.flush()
    r = json.loads(p.stdout.readline())
    assert r.get("ok"), r


req(f"CREATE TABLE t (id BIGINT PRIMARY KEY, e VECTOR({DIM}));")
S = [f"{v / 10.0:.1f}" for v in range(100)]
V = "[" + ",".join(S[(j * 13) % 100] for j in range(DIM)) + "]"
for b in range(N // 100):
    vals = ",".join(f"({i},'{V}')" for i in range(b * 100, (b + 1) * 100))
    req(f"INSERT INTO t VALUES {vals};")
cid += 1
p.stdin.write(json.dumps({"id": cid, "op": "shutdown"}) + "\n")
p.stdin.flush()
p.wait()
errf.close()
log = os.path.join(d, "mvdb.log")
print("log size:", os.path.getsize(log))
subprocess.run(["fincore", log])
print("--- stderr ---")
print(open("/root/probe_stderr.log").read()[:2000])
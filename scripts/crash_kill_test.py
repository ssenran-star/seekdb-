"""Crash-durability check: SIGKILL (Windows: TerminateProcess) the server with
an open transaction; on restart committed rows must survive and the open
transaction's rows must be gone."""
import json, os, subprocess, sys, tempfile, shutil

BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "minivectordb/build/minivectordb")
data = tempfile.mkdtemp(prefix="mvdb-kill-")
env = dict(os.environ, VECTORDB_DATA_DIR=data)

def start():
    p = subprocess.Popen([BIN], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL, env=env)
    ready = p.stdout.readline()
    assert ready.startswith(b'{"type":"ready"}'), ready
    return p

def send(p, obj):
    p.stdin.write((json.dumps(obj) + "\n").encode())
    p.stdin.flush()
    return json.loads(p.stdout.readline())

p = start()
send(p, {"id": 1, "op": "execute", "sql": "CREATE TABLE t (id BIGINT PRIMARY KEY, note VARCHAR(32), v VECTOR(4))"})
send(p, {"id": 2, "op": "execute", "sql": "CREATE VECTOR INDEX idx_t_v ON t (v)"})
# committed txn
send(p, {"id": 3, "op": "execute", "sql": "BEGIN"})
for i in range(3):
    send(p, {"id": 10 + i, "op": "execute", "sql":
        f"INSERT INTO t VALUES ({i+1}, 'row{i+1}', '[{i}.1, 0.2, 0.3, 0.4]')"})
send(p, {"id": 4, "op": "execute", "sql": "COMMIT"})
# autocommitted row
send(p, {"id": 5, "op": "execute", "sql": "INSERT INTO t VALUES (4, 'row4', '[4.0, 1.0, 2.0, 3.0]')"})
# open transaction: must NOT survive the kill
send(p, {"id": 6, "op": "execute", "sql": "BEGIN"})
send(p, {"id": 7, "op": "execute", "sql": "INSERT INTO t VALUES (5, 'row5-should-vanish', '[5.0, 5.0, 5.0, 5.0]')"})

# hard kill: no shutdown, no cleanup (Windows equivalent of SIGKILL)
p.kill()            # TerminateProcess
p.wait(timeout=10)

p = start()
r = send(p, {"id": 8, "op": "execute", "sql": "SELECT id, note FROM t ORDER BY id", "search_mode": "exact"})
ids = [row[0] for row in r.get("rows", [])]
send(p, {"id": 9, "op": "shutdown"})
p.wait(timeout=10)
shutil.rmtree(data, ignore_errors=True)

ok = ids == [1, 2, 3, 4]
print("rows after crash:", ids, "->", "PASS (committed survived, open txn rolled back)" if ok else "FAIL")
sys.exit(0 if ok else 1)

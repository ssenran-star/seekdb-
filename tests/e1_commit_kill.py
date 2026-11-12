import json
import subprocess
import sys
import tempfile

BIN = sys.argv[1] if len(sys.argv) > 1 else "./build/minivectordb"


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

    def req(self, sql):
        self.id += 1
        self.p.stdin.write(
            json.dumps({"id": self.id, "op": "execute", "sql": sql, "search_mode": "exact"}) + "\n"
        )
        self.p.stdin.flush()
        return json.loads(self.p.stdout.readline())

    def kill9(self):
        self.p.kill()
        self.p.wait()


with tempfile.TemporaryDirectory() as d:
    p = Proc(d)
    p.req("CREATE TABLE t (id BIGINT PRIMARY KEY, v VARCHAR(8));")
    p.req("INSERT INTO t VALUES (1,'a'),(2,'b');")
    p.req("BEGIN;")
    p.req("INSERT INTO t VALUES (3,'c'),(4,'d');")
    r = p.req("COMMIT;")
    print("COMMIT response received:", json.dumps(r))
    p.kill9()
    print("process killed with SIGKILL immediately after COMMIT response")

    p = Proc(d)
    r = p.req("SELECT id, v FROM t ORDER BY id;")
    print("after restart:", json.dumps(r))
    ids = [row[0] for row in r["rows"]]
    print("RESULT_A:", "ALL 4 ROWS SURVIVED" if ids == [1, 2, 3, 4] else f"UNEXPECTED {ids}")

    p.req("BEGIN;")
    p.req("INSERT INTO t VALUES (5,'e');")
    p.kill9()
    print("second kill: inside open transaction, no COMMIT sent")
    p = Proc(d)
    r = p.req("SELECT id FROM t ORDER BY id;")
    ids = [row[0] for row in r["rows"]]
    print("RESULT_B:", "UNCOMMITTED ROW 5 ROLLED BACK" if ids == [1, 2, 3, 4] else f"UNEXPECTED {ids}")
    p.id += 1
    p.p.stdin.write(json.dumps({"id": p.id, "op": "shutdown"}) + "\n")
    p.p.stdin.flush()
    p.p.wait(timeout=10)
print("E1 DONE")
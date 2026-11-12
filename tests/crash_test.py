import json
import os
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
        line = self.p.stdout.readline()
        assert json.loads(line) == {"type": "ready"}, line
        self.id = 0

    def req(self, sql=None, op="execute", mode="exact"):
        self.id += 1
        r = {"id": self.id, "op": op}
        if op == "execute":
            r["sql"] = sql
            r["search_mode"] = mode
        self.p.stdin.write(json.dumps(r) + "\n")
        self.p.stdin.flush()
        line = self.p.stdout.readline()
        assert line, "no response (process died?)"
        return json.loads(line)

    def kill(self):
        self.p.kill()
        self.p.wait()

    def close(self):
        r = self.req(op="shutdown")
        assert r["ok"], r
        self.p.wait(timeout=10)


def ids(p, sql, mode="exact"):
    r = p.req(sql, mode=mode)
    assert r["ok"], r
    return [row[0] for row in r["rows"]]


with tempfile.TemporaryDirectory() as d:
    p = Proc(d)
    p.req("CREATE TABLE t (id BIGINT PRIMARY KEY, v VARCHAR(8));")
    p.req("INSERT INTO t VALUES (1,'a'),(2,'b');")
    p.req("BEGIN;")
    p.req("INSERT INTO t VALUES (3,'c');")
    p.req("DELETE FROM t WHERE id=1;")
    p.kill()

    p = Proc(d)
    assert ids(p, "SELECT id FROM t ORDER BY id;") == [1, 2], "uncommitted not rolled back"
    p.kill()

    p = Proc(d)
    assert ids(p, "SELECT id FROM t ORDER BY id;") == [1, 2], "repeated recovery failed"
    p.req("BEGIN;")
    p.req("INSERT INTO t VALUES (4,'d');")
    r = p.req("COMMIT;")
    assert r["ok"], r
    p.kill()

    p = Proc(d)
    assert ids(p, "SELECT id FROM t ORDER BY id;") == [1, 2, 4], "committed lost after kill"
    p.req("BEGIN;")
    p.req("UPDATE t SET v='x' WHERE id=1;")
    p.req("ROLLBACK;")
    p.kill()

    p = Proc(d)
    r = p.req("SELECT v FROM t WHERE id=1;")
    assert r["rows"] == [["a"]], "rollback not durable"
    p.req("CREATE TABLE vec (id BIGINT PRIMARY KEY, e VECTOR(2));")
    p.req("INSERT INTO vec VALUES (1,'[3,4]'),(2,'[0,0]');")
    p.req("CREATE VECTOR INDEX vi ON vec(e);")
    p.req("BEGIN;")
    p.req("INSERT INTO vec VALUES (3,'[1,0]');")
    p.kill()

    p = Proc(d)
    assert ids(p, "SELECT id FROM vec ORDER BY id;") == [1, 2], "uncommitted vector row visible"
    r = p.req("SELECT id, L2_DISTANCE(e,'[0,0]') AS dist FROM vec ORDER BY dist LIMIT 5;", mode="ann")
    assert r["ok"] and [row[0] for row in r["rows"]] == [2, 1], "ann index rebuild failed"
    p.close()

    with open(os.path.join(d, "mvdb.log"), "ab") as f:
        f.write(b"\xff\xff\xff\xff torn tail garbage")
    p = Proc(d)
    assert ids(p, "SELECT id FROM t ORDER BY id;") == [1, 2, 4], "torn tail recovery failed"
    p.close()

print("CRASH TESTS PASS")
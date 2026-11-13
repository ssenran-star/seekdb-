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

    def req(self, sql, mode="exact"):
        self.id += 1
        self.p.stdin.write(
            json.dumps({"id": self.id, "op": "execute", "sql": sql, "search_mode": mode}) + "\n"
        )
        self.p.stdin.flush()
        return json.loads(self.p.stdout.readline())

    def ok(self, sql, mode="exact"):
        r = self.req(sql, mode)
        assert r.get("ok"), r
        return r

    def err(self, sql, code=None):
        r = self.req(sql)
        assert r.get("ok") is False, r
        if code:
            assert r["error"]["code"] == code, (code, r)
        return r

    def close(self):
        self.id += 1
        self.p.stdin.write(json.dumps({"id": self.id, "op": "shutdown"}) + "\n")
        self.p.stdin.flush()
        self.p.wait(timeout=10)


with tempfile.TemporaryDirectory(dir="/root") as d:
    p = Proc(d)
    p.ok("CREATE TABLE docs (\n  id BIGINT PRIMARY KEY,\n  category VARCHAR(8),\n  embedding VECTOR(2)\n);")
    print("multi-line CREATE TABLE: OK")
    p.ok("INSERT INTO docs VALUES (1,'a','[0,0]'),(2,'a','[3,4]'),(3,'b','[1,0]');")

    r = p.ok("SELECT ID, CATEGORY FROM docs ORDER BY id;")
    assert r["columns"] == ["id", "category"], r
    print("column names use definition case: OK")

    r = p.ok("SELECT id, L2_DISTANCE(embedding,'[0,0]') AS distance FROM docs ORDER BY distance DESC LIMIT 2;")
    assert [x[0] for x in r["rows"]] == [2, 3], r
    print("ORDER BY distance DESC: OK")

    r = p.ok("SELECT id, L2_DISTANCE(embedding,'[0,0]') AS distance FROM docs ORDER BY distance;")
    assert [x[0] for x in r["rows"]] == [1, 3, 2], r
    print("TopK without LIMIT returns all: OK")

    r = p.ok("SELECT id FROM docs ORDER BY id LIMIT 0;")
    assert r["rows"] == [] and r["columns"] == ["id"], r
    print("LIMIT 0: OK")

    p.ok("CREATE VECTOR INDEX di ON docs(embedding);")
    p.ok("BEGIN;")
    p.ok("INSERT INTO docs VALUES (4,'a','[0.5,0]');")
    r = p.ok("SELECT id, L2_DISTANCE(embedding,'[0,0]') AS distance FROM docs ORDER BY distance LIMIT 10;")
    assert [x[0] for x in r["rows"]] == [1, 4, 3, 2], r
    r = p.ok("SELECT id, L2_DISTANCE(embedding,'[0,0]') AS distance FROM docs ORDER BY distance LIMIT 10;", mode="ann")
    assert [x[0] for x in r["rows"]] == [1, 4, 3, 2], r
    print("in-txn vector search sees own insert (exact+ann): OK")
    p.ok("ROLLBACK;")
    r = p.ok("SELECT id FROM docs ORDER BY id;")
    assert [x[0] for x in r["rows"]] == [1, 2, 3], r

    p.err("UPDATE docs SET embedding='[1,2,3]' WHERE id>=1;", "dimension_mismatch")
    r = p.ok("SELECT id, embedding FROM docs ORDER BY id;")
    assert r["rows"] == [[1, [0.0, 0.0]], [2, [3.0, 4.0]], [3, [1.0, 0.0]]], r
    print("UPDATE atomicity on failure: OK")

    p.ok("BEGIN;")
    p.err("BEGIN;", "transaction_active")
    p.ok("ROLLBACK;")
    print("nested BEGIN rejected: OK")
    p.close()

print("E5 FORMS PASS")
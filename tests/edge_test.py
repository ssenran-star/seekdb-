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

    def err(self, sql, code=None, mode="exact"):
        r = self.req(sql, mode)
        assert r.get("ok") is False, r
        assert r["error"]["code"], r
        if code:
            assert r["error"]["code"] == code, (code, r)
        return r

    def close(self):
        self.id += 1
        self.p.stdin.write(json.dumps({"id": self.id, "op": "shutdown"}) + "\n")
        self.p.stdin.flush()
        assert json.loads(self.p.stdout.readline())["ok"]
        self.p.wait(timeout=10)


with tempfile.TemporaryDirectory() as d:
    p = Proc(d)
    p.ok("CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(4), e VECTOR(2));")
    p.err("INSERT INTO t VALUES (1,'ab','[NaN,0]');", "invalid_vector")
    p.err("INSERT INTO t VALUES (1,'ab','[Infinity,0]');", "invalid_vector")
    p.err("INSERT INTO t VALUES (1,'ab','[-Infinity,0]');", "invalid_vector")
    p.err("INSERT INTO t VALUES (1,'ab','[1e40,0]');", "invalid_vector")
    p.err("INSERT INTO t VALUES (1,'ab','[1,0,0]');", "dimension_mismatch")
    p.err("INSERT INTO t VALUES (1,'ab','[]');", "dimension_mismatch")
    p.err("INSERT INTO t VALUES (1,'中文中文中','[0,0]');", "varchar_too_long")
    p.ok("INSERT INTO t VALUES (1,'中文中文','[0,0]');")
    r = p.ok("SELECT s FROM t WHERE id=1;")
    assert r["rows"] == [["中文中文"]], r
    p.err("INSERT INTO t VALUES (2,'x','[0,0]'),(2,'y','[0,0]');", "duplicate_key")
    r = p.ok("SELECT id FROM t ORDER BY id;")
    assert [x[0] for x in r["rows"]] == [1], r
    p.err("SELECT * FROM t JOIN x ON t.id=x.id;", "unsupported_sql")
    p.err("SELECT COUNT(*) FROM t;", "unsupported_sql")
    p.err("UPDATE t SET id=9 WHERE id=1;", "unsupported_sql")
    p.err("COMMIT;", "no_transaction")
    p.ok("BEGIN;")
    p.err("BEGIN;", "transaction_active")
    p.ok("INSERT INTO t VALUES (2,'x','[1,0]');")
    p.err("INSERT INTO t VALUES (3,'y','[0]');", "dimension_mismatch")
    p.ok("COMMIT;")
    r = p.ok("SELECT id FROM t ORDER BY id;")
    assert [x[0] for x in r["rows"]] == [1, 2], r
    r = p.ok("SELECT id, L2_DISTANCE(e,'[0,0]') AS dist FROM t ORDER BY dist LIMIT 5;", mode="ann")
    assert [x[0] for x in r["rows"]] == [1, 2], r
    p.ok("CREATE VECTOR INDEX ti ON t(e);")
    r = p.ok("SELECT id FROM t WHERE s='absent' ORDER BY id LIMIT 5;", mode="ann")
    assert r["rows"] == [], r
    p.err("CREATE VECTOR INDEX ti2 ON t(e);", "index_exists")
    p.err("INSERT INTO t VALUES (9223372036854775808,'x','[0,0]');", "bigint_overflow")
    p.ok("INSERT INTO t VALUES (9223372036854775807,'x','[0,0]');")
    r = p.ok("SELECT id FROM t WHERE id=9223372036854775807;")
    assert r["rows"] == [[9223372036854775807]], r
    p.close()

print("EDGE TESTS PASS")
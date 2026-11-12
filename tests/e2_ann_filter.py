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
        r = json.loads(self.p.stdout.readline())
        assert r.get("ok"), r
        return r

    def close(self):
        self.id += 1
        self.p.stdin.write(json.dumps({"id": self.id, "op": "shutdown"}) + "\n")
        self.p.stdin.flush()
        self.p.wait(timeout=10)


Q = "SELECT id, L2_DISTANCE(embedding,'[0,0]') AS distance FROM docs{w} ORDER BY distance LIMIT {k};"

with tempfile.TemporaryDirectory() as d:
    p = Proc(d)
    p.req("CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(8), embedding VECTOR(2));")
    p.req("INSERT INTO docs VALUES (8,'keep','[1,0]'),(3,'keep','[-1,0]'),(1,'keep','[0,0]'),(2,'skip','[0,0]'),(4,'skip','[0.5,0]');")
    p.req("CREATE VECTOR INDEX di ON docs(embedding);")

    r = p.req(Q.format(w=" WHERE category='keep'", k=10), mode="exact")
    print("exact  keep LIMIT 10:", json.dumps(r["rows"]))
    r = p.req(Q.format(w=" WHERE category='keep'", k=10), mode="ann")
    print("ann    keep LIMIT 10:", json.dumps(r["rows"]))
    r = p.req(Q.format(w=" WHERE category='absent'", k=10), mode="ann")
    print("ann  absent LIMIT 10:", json.dumps(r["rows"]))
    r = p.req(Q.format(w="", k=2), mode="ann")
    print("ann nofilter LIMIT 2:", json.dumps(r["rows"]))
    p.close()
print("E2 DONE")
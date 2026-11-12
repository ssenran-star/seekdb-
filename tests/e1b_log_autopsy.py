import json
import os
import struct
import subprocess
import sys
import tempfile

BIN = sys.argv[1] if len(sys.argv) > 1 else "./build/minivectordb"
TYPES = {1: "CREATE_TABLE", 2: "CREATE_INDEX", 3: "INSERT", 4: "UPDATE", 5: "DELETE", 6: "COMMIT", 7: "ABORT"}


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

    def close(self):
        self.id += 1
        self.p.stdin.write(json.dumps({"id": self.id, "op": "shutdown"}) + "\n")
        self.p.stdin.flush()
        self.p.wait(timeout=10)


def autopsy(path):
    data = open(path, "rb").read()
    off = 8
    print(f"  log file size = {len(data)} bytes")
    while off + 4 <= len(data):
        (ln,) = struct.unpack_from("<I", data, off)
        if off + 4 + ln + 4 > len(data):
            print(f"  @{off}: TORN RECORD (declared len={ln}, bytes left={len(data)-off-4})")
            break
        body = data[off + 4 : off + 4 + ln]
        (end,) = struct.unpack_from("<I", data, off + 4 + ln)
        txn, stmt, typ = struct.unpack_from("<QIB", body, 0)
        info = TYPES.get(typ, str(typ))
        if typ in (3, 4, 5):
            tid, pk = struct.unpack_from("<Iq", body, 13)
            info += f" pk={pk}"
        if end != 0x4E45564D:
            info += " BAD_MAGIC"
        print(f"  @{off}: {info} txn={txn} stmt={stmt}")
        off += 4 + ln + 4


def ids(p):
    r = p.req("SELECT id FROM t ORDER BY id;")
    return [row[0] for row in r["rows"]]


print("=== PHASE 1: kill BEFORE commit (rows written, no COMMIT marker) ===")
with tempfile.TemporaryDirectory() as d:
    p = Proc(d)
    p.req("CREATE TABLE t (id BIGINT PRIMARY KEY, v VARCHAR(8));")
    p.req("INSERT INTO t VALUES (1,'a'),(2,'b');")
    p.req("BEGIN;")
    p.req("INSERT INTO t VALUES (3,'c'),(4,'d');")
    p.kill9()
    print("log autopsy after kill:")
    autopsy(os.path.join(d, "mvdb.log"))
    p = Proc(d)
    print("visible rows after restart:", ids(p))
    p.close()

print()
print("=== PHASE 2: commit OK, kill, then artificially tear the COMMIT marker ===")
with tempfile.TemporaryDirectory() as d:
    p = Proc(d)
    p.req("CREATE TABLE t (id BIGINT PRIMARY KEY, v VARCHAR(8));")
    p.req("INSERT INTO t VALUES (1,'a'),(2,'b');")
    p.req("BEGIN;")
    p.req("INSERT INTO t VALUES (3,'c'),(4,'d');")
    r = p.req("COMMIT;")
    assert r["ok"]
    p.kill9()
    log = os.path.join(d, "mvdb.log")
    print("log autopsy after committed kill:")
    autopsy(log)
    p = Proc(d)
    print("visible rows (marker intact):", ids(p))
    p.close()
    size = os.path.getsize(log)
    with open(log, "r+b") as f:
        f.truncate(size - 10)
    print(f"artificially truncated last 10 bytes (torn COMMIT marker; mechanism demo)")
    p = Proc(d)
    print("visible rows (marker torn):", ids(p))
    p.close()
print("E1B DONE")
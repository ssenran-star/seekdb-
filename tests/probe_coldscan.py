#!/usr/bin/env python3
"""Over-memory T/B/M probe for the packed-scan path. Not used by the official
binary or protocol_check. Typical AMD run (needs a 2g memory cgroup):

  python3 tests/probe_coldscan.py ./build/minivectordb --n 2000000 --dim 256
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time


def mem_peak_kb():
    # Prefer cgroup memory.peak when the process is already in a 2g cgroup.
    for path in (
        "/sys/fs/cgroup/memory.peak",
        "/sys/fs/cgroup/memory/memory.max_usage_in_bytes",
    ):
        try:
            raw = open(path).read().strip()
            if raw.isdigit():
                v = int(raw)
                return v // 1024 if v > 10**9 or path.endswith("bytes") else v // 1024
        except OSError:
            pass
    return None


def io_bytes(pid):
    try:
        txt = open(f"/proc/{pid}/io").read()
    except OSError:
        return None, None
    r = w = None
    for line in txt.splitlines():
        if line.startswith("read_bytes:"):
            r = int(line.split()[1])
        elif line.startswith("write_bytes:"):
            w = int(line.split()[1])
    return r, w


class Session:
    def __init__(self, proc):
        self.p = proc
        line = proc.stdout.readline()
        rec = json.loads(line)
        assert rec.get("type") == "ready", rec
        self.i = 0

    def req(self, sql, mode="exact"):
        self.i += 1
        self.p.stdin.write(json.dumps({"id": self.i, "op": "execute", "sql": sql, "search_mode": mode}) + "\n")
        self.p.stdin.flush()
        rec = json.loads(self.p.stdout.readline())
        if not rec.get("ok"):
            raise SystemExit("execute failed: %s" % rec)
        return rec

    def shutdown(self):
        self.i += 1
        self.p.stdin.write(json.dumps({"id": self.i, "op": "shutdown"}) + "\n")
        self.p.stdin.flush()
        rec = json.loads(self.p.stdout.readline())
        assert rec.get("ok"), rec
        self.p.wait(timeout=600)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("bin")
    ap.add_argument("--n", type=int, default=600000)
    ap.add_argument("--dim", type=int, default=128)
    ap.add_argument("--queries", type=int, default=5)
    ap.add_argument("--data-dir")
    args = ap.parse_args()

    data = args.data_dir or tempfile.mkdtemp(prefix="probe_coldscan_")
    os.makedirs(data, exist_ok=True)
    vecs = []
    base = [f"{((i * 17) % 100) / 10.0:.1f}" for i in range(args.dim)]
    for r in range(8):
        rot = base[r:] + base[:r]
        vecs.append("[" + ",".join(rot) + "]")

    def spawn():
        return subprocess.Popen(
            [args.bin, "--data-dir", data],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )

    t0 = time.perf_counter()
    p = spawn()
    s = Session(p)
    s.req(f"CREATE TABLE big (id BIGINT PRIMARY KEY, cat VARCHAR(16), e VECTOR({args.dim}));")
    batch = 100
    for b in range(args.n // batch):
        rows = []
        for i in range(b * batch, (b + 1) * batch):
            rows.append(f"({i},'c{i % 8}','{vecs[i % 8]}')")
        s.req("INSERT INTO big VALUES " + ",".join(rows) + ";")
    s.shutdown()
    t_write = time.perf_counter() - t0
    logsz = os.path.getsize(os.path.join(data, "mvdb.log"))
    print(f"WRITE {t_write:.3f}s log={logsz} n={args.n} dim={args.dim}", flush=True)

    q = "[" + ",".join(["0.1"] * args.dim) + "]"
    p = spawn()
    s = Session(p)
    times = []
    for qi in range(args.queries):
        t1 = time.perf_counter()
        if qi % 2 == 1:
            s.req(f"SELECT id, L2_DISTANCE(e,'{q}') AS d FROM big WHERE cat='c1' ORDER BY d LIMIT 10;")
        else:
            s.req(f"SELECT id, L2_DISTANCE(e,'{q}') AS d FROM big ORDER BY d LIMIT 10;")
        dt = time.perf_counter() - t1
        times.append(dt)
        print(f"QUERY{qi+1} {dt:.3f}s", flush=True)
    rbytes, wbytes = io_bytes(p.pid)
    s.shutdown()
    peak = mem_peak_kb()
    snap = os.path.join(data, "mvdb.scan.1")
    snapsz = os.path.getsize(snap) if os.path.exists(snap) else 0
    print(
        f"SUMMARY T_write={t_write:.3f}s T_query={times} "
        f"B_read={rbytes} B_write={wbytes} mem.peak_kb={peak} snap={snapsz}"
    )
    if not args.data_dir:
        shutil.rmtree(data, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

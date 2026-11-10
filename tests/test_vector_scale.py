#!/usr/bin/env python3
"""Large-scan vector check: exercises the optional numpy fast path.

Run once with numpy on PYTHONPATH and once without, and compare the verdict.
Also checks transaction visibility of vector search through BEGIN/ROLLBACK.
"""
from __future__ import annotations

import json
import math
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from minivectordb import engine as engine_module  # noqa: E402

COUNT = 600
DIM = 4


def vectors():
    return [(i, [i % 7, (i * 3) % 11, (i * 5) % 13, 0]) for i in range(1, COUNT + 1)]


def truth(limit=10):
    scored = [(math.sqrt(sum(v * v for v in vec)), ident)
              for ident, vec in vectors()]
    scored.sort(key=lambda pair: (pair[0], pair[1]))
    return [[ident, distance] for distance, ident in scored[:limit]]


def main():
    work = Path(tempfile.mkdtemp(prefix="minivectordb-scale-"))
    data = work / "data"
    data.mkdir()
    env = os.environ.copy()
    env["MINIVECTORDB_DATA"] = str(data)
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    proc = subprocess.Popen(
        [sys.executable, "-B", "-m", "minivectordb.main"],
        cwd=str(ROOT), env=env, stdin=subprocess.PIPE,
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
        text=True, encoding="utf-8", bufsize=1)
    serial = 0

    def call(sql):
        nonlocal serial
        serial += 1
        proc.stdin.write(json.dumps(
            {"id": serial, "op": "execute", "sql": sql,
             "search_mode": "exact"}) + "\n")
        proc.stdin.flush()
        return json.loads(proc.stdout.readline())

    try:
        assert json.loads(proc.stdout.readline()) == {"type": "ready"}
        call("CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(8), embedding VECTOR(4));")
        rows = ",".join("(%d,'a','[%d,%d,%d,%d]')" % (
            ident, vec[0], vec[1], vec[2], vec[3])
            for ident, vec in vectors())
        inserted = call("INSERT INTO docs VALUES " + rows + ";")
        full = call("SELECT id, L2_DISTANCE(embedding, '[0,0,0,0]') AS distance "
                    "FROM docs WHERE category = 'a' ORDER BY distance LIMIT 10;")
        filtered = call("SELECT id, L2_DISTANCE(embedding, '[0,0,0,0]') AS distance "
                        "FROM docs WHERE category = 'absent' ORDER BY distance LIMIT 10;")
        call("BEGIN;")
        call("INSERT INTO docs VALUES (999,'a','[0,0,0,0]');")
        in_tx = call("SELECT id, L2_DISTANCE(embedding, '[0,0,0,0]') AS distance "
                     "FROM docs WHERE category = 'a' ORDER BY distance LIMIT 1;")
        call("ROLLBACK;")
        after = call("SELECT id, L2_DISTANCE(embedding, '[0,0,0,0]') AS distance "
                     "FROM docs WHERE category = 'a' ORDER BY distance LIMIT 1;")
        serial += 1
        proc.stdin.write(json.dumps({"id": serial, "op": "shutdown"}) + "\n")
        proc.stdin.flush()
        proc.stdout.readline()
        proc.stdin.close()
        proc.wait(timeout=15)

        expected = truth()
        ok = (inserted.get("affected_rows") == COUNT
              and all(abs(a[1] - b[1]) < 1e-5 and a[0] == b[0]
                      for a, b in zip(full.get("rows", []), expected))
              and len(full.get("rows", [])) == 10
              and filtered.get("rows") == []
              and in_tx.get("rows", [[None]])[0][0] == 999
              and in_tx.get("rows", [[None, None]])[0][1] == 0.0
              and after.get("rows", [[None]])[0][0] == expected[0][0])
        print(json.dumps({
            "numpy_active": engine_module.np is not None,
            "inserted": inserted.get("affected_rows"),
            "top1_actual": full.get("rows", [])[:1],
            "top1_expected": expected[:1],
            "empty_filter_rows": filtered.get("rows"),
            "in_transaction_top1": in_tx.get("rows"),
            "after_rollback_top1": after.get("rows"),
            "verdict": "PASS" if ok else "FAIL",
        }, ensure_ascii=False, indent=2))
        return 0 if ok else 1
    finally:
        if proc.poll() is None:
            proc.kill()
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

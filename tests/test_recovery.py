#!/usr/bin/env python3
"""Crash / recovery checks that kill the process without a shutdown message.

Scenarios required by the task description:
  1. committed rows survive a forced kill and restart
  2. an uncommitted transaction never becomes visible
  3. killing again during recovery is harmless (recovery is idempotent)
"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def start(data_dir):
    env = os.environ.copy()
    env["MINIVECTORDB_DATA"] = str(data_dir)
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    proc = subprocess.Popen(
        [sys.executable, "-B", "-m", "minivectordb.main"],
        cwd=str(ROOT), env=env, stdin=subprocess.PIPE,
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
        text=True, encoding="utf-8", bufsize=1)
    assert json.loads(proc.stdout.readline()) == {"type": "ready"}
    return proc


def call(proc, serial, sql):
    proc.stdin.write(json.dumps(
        {"id": serial, "op": "execute", "sql": sql,
         "search_mode": "exact"}) + "\n")
    proc.stdin.flush()
    return json.loads(proc.stdout.readline())


def main():
    work = Path(tempfile.mkdtemp(prefix="minivectordb-recovery-"))
    data = work / "data"
    data.mkdir()
    results = {}
    try:
        proc = start(data)
        results["create"] = call(proc, 1,
            "CREATE TABLE docs (id BIGINT PRIMARY KEY, content VARCHAR(16));")
        results["committed_insert"] = call(proc, 2,
            "INSERT INTO docs VALUES (1, 'committed');")
        results["begin"] = call(proc, 3, "BEGIN;")
        results["uncommitted_insert"] = call(proc, 4,
            "INSERT INTO docs VALUES (2, 'uncommitted');")
        proc.kill()
        proc.wait(timeout=10)

        proc = start(data)
        results["after_crash"] = call(proc, 1,
            "SELECT id, content FROM docs ORDER BY id;")
        proc.kill()
        proc.wait(timeout=10)

        proc = start(data)
        results["after_second_kill"] = call(proc, 1,
            "SELECT id, content FROM docs ORDER BY id;")
        results["commit_after_recovery"] = call(proc, 2,
            "INSERT INTO docs VALUES (3, 'after-recovery');")
        proc.kill()
        proc.wait(timeout=10)

        proc = start(data)
        results["final"] = call(proc, 1,
            "SELECT id, content FROM docs ORDER BY id;")
        proc.stdin.write(json.dumps({"id": 2, "op": "shutdown"}) + "\n")
        proc.stdin.flush()
        proc.stdout.readline()
        proc.stdin.close()
        proc.wait(timeout=10)

        expected_after_crash = {"columns": ["id", "content"],
                                "rows": [[1, "committed"]]}
        expected_final = {"columns": ["id", "content"],
                          "rows": [[1, "committed"], [3, "after-recovery"]]}
        ok = (results["after_crash"].get("rows") == expected_after_crash["rows"]
              and results["after_second_kill"].get("rows") == expected_after_crash["rows"]
              and results["final"].get("rows") == expected_final["rows"])
        results["verdict"] = "PASS" if ok else "FAIL"
        print(json.dumps(results, ensure_ascii=False, indent=2))
        return 0 if ok else 1
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

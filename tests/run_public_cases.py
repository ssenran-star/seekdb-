#!/usr/bin/env python3
"""Run the public mysqltest cases without Docker.

This mirrors protocol_check/run.py's comparison semantics (including the
strict type check and 1e-5 float tolerances) but talks to a local Python
process instead of a container.  It is a development aid, not the judge.
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
CASES = ROOT / "tests" / "public_cases"
ABS_TOL = 1e-5
REL_TOL = 1e-5


def same(actual, expected):
    if isinstance(expected, float):
        return (type(actual) in (int, float) and math.isfinite(actual)
                and math.isclose(actual, expected, abs_tol=ABS_TOL,
                                 rel_tol=REL_TOL))
    if isinstance(expected, list):
        return (isinstance(actual, list) and len(actual) == len(expected)
                and all(same(a, b) for a, b in zip(actual, expected)))
    return type(actual) is type(expected) and actual == expected


def assert_response(response, expected):
    if "error" in expected:
        assert response.get("ok") is False, "statement should fail: %r" % response
        code = response.get("error", {}).get("code")
        assert isinstance(code, str) and code, "missing error.code: %r" % response
        return
    assert response.get("ok") is True, "statement failed: %r" % response
    for key in ("affected_rows", "columns"):
        if key in expected:
            assert same(response.get(key), expected[key]), \
                "%s: %r != %r" % (key, response.get(key), expected[key])
    if "rows" not in expected:
        return
    actual, wanted = response.get("rows"), expected["rows"]
    assert isinstance(actual, list), "missing rows"
    if expected.get("unordered"):
        remaining = list(actual)
        for row in wanted:
            index = next((i for i, candidate in enumerate(remaining)
                          if same(candidate, row)), None)
            assert index is not None, "missing row %r" % row
            remaining.pop(index)
        assert not remaining, "unexpected rows %r" % remaining[:5]
    else:
        assert same(actual, wanted), "rows: %r != %r" % (actual, wanted)


class Session:
    def __init__(self, data_dir, err_log):
        self.data_dir = data_dir
        self.serial = 0
        env = os.environ.copy()
        env["MINIVECTORDB_DATA"] = str(data_dir)
        env["PYTHONDONTWRITEBYTECODE"] = "1"
        self.err = open(err_log, "a", encoding="utf-8")
        self.proc = subprocess.Popen(
            [sys.executable, "-B", "-m", "minivectordb.main"],
            cwd=str(ROOT), env=env, stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=self.err,
            text=True, encoding="utf-8", bufsize=1)
        line = self.proc.stdout.readline()
        assert line, "process exited before ready; see %s" % err_log
        assert json.loads(line) == {"type": "ready"}, \
            "bad handshake: %r" % line

    def send(self, message):
        self.proc.stdin.write(json.dumps(message, ensure_ascii=False) + "\n")
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        assert line, "no response; see stderr log"
        return json.loads(line)

    def execute(self, sql, mode):
        self.serial += 1
        return self.send({"id": self.serial, "op": "execute", "sql": sql,
                          "search_mode": mode})

    def shutdown(self):
        self.serial += 1
        response = self.send({"id": self.serial, "op": "shutdown"})
        assert response.get("ok") is True, response
        self.proc.stdin.close()
        self.proc.wait(timeout=15)
        assert self.proc.returncode == 0, "nonzero exit after shutdown"
        self.err.close()


def run_case(case_id, work_dir):
    statements = (CASES / (case_id + ".test")).read_text(
        encoding="utf-8").splitlines()
    expected = json.loads((CASES / (case_id + ".result")).read_text(
        encoding="utf-8"))
    data_dir = work_dir / "data"
    data_dir.mkdir(parents=True, exist_ok=True)
    err_log = str(work_dir / "stderr.log")
    session = Session(data_dir, err_log)
    mode = "exact"
    index = 0
    try:
        for raw in statements:
            line = raw.strip()
            if not line:
                continue
            if line.startswith("--restart"):
                session.shutdown()
                session = Session(data_dir, err_log)
                continue
            if line.startswith("--mode"):
                mode = line.split()[-1].lower()
                continue
            assert index < len(expected), "more SQL statements than assertions"
            response = session.execute(line, mode)
            try:
                assert_response(response, expected[index])
            except AssertionError as exc:
                return "FAIL at statement %d [%s]: %s\n  response: %r" % (
                    index + 1, line, exc, response)
            index += 1
        assert index == len(expected), \
            "assertions left unused: %d of %d" % (index, len(expected))
        session.shutdown()
        return None
    finally:
        if session.proc.poll() is None:
            session.proc.kill()


def main():
    selected = sys.argv[1:]
    manifest = json.loads((CASES / "manifest.json").read_text(encoding="utf-8"))
    ids = selected or [item["id"] for item in manifest["cases"]]
    failures = 0
    root = Path(tempfile.mkdtemp(prefix="minivectordb-public-"))
    try:
        for case_id in ids:
            work = root / case_id
            work.mkdir(parents=True, exist_ok=True)
            try:
                problem = run_case(case_id, work)
            except Exception as exc:
                problem = "ERROR: %s: %s" % (type(exc).__name__, exc)
            if problem:
                failures += 1
                print("FAIL %-32s %s" % (case_id, problem))
            else:
                print("PASS %s" % case_id)
    finally:
        shutil.rmtree(root, ignore_errors=True)
    print("\n%d/%d cases passed" % (len(ids) - failures, len(ids)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Local runner for the public mysqltest cases.

The bundled protocol_check/run.py assumes a POSIX container transport
(selectors + process groups). This runner speaks the same JSON-Lines protocol
against a locally launched process and works on Windows and Linux alike, so the
public cases can be verified before the image is built.

Usage:
    python3 scripts/run_public_tests.py --bin path/to/minivectordb
    python3 scripts/run_public_tests.py --bin path/to/minivectordb --case sql_crud
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "protocol_check"))
from _assertions import assert_response  # noqa: E402

STARTUP_TIMEOUT = 60
REQUEST_TIMEOUT = 60
CASE_TIMEOUT = 300


class ProtocolError(RuntimeError):
    pass


def strict_json(text):
    def invalid(value):
        raise ValueError("non-finite JSON number: %s" % value)

    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError("duplicate JSON key: %s" % key)
            result[key] = value
        return result

    return json.loads(text, parse_constant=invalid, object_pairs_hook=pairs)


class Session:
    """One database process speaking the JSON-Lines protocol."""

    def __init__(self, binary, data_dir, log_path):
        self.binary = str(binary)
        self.data_dir = str(data_dir)
        self.log = open(log_path, "ab", buffering=0)
        self.proc = None
        self.sequence = 0
        self._watchdog = None

    def start(self):
        env = dict(os.environ)
        env["VECTORDB_DATA_DIR"] = self.data_dir
        self.proc = subprocess.Popen(
            [self.binary],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=self.log,
            env=env,
        )
        self._arm(STARTUP_TIMEOUT)
        line = self._read_line()
        message = strict_json(line)
        if message != {"type": "ready"}:
            raise ProtocolError("expected ready, got %r" % (message,))
        self._disarm()

    def _arm(self, seconds):
        def killer():
            time.sleep(seconds)
            if self.proc and self.proc.poll() is None:
                try:
                    self.proc.kill()
                except Exception:
                    pass

        self._watchdog = threading.Thread(target=killer, daemon=True)
        self._watchdog.start()

    def _disarm(self):
        self._watchdog = None

    def _read_line(self):
        chunks = []

        def reader():
            while True:
                byte = self.proc.stdout.read(1)
                if not byte:
                    break
                chunks.append(byte)
                if byte == b"\n":
                    break

        thread = threading.Thread(target=reader, daemon=True)
        thread.start()
        thread.join(REQUEST_TIMEOUT)
        if thread.is_alive() or not chunks:
            raise ProtocolError("timeout waiting for a response")
        return b"".join(chunks).decode("utf-8").rstrip("\r\n")

    def request(self, sql=None, mode="exact", op="execute"):
        self.sequence += 1
        payload = {"id": self.sequence, "op": op}
        if sql is not None:
            payload.update(sql=sql, search_mode=mode)
        data = (json.dumps(payload, ensure_ascii=False, allow_nan=False) + "\n").encode("utf-8")
        self._arm(REQUEST_TIMEOUT)
        self.proc.stdin.write(data)
        self.proc.stdin.flush()
        line = self._read_line()
        self._disarm()
        try:
            response = strict_json(line)
        except Exception:
            print("[raw response] %r" % line)
            raise
        if type(response.get("id")) is not int or response["id"] != payload["id"]:
            raise ProtocolError("response id differs from request: %r" % (response,))
        if type(response.get("ok")) is not bool:
            raise ProtocolError("missing boolean ok: %r" % (response,))
        return response

    def shutdown(self):
        response = self.request(op="shutdown")
        if response["ok"] is not True:
            raise ProtocolError("shutdown failed")
        self.proc.stdin.close()
        code = self.proc.wait(timeout=REQUEST_TIMEOUT)
        if code != 0:
            raise ProtocolError("non-zero exit after shutdown: %s" % code)

    def kill(self):
        if self.proc and self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait(timeout=10)

    def close(self):
        self.kill()
        self.log.close()


def check_case(session_factory, case, package, config, workdir):
    expectations = strict_json((package / case["result"]).read_text(encoding="utf-8"))
    statements = (package / case["test"]).read_text(encoding="utf-8").splitlines()
    index, mode = 0, "exact"
    data_dir = workdir / case["id"]
    data_dir.mkdir(parents=True, exist_ok=True)
    session = session_factory(data_dir)
    session.start()
    try:
        for number, text in enumerate(statements, 1):
            sql = text.strip()
            if not sql or sql.startswith("#"):
                continue
            if sql == "--restart":
                session.shutdown()
                session = session_factory(data_dir)
                session.start()
            elif sql in ("--mode exact", "--mode ann"):
                mode = sql.split()[1]
            elif sql.startswith("--"):
                raise ValueError("unsupported directive: %s" % sql)
            else:
                if index >= len(expectations):
                    raise ValueError("missing expected result")
                response = session.request(sql, mode)
                try:
                    assert_response(response, expectations[index], config)
                except AssertionError as exc:
                    raise AssertionError("line %d: %s: %s" % (number, sql, exc)) from exc
                index += 1
        if index != len(expectations):
            raise ValueError("SQL/result count mismatch")
    finally:
        try:
            session.shutdown()
        except Exception:
            session.kill()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", required=True, help="path to the minivectordb executable")
    parser.add_argument("--case", action="append", help="public case id; repeatable")
    parser.add_argument("--package", type=Path, default=ROOT / "mysqltest")
    parser.add_argument("--config", type=Path, default=ROOT / "protocol_check" / "config.json")
    parser.add_argument("--output", type=Path, help="directory for logs and the report")
    args = parser.parse_args(argv)

    binary = Path(args.bin).resolve()
    if not binary.exists():
        parser.error("binary not found: %s" % binary)
    config = strict_json(args.config.read_text(encoding="utf-8"))
    manifest = strict_json((args.package / "manifest.json").read_text(encoding="utf-8"))
    cases = manifest["cases"]
    if args.case:
        unknown = set(args.case) - {c["id"] for c in cases}
        if unknown:
            parser.error("unknown public case: %s" % ", ".join(sorted(unknown)))
        cases = [c for c in cases if c["id"] in args.case]

    workdir = Path(tempfile.mkdtemp(prefix="mvdb-cases-"))
    outdir = args.output or (workdir / "out")
    outdir.mkdir(parents=True, exist_ok=True)

    def factory(data_dir):
        return Session(binary, data_dir, outdir / "stderr.log")

    results = []
    for case in cases:
        started = time.monotonic()
        try:
            check_case(factory, case, args.package, config, workdir)
            status, message = "PASS", ""
        except Exception as exc:  # noqa: BLE001
            status, message = "FAIL", "%s: %s" % (type(exc).__name__, exc)
        row = {
            "id": case["id"],
            "status": status,
            "message": message,
            "duration_ms": round((time.monotonic() - started) * 1000),
        }
        results.append(row)
        print("%-32s %-6s %s" % (row["id"], row["status"], row["message"]), flush=True)

    passed = all(r["status"] == "PASS" for r in results)
    report = {
        "scope": "public-cases-local",
        "passed": passed,
        "cases": results,
        "binary": str(binary),
    }
    (outdir / "result.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n",
                                        encoding="utf-8")
    print("\n%s  (%d/%d cases passed)" % (
        "ALL PUBLIC CASES PASSED" if passed else "FAILURES PRESENT",
        sum(1 for r in results if r["status"] == "PASS"), len(results)))
    print("report: %s" % (outdir / "result.json"))
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())

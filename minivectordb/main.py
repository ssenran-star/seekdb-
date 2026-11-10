#!/usr/bin/env python3
"""JSON Lines stdin/stdout front end.

Follows STDIO_PROTOCOL.md on the 2026-competition-round2 branch:
  * emit {"type":"ready"} before reading any request
  * exactly one JSON response per request, on one physical line, flushed
  * stdout carries protocol messages only; diagnostics go to stderr
"""
from __future__ import annotations

import json
import os
import sys
import traceback

from .engine import DBError, Engine

DATA_DIR = os.environ.get("MINIVECTORDB_DATA", "/data")


def emit(message):
    sys.stdout.write(json.dumps(message, ensure_ascii=False, allow_nan=False,
                                separators=(",", ":")) + "\n")
    sys.stdout.flush()


def main():
    for stream in (sys.stdin, sys.stdout):
        try:
            stream.reconfigure(encoding="utf-8")
        except (AttributeError, ValueError):
            pass

    engine = Engine(DATA_DIR)
    emit({"type": "ready"})

    for line in sys.stdin:
        ident = None
        try:
            request = json.loads(line)
            ident = request.get("id")
            op = request.get("op")
            if op == "shutdown":
                engine.shutdown()
                emit({"id": ident, "ok": True})
                return 0
            if op != "execute":
                raise DBError("unsupported_op", "unsupported operation %r" % op)
            result = engine.execute(request["sql"],
                                    request.get("search_mode", "exact"))
            response = dict(result)
            response["id"] = ident
            response["ok"] = True
            emit(response)
        except DBError as exc:
            emit({"id": ident, "ok": False,
                  "error": {"code": exc.code, "message": exc.message}})
        except Exception as exc:  # keep the process alive after a bad statement
            traceback.print_exc(file=sys.stderr)
            emit({"id": ident, "ok": False,
                  "error": {"code": "internal_error", "message": str(exc)}})
    engine.shutdown()
    return 0


if __name__ == "__main__":
    sys.exit(main())

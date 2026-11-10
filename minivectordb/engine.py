"""MiniVectorDB execution engine.

Design notes (see README.md for the full write-up):
  * /data/catalog.json holds table definitions and vector index metadata.
  * /data/wal.log is an append-only log.  Each transaction writes its record
    changes followed by a commit marker, then fsyncs before COMMIT is acked.
    Recovery replays only transactions closed by a commit marker, so a crash
    can never leave a partially applied transaction behind.
  * The in-memory row dict is a cache over the log; offsets always point at
    the authoritative on-disk record.  Above CACHE_LIMIT rows the cache is
    dropped and rows are read from the log on demand.
  * Vector TopK is an exact scan.  Filtering happens before distance ranking,
    so filtered TopK is computed over the qualifying set, as required.
"""
from __future__ import annotations

import functools
import json
import math
import os
import struct
import sys
import zlib

from .sql import ParseError, parse

try:
    import numpy as np
except Exception:  # numpy is optional; the engine has a pure-Python path
    np = None

CACHE_LIMIT = 200_000
FLOAT32_MAX = 3.4028234663852886e38
INT64_MIN = -(1 << 63)
INT64_MAX = (1 << 63) - 1


class DBError(Exception):
    def __init__(self, code, message=""):
        super().__init__(message or code)
        self.code = code
        self.message = message or code


class _Deleted:
    pass


DELETED = _Deleted()


def _dumps(obj):
    return json.dumps(obj, ensure_ascii=False, separators=(",", ":"),
                      sort_keys=True, allow_nan=False)


def _to_float32(value):
    return struct.unpack("f", struct.pack("f", float(value)))[0]


def _fsync_dir(path):
    try:
        fd = os.open(path, os.O_RDONLY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)
    except (OSError, AttributeError):
        pass


class Storage:
    """Catalog + write-ahead log + recovery."""

    def __init__(self, data_dir):
        self.dir = data_dir
        os.makedirs(data_dir, exist_ok=True)
        self.catalog_path = os.path.join(data_dir, "catalog.json")
        self.wal_path = os.path.join(data_dir, "wal.log")
        if not os.path.exists(self.wal_path):
            with open(self.wal_path, "wb"):
                pass
        self.fp = open(self.wal_path, "r+b", buffering=0)
        self.tables = {}
        self.offsets = {}
        self.rows = {}
        self.cache_enabled = True
        self.version = 0
        self.next_tx = 1
        self._load_catalog()
        self._recover()

    def _load_catalog(self):
        if not os.path.exists(self.catalog_path):
            return
        try:
            with open(self.catalog_path, "r", encoding="utf-8") as handle:
                self.tables = json.load(handle)
        except (OSError, ValueError):
            self.tables = {}
        for name in self.tables:
            self.offsets.setdefault(name, {})
            self.rows.setdefault(name, {})

    def save_catalog(self):
        tmp = self.catalog_path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as handle:
            handle.write(_dumps(self.tables))
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(tmp, self.catalog_path)
        _fsync_dir(self.dir)

    def _append(self, obj):
        payload = _dumps(obj)
        crc = zlib.crc32(payload.encode("utf-8")) & 0xFFFFFFFF
        line = (payload + "\t" + format(crc, "08x") + "\n").encode("utf-8")
        self.fp.seek(0, os.SEEK_END)
        offset = self.fp.tell()
        self.fp.write(line)
        return offset

    def _read_line(self, offset):
        self.fp.seek(offset)
        line = self.fp.readline()
        if not line.endswith(b"\n"):
            return None
        try:
            payload, crc = line[:-1].decode("utf-8").rsplit("\t", 1)
            if format(zlib.crc32(payload.encode("utf-8")) & 0xFFFFFFFF,
                      "08x") != crc:
                return None
            return json.loads(payload)
        except (ValueError, UnicodeDecodeError):
            return None

    def _recover(self):
        self.fp.seek(0)
        pending = []
        current_tx = None
        while True:
            offset = self.fp.tell()
            line = self.fp.readline()
            if not line or not line.endswith(b"\n"):
                break
            obj = self._read_line(offset)
            if obj is None:
                break
            tx = obj.get("tx")
            op = obj.get("op")
            if op == "commit":
                for item, item_offset in pending:
                    self._apply(item, item_offset, replay=True)
                pending = []
                current_tx = None
                self.next_tx = max(self.next_tx, (tx or 0) + 1)
                continue
            if current_tx is None:
                current_tx = tx
            elif tx != current_tx:
                pending = []
                current_tx = tx
            if op in ("put", "del"):
                pending.append((obj, offset))
            else:
                pending = []
        self.fp.seek(0, os.SEEK_END)
        self._refresh_cache_mode()

    def _apply(self, obj, offset, replay=False):
        table = obj["table"]
        pk = obj["pk"]
        self.offsets.setdefault(table, {})
        self.rows.setdefault(table, {})
        if obj["op"] == "put":
            self.offsets[table][pk] = offset
            if self.cache_enabled:
                self.rows[table][pk] = obj["row"]
        else:
            self.offsets[table].pop(pk, None)
            self.rows[table].pop(pk, None)
        if not replay:
            self.version += 1

    def _refresh_cache_mode(self):
        total = sum(len(v) for v in self.offsets.values())
        if total > CACHE_LIMIT and self.cache_enabled:
            self.cache_enabled = False
            for name in self.rows:
                self.rows[name].clear()

    def commit(self, changes):
        """Atomically persist a transaction and only then publish it."""
        tx = self.next_tx
        self.next_tx += 1
        written = []
        for op, table, pk, row in changes:
            obj = {"tx": tx, "op": op, "table": table, "pk": pk}
            if op == "put":
                obj["row"] = row
            written.append((obj, self._append(obj)))
        self._append({"tx": tx, "op": "commit", "table": "", "pk": 0})
        self.fp.flush()
        os.fsync(self.fp.fileno())
        for obj, offset in written:
            self._apply(obj, offset)
        self._refresh_cache_mode()

    def read_row(self, table, pk):
        if self.cache_enabled:
            return self.rows.get(table, {}).get(pk)
        offset = self.offsets.get(table, {}).get(pk)
        if offset is None:
            return None
        obj = self._read_line(offset)
        return obj.get("row") if obj else None

    def iter_rows(self, table):
        """Rows ordered by primary key, giving deterministic result ordering."""
        if self.cache_enabled:
            table_rows = self.rows.get(table, {})
            return [table_rows[pk] for pk in sorted(table_rows)]
        offsets = self.offsets.get(table, {})
        out = []
        for pk in sorted(offsets):
            obj = self._read_line(offsets[pk])
            if obj and "row" in obj:
                out.append(obj["row"])
        return out

    def close(self):
        try:
            self.fp.flush()
            os.fsync(self.fp.fileno())
        finally:
            self.fp.close()


class Engine:
    def __init__(self, data_dir):
        self.store = Storage(data_dir)
        self.tx = None
        self._matrix_cache = {}

    # ---- schema helpers ----------------------------------------------------

    def _table(self, name):
        key = name.lower()
        for stored, definition in self.store.tables.items():
            if stored.lower() == key:
                return definition
        raise DBError("unknown_table", "unknown table %s" % name)

    def _table_name(self, name):
        key = name.lower()
        for stored in self.store.tables:
            if stored.lower() == key:
                return stored
        raise DBError("unknown_table", "unknown table %s" % name)

    @staticmethod
    def _columns(definition):
        return {c["name"].lower(): c for c in definition["columns"]}

    @staticmethod
    def _pk_column(definition):
        for column in definition["columns"]:
            if column["primary"]:
                return column["name"]
        return definition["columns"][0]["name"]

    @staticmethod
    def _vector_column(definition):
        for column in definition["columns"]:
            if column["type"] == "VECTOR":
                return column["name"]
        return None

    def _column(self, definition, name):
        columns = self._columns(definition)
        if name.lower() not in columns:
            raise DBError("unknown_column", "unknown column %s" % name)
        return columns[name.lower()]["name"]

    # ---- value handling ----------------------------------------------------

    def _cast(self, column, value):
        ctype = column["type"]
        if ctype == "BIGINT":
            if isinstance(value, bool) or not isinstance(value, int):
                raise DBError("type_mismatch", "BIGINT requires an integer")
            if value < INT64_MIN or value > INT64_MAX:
                raise DBError("bigint_overflow", "BIGINT out of range")
            return value
        if ctype == "VARCHAR":
            if not isinstance(value, str):
                raise DBError("type_mismatch", "VARCHAR requires a string")
            limit = column.get("size")
            if limit is not None and len(value) > limit:
                raise DBError("value_too_long", "VARCHAR length exceeded")
            return value
        if ctype == "VECTOR":
            return self._parse_vector(value, column.get("size"))
        raise DBError("unsupported_sql", "unsupported column type")

    @staticmethod
    def _parse_vector(value, dimension):
        if isinstance(value, str):
            text = value.strip()
            if not (text.startswith("[") and text.endswith("]")):
                raise DBError("invalid_vector", "vector literal must be [..]")
            parts = [p.strip() for p in text[1:-1].split(",") if p.strip()]
        elif isinstance(value, (list, tuple)):
            parts = list(value)
        else:
            raise DBError("invalid_vector", "vector literal must be [..]")
        if dimension is None or len(parts) != dimension:
            raise DBError("dimension_mismatch", "vector dimension mismatch")
        result = []
        for part in parts:
            try:
                number = float(part)
            except (TypeError, ValueError):
                raise DBError("invalid_vector", "vector element is not numeric")
            if not math.isfinite(number):
                raise DBError("invalid_vector", "vector element is not finite")
            if abs(number) > FLOAT32_MAX:
                raise DBError("vector_overflow", "float32 overflow")
            try:
                converted = _to_float32(number)
            except (OverflowError, struct.error):
                raise DBError("vector_overflow", "float32 overflow")
            if not math.isfinite(converted):
                raise DBError("vector_overflow", "float32 overflow")
            result.append(converted)
        return result

    # ---- transaction visibility -------------------------------------------

    def _visible(self, table_name, definition):
        rows = self.store.iter_rows(table_name)
        overlay = self.tx["overlay"].get(table_name) if self.tx else None
        if not overlay:
            return rows
        pk_col = self._pk_column(definition)
        merged = {row[pk_col]: row for row in rows}
        for pk, value in overlay.items():
            if value is DELETED:
                merged.pop(pk, None)
            else:
                merged[pk] = value
        return [merged[pk] for pk in sorted(merged)]

    def _stage(self, table_name, changes):
        """Apply statement changes either to the open transaction or to disk."""
        if self.tx is not None:
            overlay = self.tx["overlay"].setdefault(table_name, {})
            for op, _table, pk, row in changes:
                overlay[pk] = DELETED if op == "del" else row
            self.tx["changes"].extend(changes)
            self._matrix_cache.clear()
        else:
            self.store.commit(changes)
            self._matrix_cache.clear()

    # ---- expressions -------------------------------------------------------

    @staticmethod
    def _compare(a, b):
        if a is None or b is None:
            raise DBError("null_not_supported", "NULL comparison is unsupported")
        if isinstance(a, (int, float)) and isinstance(b, (int, float)):
            return (a > b) - (a < b)
        if isinstance(a, str) and isinstance(b, str):
            return (a > b) - (a < b)
        if isinstance(a, list) and isinstance(b, list):
            return (a > b) - (a < b)
        raise DBError("type_mismatch", "incomparable operand types")

    def _eval_where(self, condition, row, definition, distances):
        if condition is None:
            return True
        kind = condition["t"]
        if kind == "and":
            return all(self._eval_where(c, row, definition, distances)
                       for c in condition["children"])
        if kind == "or":
            return any(self._eval_where(c, row, definition, distances)
                       for c in condition["children"])
        if kind == "cmp":
            left = self._eval_expr(condition["left"], row, definition, distances)
            right = self._eval_expr(condition["right"], row, definition, distances)
            op = condition["op"]
            result = self._compare(left, right)
            if op == "=":
                return result == 0
            if op in ("!=", "<>"):
                return result != 0
            if op == "<":
                return result < 0
            if op == "<=":
                return result <= 0
            if op == ">":
                return result > 0
            return result >= 0
        raise DBError("unsupported_sql", "unsupported condition")

    def _eval_expr(self, expr, row, definition, distances):
        kind = expr["t"]
        if kind == "lit":
            return expr["value"]
        if kind == "col":
            return row.get(self._column(definition, expr["name"]))
        if kind == "l2":
            return distances.get(expr)
        raise DBError("unsupported_sql", "unsupported expression")

    @staticmethod
    def _distance(left, right):
        total = 0.0
        for a, b in zip(left, right):
            delta = float(a) - float(b)
            total += delta * delta
        return math.sqrt(total)

    # ---- statements --------------------------------------------------------

    def execute(self, sql, search_mode="exact"):
        try:
            statement = parse(sql)
        except ParseError as exc:
            raise DBError("unsupported_sql", str(exc))
        handler = getattr(self, "_do_" + statement["kind"], None)
        if handler is None:
            raise DBError("unsupported_sql", "unsupported statement")
        return handler(statement, search_mode)

    def _do_create_table(self, stmt, _mode):
        name = stmt["table"]
        if name.lower() in {t.lower() for t in self.store.tables}:
            raise DBError("table_exists", "table already exists")
        columns = stmt["columns"]
        primaries = [c for c in columns if c["primary"]]
        if len(primaries) != 1 or primaries[0]["type"] != "BIGINT":
            raise DBError("invalid_schema", "exactly one BIGINT primary key required")
        vectors = [c for c in columns if c["type"] == "VECTOR"]
        if len(vectors) > 1:
            raise DBError("invalid_schema", "at most one vector column allowed")
        for column in columns:
            if column["type"] in ("VARCHAR", "VECTOR"):
                size = column.get("size")
                if not isinstance(size, int) or size <= 0:
                    raise DBError("invalid_schema", "column size is required")
        definition = {"columns": columns, "indexes": []}
        self.store.tables[name] = definition
        self.store.offsets.setdefault(name, {})
        self.store.rows.setdefault(name, {})
        self.store.save_catalog()
        return {}

    def _do_create_vector_index(self, stmt, _mode):
        name = self._table_name(stmt["table"])
        definition = self._table(name)
        column = self._column(definition, stmt["column"])
        if self._columns(definition)[column.lower()]["type"] != "VECTOR":
            raise DBError("invalid_schema", "vector index requires a VECTOR column")
        if not any(idx["name"].lower() == stmt["name"].lower()
                   for idx in definition["indexes"]):
            definition["indexes"].append({"name": stmt["name"], "column": column})
            self.store.save_catalog()
        self._matrix_cache.clear()
        return {}

    def _do_insert(self, stmt, _mode):
        name = self._table_name(stmt["table"])
        definition = self._table(name)
        columns = definition["columns"]
        pk_col = self._pk_column(definition)
        wanted = stmt["columns"]
        if wanted is None:
            if any(len(row) != len(columns) for row in stmt["rows"]):
                raise DBError("column_count_mismatch", "value count does not match")
            positions = list(range(len(columns)))
        else:
            positions = []
            for item in wanted:
                target = self._column(definition, item)
                positions.append(next(i for i, c in enumerate(columns)
                                      if c["name"] == target))
            if any(len(row) != len(positions) for row in stmt["rows"]):
                raise DBError("column_count_mismatch", "value count does not match")
        rows = []
        seen = set()
        existing = {row[pk_col] for row in self._visible(name, definition)}
        for values in stmt["rows"]:
            record = {c["name"]: None for c in columns}
            for index, value in zip(positions, values):
                target = columns[index]
                record[target["name"]] = self._cast(target, value)
            pk = record[pk_col]
            if pk in existing or pk in seen:
                raise DBError("duplicate_key", "duplicate primary key")
            seen.add(pk)
            rows.append(record)
        changes = [("put", name, row[pk_col], row) for row in rows]
        self._stage(name, changes)
        return {"affected_rows": len(rows)}

    def _do_update(self, stmt, _mode):
        name = self._table_name(stmt["table"])
        definition = self._table(name)
        pk_col = self._pk_column(definition)
        columns = self._columns(definition)
        assignments = []
        for column_name, value in stmt["assignments"]:
            real = self._column(definition, column_name)
            spec = columns[real.lower()]
            if spec["primary"]:
                raise DBError("unsupported_sql", "primary key update is unsupported")
            assignments.append((real, self._cast(spec, value)))
        new_rows = []
        for row in self._visible(name, definition):
            if not self._eval_where(stmt["where"], row, definition, {}):
                continue
            updated = dict(row)
            for real, value in assignments:
                updated[real] = value
            new_rows.append(updated)
        changes = [("put", name, row[pk_col], row) for row in new_rows]
        self._stage(name, changes)
        return {"affected_rows": len(new_rows)}

    def _do_delete(self, stmt, _mode):
        name = self._table_name(stmt["table"])
        definition = self._table(name)
        pk_col = self._pk_column(definition)
        ids = [row[pk_col] for row in self._visible(name, definition)
               if self._eval_where(stmt["where"], row, definition, {})]
        changes = [("del", name, pk, None) for pk in ids]
        self._stage(name, changes)
        return {"affected_rows": len(ids)}

    def _do_select(self, stmt, search_mode):
        name = self._table_name(stmt["table"])
        definition = self._table(name)
        pk_col = self._pk_column(definition)
        vector_col = self._vector_column(definition)
        columns = self._columns(definition)

        items = []
        for item in stmt["items"]:
            expression = item["expr"]
            if expression["t"] == "star":
                for column in definition["columns"]:
                    items.append(({"t": "col", "name": column["name"]},
                                  column["name"]))
            else:
                alias = item["alias"]
                if expression["t"] == "col":
                    real = self._column(definition, expression["name"])
                    items.append(({"t": "col", "name": real}, alias or real))
                elif expression["t"] == "l2":
                    real = self._column(definition, expression["col"])
                    items.append(({"t": "l2", "col": real,
                                   "vec": expression["vec"]}, alias or "distance"))
                else:
                    items.append((expression, alias or "value"))

        rows = self._visible(name, definition)
        l2_items = [(i, expr) for i, (expr, _) in enumerate(items)
                    if expr["t"] == "l2"]
        vectors_by_item = {}
        for index, expr in l2_items:
            if vector_col is None:
                raise DBError("invalid_vector", "table has no vector column")
            dimension = columns[expr["col"].lower()]["size"]
            vectors_by_item[index] = self._parse_vector(expr["vec"], dimension)

        # Filter first; TopK is then computed only over qualifying rows.
        filtered = [row for row in rows
                    if self._eval_where(stmt["where"], row, definition, {})]

        distance_lists = {}
        # `ann` mode may also return exact results (Recall@K = 1.0), so both
        # search modes intentionally share the same exact-scan implementation.
        use_numpy = (np is not None and len(filtered) >= 512
                     and bool(l2_items) and vector_col is not None)
        if use_numpy:
            matrix = self._matrix(name, filtered, vector_col, definition)
            for index, expr in l2_items:
                query = np.asarray(vectors_by_item[index], dtype=np.float32)
                diffs = matrix - query
                distance_lists[index] = np.sqrt(
                    np.square(diffs).sum(axis=1)).astype(np.float64).tolist()
        else:
            for index, expr in l2_items:
                query = vectors_by_item[index]
                distance_lists[index] = [
                    self._distance(row.get(expr["col"], []), query)
                    for row in filtered]

        projected = []
        for position, row in enumerate(filtered):
            values = []
            for index, (expr, _label) in enumerate(items):
                if expr["t"] == "col":
                    values.append(row.get(expr["name"]))
                elif expr["t"] == "l2":
                    values.append(distance_lists[index][position])
                else:
                    values.append(expr.get("value"))
            projected.append((row[pk_col], values))

        order = stmt["order_by"]
        if order:
            keys = []
            for spec in order:
                expr = spec["expr"]
                if expr["t"] == "col" and expr["name"].lower() in columns:
                    real = self._column(definition, expr["name"])
                    keys.append(("expr", {"t": "col", "name": real},
                                 spec["direction"]))
                elif expr["t"] == "col":
                    match = next((i for i, (_e, label) in enumerate(items)
                                  if label.lower() == expr["name"].lower()), None)
                    if match is None:
                        raise DBError("unknown_column",
                                      "unknown ORDER BY column %s" % expr["name"])
                    keys.append(("projected", match, spec["direction"]))
                elif expr["t"] == "l2":
                    real = self._column(definition, expr["col"])
                    keys.append(("expr", {"t": "l2", "col": real},
                                 spec["direction"]))
                else:
                    keys.append(("expr", expr, spec["direction"]))

            keyed = [(pk, values, position)
                     for position, (pk, values) in enumerate(projected)]

            def compare_rows(first, second):
                for source, key, direction in keys:
                    if source == "projected":
                        left, right = first[1][key], second[1][key]
                    else:
                        expr = key
                        if expr["t"] == "col":
                            row_a, row_b = filtered[first[2]], filtered[second[2]]
                            left, right = row_a.get(expr["name"]), row_b.get(expr["name"])
                        elif expr["t"] == "l2":
                            target = next((i for i, (e, _l) in enumerate(items)
                                           if e["t"] == "l2"
                                           and e["col"] == expr["col"]), None)
                            if target is None:
                                continue
                            left = distance_lists[target][first[2]]
                            right = distance_lists[target][second[2]]
                        else:
                            left = right = expr.get("value")
                    comparison = self._compare(left, right)
                    if direction == "DESC":
                        comparison = -comparison
                    if comparison:
                        return comparison
                return (first[0] > second[0]) - (first[0] < second[0])

            keyed.sort(key=functools.cmp_to_key(compare_rows))
            projected = [(pk, values) for pk, values, _pos in keyed]
        else:
            projected.sort(key=lambda pair: pair[0])

        limit = stmt["limit"]
        if limit is not None:
            projected = projected[:max(0, int(limit))]
        return {"columns": [label for _expr, label in items],
                "rows": [values for _pk, values in projected]}

    def _matrix(self, table, rows, vector_col, definition):
        key = (table, self.store.version, len(rows), self.tx is not None)
        cached = self._matrix_cache.get(key)
        if cached is not None:
            return cached
        matrix = np.asarray([row.get(vector_col) for row in rows],
                            dtype=np.float32)
        self._matrix_cache = {key: matrix}
        return matrix

    def _do_begin(self, _stmt, _mode):
        if self.tx is not None:
            raise DBError("nested_transaction", "transaction already active")
        self.tx = {"changes": [], "overlay": {}}
        self._matrix_cache.clear()
        return {}

    def _do_commit(self, _stmt, _mode):
        if self.tx is None:
            return {}
        changes = self.tx["changes"]
        self.tx = None
        if changes:
            self.store.commit(changes)
        self._matrix_cache.clear()
        return {}

    def _do_rollback(self, _stmt, _mode):
        self.tx = None
        self._matrix_cache.clear()
        return {}

    def shutdown(self):
        # An open transaction is deliberately never persisted.
        self.tx = None
        self.store.close()

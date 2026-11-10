"""SQL subset parser for MiniVectorDB.

Supported statements (see README.md):
  CREATE TABLE / CREATE VECTOR INDEX / INSERT / SELECT / UPDATE / DELETE
  BEGIN / COMMIT / ROLLBACK

The parser is deliberately small: it produces plain dictionaries consumed by
engine.py.  Anything outside the subset raises ParseError, which the engine
maps to the `unsupported_sql` error code.
"""
from __future__ import annotations

import re


class ParseError(Exception):
    pass


_TOKEN = re.compile(
    r"(?P<ws>\s+)"
    r"|(?P<string>'(?:[^']|'')*')"
    r"|(?P<number>\d+\.\d*(?:[eE][+-]?\d+)?|\.\d+(?:[eE][+-]?\d+)?|\d+(?:[eE][+-]?\d+)?)"
    r"|(?P<ident>[A-Za-z_][A-Za-z0-9_]*)"
    r"|(?P<op><=|>=|<>|!=|=|<|>)"
    r"|(?P<punct>[(),;*+\-\[\]])",
    re.VERBOSE,
)

_COMPARISONS = ("=", "<", "<=", ">", ">=", "!=", "<>")


def tokenize(sql):
    out = []
    pos = 0
    length = len(sql)
    while pos < length:
        match = _TOKEN.match(sql, pos)
        if not match:
            raise ParseError("unexpected character at offset %d" % pos)
        pos = match.end()
        kind = match.lastgroup
        text = match.group()
        if kind == "ws":
            continue
        if kind == "string":
            out.append(("string", text[1:-1].replace("''", "'")))
        elif kind == "number":
            if any(ch in text for ch in ".eE"):
                out.append(("number", float(text)))
            else:
                out.append(("number", int(text)))
        elif kind == "ident":
            out.append(("ident", text))
        elif kind == "op":
            out.append(("op", text))
        else:
            out.append((text, text))
    return out


class Parser:
    def __init__(self, sql):
        self.toks = tokenize(sql)
        self.i = 0

    def peek(self, ahead=0):
        j = self.i + ahead
        return self.toks[j] if j < len(self.toks) else (None, None)

    def take(self):
        tok = self.peek()
        self.i += 1
        return tok

    def at_kw(self, keyword):
        kind, value = self.peek()
        return kind == "ident" and value.upper() == keyword

    def accept_kw(self, keyword):
        if self.at_kw(keyword):
            self.i += 1
            return True
        return False

    def expect_kw(self, keyword):
        if not self.accept_kw(keyword):
            raise ParseError("expected keyword %s" % keyword)

    def at(self, ch):
        return self.peek()[0] == ch

    def accept(self, ch):
        if self.at(ch):
            self.i += 1
            return True
        return False

    def expect(self, ch):
        if not self.accept(ch):
            raise ParseError("expected %r" % ch)

    def accept_op(self, op):
        kind, value = self.peek()
        if kind == "op" and value == op:
            self.i += 1
            return True
        return False

    def expect_op(self, op):
        if not self.accept_op(op):
            raise ParseError("expected operator %r" % op)

    def ident(self):
        kind, value = self.take()
        if kind != "ident":
            raise ParseError("expected identifier, got %r" % (value,))
        return value

    def number(self):
        kind, value = self.take()
        if kind != "number":
            raise ParseError("expected number, got %r" % (value,))
        return value

    # ---- values and expressions -------------------------------------------

    def value(self):
        if self.accept("-"):
            return -self.number()
        if self.accept("+"):
            return self.number()
        kind, value = self.peek()
        if kind in ("number", "string"):
            self.take()
            return value
        if self.at("["):
            self.take()
            values = []
            if not self.at("]"):
                values.append(self.value())
                while self.accept(","):
                    values.append(self.value())
            self.expect("]")
            return values
        if self.at_kw("NULL"):
            self.take()
            return None
        if self.at_kw("TRUE"):
            self.take()
            return True
        if self.at_kw("FALSE"):
            self.take()
            return False
        raise ParseError("expected value, got %r" % (value,))

    def expr(self):
        if self.at_kw("L2_DISTANCE"):
            self.take()
            self.expect("(")
            column = self.ident()
            self.expect(",")
            vector = self.value()
            self.expect(")")
            return {"t": "l2", "col": column, "vec": vector}
        kind, value = self.peek()
        if kind == "ident":
            self.take()
            return {"t": "col", "name": value}
        return {"t": "lit", "value": self.value()}

    def condition(self):
        node = self.comparison()
        while self.accept_kw("AND"):
            node = {"t": "and", "children": [node, self.comparison()]}
        while self.accept_kw("OR"):
            node = {"t": "or", "children": [node, self.comparison()]}
        return node

    def comparison(self):
        left = self.expr()
        kind, value = self.peek()
        if kind != "op" or value not in _COMPARISONS:
            raise ParseError("expected comparison operator")
        self.take()
        right = self.expr()
        return {"t": "cmp", "left": left, "op": value, "right": right}

    def optional_where(self):
        if self.accept_kw("WHERE"):
            return self.condition()
        return None

    # ---- statements --------------------------------------------------------

    def statement(self):
        if self.at_kw("CREATE"):
            return self.create()
        if self.at_kw("INSERT"):
            return self.insert()
        if self.at_kw("SELECT"):
            return self.select()
        if self.at_kw("UPDATE"):
            return self.update()
        if self.at_kw("DELETE"):
            return self.delete()
        if self.accept_kw("BEGIN"):
            self.accept_kw("TRANSACTION")
            return {"kind": "begin"}
        if self.accept_kw("COMMIT"):
            return {"kind": "commit"}
        if self.accept_kw("ROLLBACK"):
            return {"kind": "rollback"}
        raise ParseError("unsupported statement")

    def create(self):
        self.expect_kw("CREATE")
        if self.accept_kw("TABLE"):
            return self.create_table()
        if self.accept_kw("VECTOR"):
            self.expect_kw("INDEX")
            name = self.ident()
            self.expect_kw("ON")
            table = self.ident()
            self.expect("(")
            column = self.ident()
            self.expect(")")
            return {"kind": "create_vector_index", "name": name,
                    "table": table, "column": column}
        raise ParseError("unsupported CREATE statement")

    def create_table(self):
        name = self.ident()
        self.expect("(")
        columns = []
        while True:
            col_name = self.ident()
            type_name = self.ident().upper()
            size = None
            if self.accept("("):
                size = self.number()
                self.expect(")")
            primary = False
            if self.accept_kw("PRIMARY"):
                self.expect_kw("KEY")
                primary = True
            if type_name not in ("BIGINT", "VARCHAR", "VECTOR"):
                raise ParseError("unsupported column type %s" % type_name)
            columns.append({"name": col_name, "type": type_name,
                            "size": size, "primary": primary})
            if not self.accept(","):
                break
        self.expect(")")
        return {"kind": "create_table", "table": name, "columns": columns}

    def insert(self):
        self.expect_kw("INSERT")
        self.expect_kw("INTO")
        table = self.ident()
        columns = None
        if self.at("("):
            self.take()
            columns = [self.ident()]
            while self.accept(","):
                columns.append(self.ident())
            self.expect(")")
        self.expect_kw("VALUES")
        rows = []
        while True:
            self.expect("(")
            values = [self.value()]
            while self.accept(","):
                values.append(self.value())
            self.expect(")")
            rows.append(values)
            if not self.accept(","):
                break
        return {"kind": "insert", "table": table, "columns": columns,
                "rows": rows}

    def select(self):
        self.expect_kw("SELECT")
        items = []
        while True:
            if self.accept("*"):
                items.append({"expr": {"t": "star"}, "alias": None})
            else:
                expression = self.expr()
                alias = None
                if self.accept_kw("AS"):
                    alias = self.ident()
                items.append({"expr": expression, "alias": alias})
            if not self.accept(","):
                break
        self.expect_kw("FROM")
        table = self.ident()
        where = self.optional_where()
        order_by = []
        if self.accept_kw("ORDER"):
            self.expect_kw("BY")
            while True:
                expression = self.expr()
                direction = "DESC" if self.accept_kw("DESC") else "ASC"
                if direction == "ASC":
                    self.accept_kw("ASC")
                order_by.append({"expr": expression, "direction": direction})
                if not self.accept(","):
                    break
        limit = None
        if self.accept_kw("LIMIT"):
            limit = self.number()
        return {"kind": "select", "table": table, "items": items,
                "where": where, "order_by": order_by, "limit": limit}

    def update(self):
        self.expect_kw("UPDATE")
        table = self.ident()
        self.expect_kw("SET")
        assignments = []
        while True:
            column = self.ident()
            self.expect_op("=")
            assignments.append((column, self.value()))
            if not self.accept(","):
                break
        where = self.optional_where()
        return {"kind": "update", "table": table,
                "assignments": assignments, "where": where}

    def delete(self):
        self.expect_kw("DELETE")
        self.expect_kw("FROM")
        table = self.ident()
        where = self.optional_where()
        return {"kind": "delete", "table": table, "where": where}


def parse(sql):
    parser = Parser(sql)
    statement = parser.statement()
    parser.accept(";")
    if parser.i < len(parser.toks):
        raise ParseError("trailing tokens after statement")
    return statement

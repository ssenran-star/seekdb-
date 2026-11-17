#include "parser.h"

#include <cctype>
#include <cstdlib>

namespace mvdb {
namespace {

struct Token {
    enum Kind { IDENT, NUMBER, STRING, PUNCT, END } kind = END;
    std::string text;
    int64_t ival = 0;
    double dval = 0;
    bool is_float = false;
};

std::string upper(const std::string& s) {
    std::string r = s;
    for (char& c : r) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return r;
}

class Lexer {
public:
    explicit Lexer(const std::string& sql) : s_(sql) {}

    Token next() {
        skip_ws();
        Token t;
        if (pos_ >= s_.size()) {
            t.kind = Token::END;
            return t;
        }
        char c = s_[pos_];
        if (c == '\'') {
            t.kind = Token::STRING;
            ++pos_;
            size_t start = pos_;
            bool escaped = false;
            while (true) {
                if (pos_ >= s_.size()) throw DbError("syntax_error", "unterminated string");
                if (s_[pos_] == '\'') {
                    if (pos_ + 1 < s_.size() && s_[pos_ + 1] == '\'') {
                        escaped = true;
                        pos_ += 2;
                    } else {
                        break;
                    }
                } else {
                    ++pos_;
                }
            }
            if (!escaped) {
                t.text.assign(s_, start, pos_ - start);
            } else {
                t.text.clear();
                t.text.reserve(pos_ - start);
                for (size_t i = start; i < pos_; ) {
                    if (s_[i] == '\'' && i + 1 < pos_ && s_[i + 1] == '\'') {
                        t.text += '\'';
                        i += 2;
                    } else {
                        t.text += s_[i++];
                    }
                }
            }
            ++pos_;  // closing quote
            return t;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '-' && pos_ + 1 < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_ + 1])))) {
            size_t start = pos_;
            if (c == '-') ++pos_;
            bool is_float = false;
            while (pos_ < s_.size() &&
                   (std::isdigit(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '.' ||
                    s_[pos_] == 'e' || s_[pos_] == 'E' ||
                    ((s_[pos_] == '+' || s_[pos_] == '-') && pos_ > start &&
                     (s_[pos_ - 1] == 'e' || s_[pos_ - 1] == 'E')))) {
                if (s_[pos_] == '.' || s_[pos_] == 'e' || s_[pos_] == 'E') is_float = true;
                ++pos_;
            }
            t.text = s_.substr(start, pos_ - start);
            t.is_float = is_float;
            if (is_float) {
                t.kind = Token::NUMBER;
                t.dval = std::strtod(t.text.c_str(), nullptr);
            } else {
                t.kind = Token::NUMBER;
                errno = 0;
                char* end = nullptr;
                long long v = std::strtoll(t.text.c_str(), &end, 10);
                if (errno == ERANGE || *end != '\0')
                    throw DbError("bigint_overflow", "integer out of range: " + t.text);
                t.ival = v;
            }
            return t;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            size_t start = pos_;
            while (pos_ < s_.size() &&
                   (std::isalnum(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '_'))
                ++pos_;
            t.kind = Token::IDENT;
            t.text = s_.substr(start, pos_ - start);
            return t;
        }
        if (c == '<' || c == '>') {
            t.kind = Token::PUNCT;
            t.text = std::string(1, c);
            ++pos_;
            if (pos_ < s_.size() && s_[pos_] == '=') {
                t.text += '=';
                ++pos_;
            }
            return t;
        }
        if (std::string("(),;=*.+-").find(c) != std::string::npos) {
            t.kind = Token::PUNCT;
            t.text = std::string(1, c);
            ++pos_;
            return t;
        }
        throw DbError("syntax_error", std::string("unexpected character: ") + c);
    }

private:
    void skip_ws() {
        while (pos_ < s_.size() &&
               (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\r' || s_[pos_] == '\n'))
            ++pos_;
    }
    const std::string& s_;
    size_t pos_ = 0;
};

class Parser {
public:
    explicit Parser(const std::string& sql) : lex_(sql) { advance(); }

    Statement parse() {
        Statement st;
        if (tok_.kind != Token::IDENT) throw DbError("syntax_error", "expected statement");
        std::string kw = upper(tok_.text);
        if (kw == "CREATE") {
            advance();
            if (accept_kw("TABLE")) {
                st.type = StmtType::CREATE_TABLE;
                parse_create_table(st);
            } else if (accept_kw("VECTOR")) {
                expect_kw("INDEX");
                st.type = StmtType::CREATE_INDEX;
                st.index_name = expect_ident();
                expect_kw("ON");
                st.table = expect_ident();
                expect_punct("(");
                st.column = expect_ident();
                expect_punct(")");
            } else {
                throw DbError("unsupported_sql", "unsupported CREATE target");
            }
        } else if (kw == "INSERT") {
            st.type = StmtType::INSERT;
            advance();
            expect_kw("INTO");
            st.table = expect_ident();
            if (accept_punct("(")) {
                while (true) {
                    st.insert_cols.push_back(expect_ident());
                    if (!accept_punct(",")) break;
                }
                expect_punct(")");
            }
            expect_kw("VALUES");
            while (true) {
                expect_punct("(");
                std::vector<Literal> row;
                while (true) {
                    row.push_back(parse_literal());
                    if (!accept_punct(",")) break;
                }
                expect_punct(")");
                st.rows.push_back(std::move(row));
                if (!accept_punct(",")) break;
            }
        } else if (kw == "SELECT") {
            st.type = StmtType::SELECT;
            advance();
            parse_select(st);
        } else if (kw == "UPDATE") {
            st.type = StmtType::UPDATE;
            advance();
            st.table = expect_ident();
            expect_kw("SET");
            while (true) {
                std::string col = expect_ident();
                expect_punct("=");
                Literal v = parse_literal();
                st.sets.emplace_back(col, std::move(v));
                if (!accept_punct(",")) break;
            }
            if (accept_kw("WHERE")) parse_where(st);
        } else if (kw == "DELETE") {
            st.type = StmtType::DELETE_ROWS;
            advance();
            expect_kw("FROM");
            st.table = expect_ident();
            if (accept_kw("WHERE")) parse_where(st);
        } else if (kw == "BEGIN") {
            st.type = StmtType::BEGIN;
            advance();
            accept_kw("TRANSACTION");
        } else if (kw == "COMMIT") {
            st.type = StmtType::COMMIT;
            advance();
        } else if (kw == "ROLLBACK") {
            st.type = StmtType::ROLLBACK;
            advance();
        } else if (kw == "ALTER" || kw == "DROP" || kw == "TRUNCATE" || kw == "GRANT" ||
                   kw == "EXPLAIN" || kw == "SHOW" || kw == "USE" || kw == "SET") {
            throw DbError("unsupported_sql", "unsupported statement: " + tok_.text);
        } else {
            throw DbError("syntax_error", "unknown statement: " + tok_.text);
        }
        accept_punct(";");
        if (tok_.kind != Token::END) throw DbError("syntax_error", "trailing tokens");
        return st;
    }

private:
    void advance() { tok_ = lex_.next(); }

    bool accept_kw(const char* kw) {
        if (tok_.kind == Token::IDENT && upper(tok_.text) == kw) {
            advance();
            return true;
        }
        return false;
    }
    void expect_kw(const char* kw) {
        if (!accept_kw(kw)) throw DbError("syntax_error", std::string("expected ") + kw);
    }
    bool accept_punct(const char* p) {
        if (tok_.kind == Token::PUNCT && tok_.text == p) {
            advance();
            return true;
        }
        return false;
    }
    void expect_punct(const char* p) {
        if (!accept_punct(p)) throw DbError("syntax_error", std::string("expected ") + p);
    }
    std::string expect_ident() {
        if (tok_.kind != Token::IDENT) throw DbError("syntax_error", "expected identifier");
        std::string s = tok_.text;
        advance();
        return s;
    }

    Literal parse_literal() {
        Literal lit;
        if (tok_.kind == Token::NUMBER) {
            if (tok_.is_float) {
                lit.kind = Literal::FLOAT;
                lit.d = tok_.dval;
            } else {
                lit.kind = Literal::INT;
                lit.i = tok_.ival;
            }
            advance();
            return lit;
        }
        if (tok_.kind == Token::STRING) {
            lit.kind = Literal::STRING;
            lit.s = tok_.text;
            advance();
            return lit;
        }
        if (tok_.kind == Token::IDENT && upper(tok_.text) == "NULL")
            throw DbError("unsupported_sql", "NULL is not supported");
        throw DbError("syntax_error", "expected literal");
    }

    void parse_create_table(Statement& st) {
        st.table = expect_ident();
        expect_punct("(");
        while (true) {
            ColumnDef col;
            col.name = expect_ident();
            std::string ty = upper(expect_ident());
            if (ty == "BIGINT") {
                col.type = ValType::BIGINT;
            } else if (ty == "VARCHAR") {
                col.type = ValType::VARCHAR;
                expect_punct("(");
                if (tok_.kind != Token::NUMBER || tok_.is_float || tok_.ival <= 0)
                    throw DbError("syntax_error", "VARCHAR length must be positive integer");
                col.len = static_cast<uint32_t>(tok_.ival);
                advance();
                expect_punct(")");
            } else if (ty == "VECTOR") {
                col.type = ValType::VECTOR;
                expect_punct("(");
                if (tok_.kind != Token::NUMBER || tok_.is_float || tok_.ival <= 0)
                    throw DbError("syntax_error", "VECTOR dim must be positive integer");
                col.len = static_cast<uint32_t>(tok_.ival);
                advance();
                expect_punct(")");
            } else {
                throw DbError("unsupported_sql", "unsupported type: " + ty);
            }
            if (accept_kw("PRIMARY")) {
                expect_kw("KEY");
                if (!st.pk_col.empty()) throw DbError("unsupported_sql", "multiple primary keys");
                if (col.type != ValType::BIGINT)
                    throw DbError("unsupported_sql", "primary key must be BIGINT");
                st.pk_col = col.name;
            } else {
                accept_kw("NOT");
                accept_kw("NULL");
            }
            st.cols.push_back(std::move(col));
            if (!accept_punct(",")) break;
        }
        expect_punct(")");
        if (st.pk_col.empty()) throw DbError("syntax_error", "BIGINT PRIMARY KEY required");
    }

    void parse_select(Statement& st) {
        if (accept_punct("*")) {
            st.star = true;
        } else {
            while (true) {
                SelectItem item;
                if (tok_.kind == Token::IDENT && upper(tok_.text) == "L2_DISTANCE") {
                    advance();
                    expect_punct("(");
                    item.is_distance = true;
                    item.column = expect_ident();
                    expect_punct(",");
                    if (tok_.kind != Token::STRING)
                        throw DbError("syntax_error", "L2_DISTANCE needs vector literal");
                    item.raw_vec = tok_.text;
                    advance();
                    expect_punct(")");
                } else {
                    std::string kw = tok_.kind == Token::IDENT ? upper(tok_.text) : "";
                    if (kw == "JOIN" || kw == "INNER" || kw == "LEFT" || kw == "RIGHT" ||
                        kw == "GROUP" || kw == "HAVING" || kw == "DISTINCT" || kw == "COUNT" ||
                        kw == "SUM" || kw == "AVG" || kw == "MIN" || kw == "MAX")
                        throw DbError("unsupported_sql", "unsupported SELECT feature");
                    item.column = expect_ident();
                }
                if (accept_kw("AS")) {
                    item.alias = expect_ident();
                } else if (tok_.kind == Token::IDENT && upper(tok_.text) != "FROM" &&
                           upper(tok_.text) != "WHERE" && upper(tok_.text) != "ORDER" &&
                           upper(tok_.text) != "LIMIT") {
                    item.alias = expect_ident();
                }
                st.items.push_back(std::move(item));
                if (!accept_punct(",")) break;
            }
        }
        expect_kw("FROM");
        if (tok_.kind == Token::IDENT && upper(tok_.text) == "SELECT")
            throw DbError("unsupported_sql", "subqueries not supported");
        st.table = expect_ident();
        if (tok_.kind == Token::PUNCT && tok_.text == ",")
            throw DbError("unsupported_sql", "JOIN not supported");
        if (accept_kw("WHERE")) parse_where(st);
        if (accept_kw("ORDER")) {
            expect_kw("BY");
            st.has_order = true;
            st.order_by = expect_ident();
            if (accept_kw("DESC")) {
                st.order_desc = true;
            } else {
                accept_kw("ASC");
            }
        }
        if (accept_kw("LIMIT")) {
            if (tok_.kind != Token::NUMBER || tok_.is_float || tok_.ival < 0)
                throw DbError("syntax_error", "LIMIT must be non-negative integer");
            st.has_limit = true;
            st.limit = tok_.ival;
            advance();
        }
        if (tok_.kind == Token::IDENT) {
            std::string kw = upper(tok_.text);
            if (kw == "JOIN" || kw == "GROUP" || kw == "HAVING" || kw == "UNION" || kw == "OFFSET")
                throw DbError("unsupported_sql", "unsupported SELECT feature: " + kw);
        }
    }

    void parse_where(Statement& st) {
        while (true) {
            std::string kw = tok_.kind == Token::IDENT ? upper(tok_.text) : "";
            if (kw == "OR" || kw == "LIKE" || kw == "IN" || kw == "BETWEEN" || kw == "IS" ||
                kw == "NOT" || kw == "EXISTS")
                throw DbError("unsupported_sql", "unsupported WHERE feature");
            Predicate p;
            p.column = expect_ident();
            if (tok_.kind != Token::PUNCT ||
                (tok_.text != "=" && tok_.text != "<" && tok_.text != "<=" &&
                 tok_.text != ">" && tok_.text != ">="))
                throw DbError("syntax_error", "expected comparison operator");
            p.op = tok_.text;
            advance();
            p.literal = parse_literal();
            st.where.push_back(std::move(p));
            if (!accept_kw("AND")) break;
        }
    }

    Lexer lex_;
    Token tok_;
};

}  // namespace

bool try_parse_l2_select(const std::string& sql, Statement& st) {
    const char* s = sql.data();
    const char* e = s + sql.size();
    auto skip = [&]() {
        while (s < e && is_ascii_space(*s)) ++s;
    };
    auto kw = [&](const char* k) -> bool {
        skip();
        const char* p = s;
        for (; *k; ++k, ++p) {
            if (p >= e) return false;
            char c = *p;
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
            if (c != *k) return false;
        }
        if (p < e) {
            unsigned char u = static_cast<unsigned char>(*p);
            if (std::isalnum(u) || *p == '_') return false;
        }
        s = p;
        return true;
    };
    auto ident = [&](std::string& out) -> bool {
        skip();
        if (s >= e) return false;
        unsigned char u = static_cast<unsigned char>(*s);
        if (!(std::isalpha(u) || *s == '_')) return false;
        const char* b = s++;
        while (s < e) {
            u = static_cast<unsigned char>(*s);
            if (!(std::isalnum(u) || *s == '_')) break;
            ++s;
        }
        out.assign(b, s);
        return true;
    };
    auto punct = [&](char c) -> bool {
        skip();
        if (s < e && *s == c) {
            ++s;
            return true;
        }
        return false;
    };

    if (!kw("SELECT")) return false;
    Statement tmp;
    tmp.type = StmtType::SELECT;
    SelectItem id_item;
    if (!ident(id_item.column)) return false;
    if (kw("AS")) {
        if (!ident(id_item.alias)) return false;
    }
    tmp.items.push_back(std::move(id_item));
    if (!punct(',')) return false;
    if (!kw("L2_DISTANCE") || !punct('(')) return false;
    SelectItem ditem;
    ditem.is_distance = true;
    if (!ident(ditem.column) || !punct(',')) return false;
    skip();
    if (s >= e || *s != '\'') return false;
    ++s;
    const char* vs = s;
    while (s < e && *s != '\'') ++s;
    if (s >= e) return false;
    ditem.query_vec = parse_vector_literal(vs, s, 0);
    ++s;
    if (!punct(')')) return false;
    if (kw("AS")) {
        if (!ident(ditem.alias)) return false;
    } else {
        skip();
        if (s < e) {
            unsigned char u = static_cast<unsigned char>(*s);
            if (std::isalpha(u) || *s == '_') {
                const char* p = s;
                const char* b = p++;
                while (p < e) {
                    u = static_cast<unsigned char>(*p);
                    if (!(std::isalnum(u) || *p == '_')) break;
                    ++p;
                }
                std::string maybe(b, p);
                std::string up = maybe;
                for (char& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                if (up != "FROM" && up != "WHERE" && up != "ORDER" && up != "LIMIT") {
                    ditem.alias = std::move(maybe);
                    s = p;
                }
            }
        }
    }
    tmp.items.push_back(std::move(ditem));
    if (!kw("FROM") || !ident(tmp.table)) return false;
    if (kw("WHERE")) {
        Predicate pred;
        if (!ident(pred.column)) return false;
        skip();
        if (s >= e || *s != '=') return false;
        ++s;
        pred.op = "=";
        skip();
        if (s >= e || *s != '\'') return false;
        ++s;
        const char* a = s;
        while (s < e && *s != '\'') ++s;
        if (s >= e) return false;
        pred.literal.kind = Literal::STRING;
        pred.literal.s.assign(a, s);
        ++s;
        tmp.where.push_back(std::move(pred));
    }
    if (!kw("ORDER") || !kw("BY")) return false;
    tmp.has_order = true;
    if (!ident(tmp.order_by)) return false;
    if (kw("DESC")) return false;
    kw("ASC");
    if (!kw("LIMIT")) return false;
    skip();
    if (s >= e || *s < '0' || *s > '9') return false;
    int64_t lim = 0;
    while (s < e && *s >= '0' && *s <= '9') {
        lim = lim * 10 + (*s - '0');
        ++s;
    }
    tmp.has_limit = true;
    tmp.limit = lim;
    skip();
    if (s < e && *s == ';') ++s;
    skip();
    if (s < e) return false;
    st = std::move(tmp);
    return true;
}

Statement parse_sql(const std::string& sql) {
    Statement st;
    if (try_parse_l2_select(sql, st)) return st;
    std::string t = trim(sql);
    if (t.empty()) throw DbError("syntax_error", "empty statement");
    Parser p(t);
    return p.parse();
}

}  // namespace mvdb
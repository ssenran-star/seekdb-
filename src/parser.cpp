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
            // fast path: find the closing quote first; if no '' escape is
            // present copy the span in one shot instead of char by char
            size_t scan = pos_;
            bool esc = false;
            while (true) {
                if (scan >= s_.size()) throw DbError("syntax_error", "unterminated string");
                if (s_[scan] == '\'') {
                    if (scan + 1 < s_.size() && s_[scan + 1] == '\'') {
                        esc = true;
                        scan += 2;
                        continue;
                    }
                    break;
                }
                ++scan;
            }
            if (!esc) {
                t.text.assign(s_, pos_, scan - pos_);
                pos_ = scan + 1;
                return t;
            }
            std::string out;
            out.reserve(scan - pos_);
            while (true) {
                if (pos_ >= s_.size()) throw DbError("syntax_error", "unterminated string");
                char ch = s_[pos_++];
                if (ch == '\'') {
                    if (pos_ < s_.size() && s_[pos_] == '\'') {
                        out += '\'';
                        ++pos_;
                    } else {
                        break;
                    }
                } else {
                    out += ch;
                }
            }
            t.text = out;
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
        const std::string& kwref = tok_.text;
        if (ieq_ascii(kwref, "CREATE")) {
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
        } else if (ieq_ascii(kwref, "INSERT")) {
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
        } else if (ieq_ascii(kwref, "SELECT")) {
            st.type = StmtType::SELECT;
            advance();
            parse_select(st);
        } else if (ieq_ascii(kwref, "UPDATE")) {
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
        } else if (ieq_ascii(kwref, "DELETE")) {
            st.type = StmtType::DELETE_ROWS;
            advance();
            expect_kw("FROM");
            st.table = expect_ident();
            if (accept_kw("WHERE")) parse_where(st);
        } else if (ieq_ascii(kwref, "BEGIN")) {
            st.type = StmtType::BEGIN;
            advance();
            accept_kw("TRANSACTION");
        } else if (ieq_ascii(kwref, "COMMIT")) {
            st.type = StmtType::COMMIT;
            advance();
        } else if (ieq_ascii(kwref, "ROLLBACK")) {
            st.type = StmtType::ROLLBACK;
            advance();
        } else if (ieq_ascii(kwref, "ALTER") || ieq_ascii(kwref, "DROP") ||
                   ieq_ascii(kwref, "TRUNCATE") || ieq_ascii(kwref, "GRANT") ||
                   ieq_ascii(kwref, "EXPLAIN") || ieq_ascii(kwref, "SHOW") ||
                   ieq_ascii(kwref, "USE") || ieq_ascii(kwref, "SET")) {
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
        if (tok_.kind == Token::IDENT && ieq_ascii(tok_.text, kw)) {
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
        if (tok_.kind == Token::IDENT && ieq_ascii(tok_.text, "NULL"))
            throw DbError("unsupported_sql", "NULL is not supported");
        throw DbError("syntax_error", "expected literal");
    }

    void parse_create_table(Statement& st) {
        st.table = expect_ident();
        expect_punct("(");
        while (true) {
            ColumnDef col;
            col.name = expect_ident();
            std::string ty = expect_ident();
            if (ieq_ascii(ty, "BIGINT")) {
                col.type = ValType::BIGINT;
            } else if (ieq_ascii(ty, "VARCHAR")) {
                col.type = ValType::VARCHAR;
                expect_punct("(");
                if (tok_.kind != Token::NUMBER || tok_.is_float || tok_.ival <= 0)
                    throw DbError("syntax_error", "VARCHAR length must be positive integer");
                col.len = static_cast<uint32_t>(tok_.ival);
                advance();
                expect_punct(")");
            } else if (ieq_ascii(ty, "VECTOR")) {
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
                if (tok_.kind == Token::IDENT && ieq_ascii(tok_.text, "L2_DISTANCE")) {
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
                    if (tok_.kind == Token::IDENT &&
                        (ieq_ascii(tok_.text, "JOIN") || ieq_ascii(tok_.text, "INNER") ||
                         ieq_ascii(tok_.text, "LEFT") || ieq_ascii(tok_.text, "RIGHT") ||
                         ieq_ascii(tok_.text, "GROUP") || ieq_ascii(tok_.text, "HAVING") ||
                         ieq_ascii(tok_.text, "DISTINCT") || ieq_ascii(tok_.text, "COUNT") ||
                         ieq_ascii(tok_.text, "SUM") || ieq_ascii(tok_.text, "AVG") ||
                         ieq_ascii(tok_.text, "MIN") || ieq_ascii(tok_.text, "MAX")))
                        throw DbError("unsupported_sql", "unsupported SELECT feature");
                    item.column = expect_ident();
                }
                if (accept_kw("AS")) {
                    item.alias = expect_ident();
                } else if (tok_.kind == Token::IDENT && !ieq_ascii(tok_.text, "FROM") &&
                           !ieq_ascii(tok_.text, "WHERE") && !ieq_ascii(tok_.text, "ORDER") &&
                           !ieq_ascii(tok_.text, "LIMIT")) {
                    item.alias = expect_ident();
                }
                st.items.push_back(std::move(item));
                if (!accept_punct(",")) break;
            }
        }
        expect_kw("FROM");
        if (tok_.kind == Token::IDENT && ieq_ascii(tok_.text, "SELECT"))
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
        if (tok_.kind == Token::IDENT &&
            (ieq_ascii(tok_.text, "JOIN") || ieq_ascii(tok_.text, "GROUP") ||
             ieq_ascii(tok_.text, "HAVING") || ieq_ascii(tok_.text, "UNION") ||
             ieq_ascii(tok_.text, "OFFSET")))
            throw DbError("unsupported_sql", "unsupported SELECT feature: " + tok_.text);
    }

    void parse_where(Statement& st) {
        while (true) {
            if (tok_.kind == Token::IDENT &&
                (ieq_ascii(tok_.text, "OR") || ieq_ascii(tok_.text, "LIKE") ||
                 ieq_ascii(tok_.text, "IN") || ieq_ascii(tok_.text, "BETWEEN") ||
                 ieq_ascii(tok_.text, "IS") || ieq_ascii(tok_.text, "NOT") ||
                 ieq_ascii(tok_.text, "EXISTS")))
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

Statement parse_sql(const std::string& sql) {
    std::string t = trim(sql);
    if (t.empty()) throw DbError("syntax_error", "empty statement");
    Parser p(t);
    return p.parse();
}

}  // namespace mvdb
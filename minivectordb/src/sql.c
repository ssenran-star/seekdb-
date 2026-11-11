/* SQL front end for the supported subset.
 *
 * Supported: CREATE TABLE, CREATE VECTOR INDEX, INSERT, SELECT (projections,
 * comparisons, AND, ORDER BY, LIMIT), UPDATE, DELETE, BEGIN/COMMIT/ROLLBACK.
 * Everything outside the subset (ALTER TABLE, JOIN, aggregates, subqueries,
 * NULL, foreign keys, nested transactions, primary-key updates) is rejected
 * with an explicit error instead of being silently reinterpreted.
 */
#include "db.h"

#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

typedef enum { T_EOF = 0, T_IDENT, T_INT, T_DOUBLE, T_STRING, T_PUNCT } TokType;

typedef struct {
    TokType type;
    char text[80];
    int64_t ival;
    double dval;
    char *sval;
    size_t slen;
} Token;

typedef struct {
    const char *p;
    const char *end;
    Token tok;
    Token ahead;
    int has_ahead;
    MvError *err;
} Parser;

static int fail(Parser *P, const char *code, const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    mv_err_set(P->err, code, "%s", buf);
    return MV_ERR;
}

static int tok_is_kw(const Token *t, const char *kw) {
    if (t->type != T_IDENT) return 0;
    size_t n = strlen(kw);
    if (strlen(t->text) != n) return 0;
    for (size_t i = 0; i < n; i++) {
        if (toupper((unsigned char)t->text[i]) != toupper((unsigned char)kw[i])) return 0;
    }
    return 1;
}

static int tok_is_punct(const Token *t, const char *p) {
    return t->type == T_PUNCT && strcmp(t->text, p) == 0;
}

/* --------------------------------------------------------------- lexer */
static void skip_space(Parser *P) {
    while (P->p < P->end) {
        char c = *P->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            P->p++;
        } else if (c == '#') {
            while (P->p < P->end && *P->p != '\n') P->p++;
        } else if (c == '-' && P->p + 1 < P->end && P->p[1] == '-') {
            P->p += 2;
            while (P->p < P->end && *P->p != '\n') P->p++;
        } else if (c == '/' && P->p + 1 < P->end && P->p[1] == '*') {
            P->p += 2;
            while (P->p + 1 < P->end && !(P->p[0] == '*' && P->p[1] == '/')) P->p++;
            if (P->p + 1 < P->end) P->p += 2;
            else P->p = P->end;
        } else {
            break;
        }
    }
}

static int lex(Parser *P, Token *t) {
    memset(t, 0, sizeof(*t));
    skip_space(P);
    if (P->p >= P->end) {
        t->type = T_EOF;
        return MV_OK;
    }
    char c = *P->p;
    if (isalpha((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80) {
        const char *s = P->p;
        while (P->p < P->end) {
            char d = *P->p;
            if (isalnum((unsigned char)d) || d == '_' || d == '$' || (unsigned char)d >= 0x80) P->p++;
            else break;
        }
        size_t n = (size_t)(P->p - s);
        if (n >= sizeof(t->text)) n = sizeof(t->text) - 1;
        memcpy(t->text, s, n);
        t->text[n] = '\0';
        t->type = T_IDENT;
        return MV_OK;
    }
    if (isdigit((unsigned char)c) ||
        ((c == '-' || c == '+') && P->p + 1 < P->end && isdigit((unsigned char)P->p[1])) ||
        (c == '.' && P->p + 1 < P->end && isdigit((unsigned char)P->p[1]))) {
        const char *s = P->p;
        if (c == '-' || c == '+') P->p++;
        int isdbl = 0;
        while (P->p < P->end) {
            char d = *P->p;
            if (isdigit((unsigned char)d)) {
                P->p++;
            } else if (d == '.' || d == 'e' || d == 'E') {
                isdbl = 1;
                P->p++;
            } else if ((d == '+' || d == '-') && (P->p[-1] == 'e' || P->p[-1] == 'E')) {
                P->p++;
            } else {
                break;
            }
        }
        size_t n = (size_t)(P->p - s);
        char tmp[64];
        if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
        memcpy(tmp, s, n);
        tmp[n] = '\0';
        if (!isdbl) {
            char *endp = NULL;
            errno = 0;
            long long v = strtoll(tmp, &endp, 10);
            if (endp && *endp == '\0' && errno != ERANGE) {
                t->type = T_INT;
                t->ival = (int64_t)v;
                return MV_OK;
            }
        }
        t->type = T_DOUBLE;
        t->dval = strtod(tmp, NULL);
        return MV_OK;
    }
    if (c == '\'' || c == '"') {
        char quote = c;
        P->p++;
        MvBuf b;
        mv_buf_init(&b);
        while (P->p < P->end) {
            char d = *P->p++;
            if (d == quote) {
                if (P->p < P->end && *P->p == quote) {
                    mv_buf_append(&b, &d, 1);
                    P->p++;
                    continue;
                }
                t->type = T_STRING;
                t->sval = b.data ? b.data : mv_strdup("");
                t->slen = b.len;
                return MV_OK;
            }
            if (d == '\\' && P->p < P->end) {
                char e = *P->p++;
                switch (e) {
                    case 'n': mv_buf_append(&b, "\n", 1); break;
                    case 't': mv_buf_append(&b, "\t", 1); break;
                    case 'r': mv_buf_append(&b, "\r", 1); break;
                    case '0': mv_buf_append(&b, "\0", 1); break;
                    case '\\': mv_buf_append(&b, "\\", 1); break;
                    case '\'': mv_buf_append(&b, "'", 1); break;
                    case '"': mv_buf_append(&b, "\"", 1); break;
                    default: mv_buf_append(&b, &e, 1); break;
                }
                continue;
            }
            mv_buf_append(&b, &d, 1);
        }
        mv_buf_free(&b);
        return fail(P, MV_E_SYNTAX, "unterminated string literal");
    }
    if ((c == '<' || c == '>' || c == '!') && P->p + 1 < P->end &&
        (P->p[1] == '=' || (c == '<' && P->p[1] == '>'))) {
        t->type = T_PUNCT;
        t->text[0] = c;
        t->text[1] = P->p[1];
        t->text[2] = '\0';
        P->p += 2;
        return MV_OK;
    }
    if (strchr("(),;=*.[+-/<>", c)) {
        t->type = T_PUNCT;
        t->text[0] = c;
        t->text[1] = '\0';
        P->p++;
        return MV_OK;
    }
    return fail(P, MV_E_SYNTAX, "unexpected character '%c'", c);
}

static void advance(Parser *P) {
    if (P->has_ahead) {
        P->tok = P->ahead;
        P->has_ahead = 0;
        return;
    }
    lex(P, &P->tok);
}

static Token *peek(Parser *P) {
    if (!P->has_ahead) {
        lex(P, &P->ahead);
        P->has_ahead = 1;
    }
    return &P->ahead;
}

static int accept_kw(Parser *P, const char *kw) {
    if (!tok_is_kw(&P->tok, kw)) return 0;
    advance(P);
    return 1;
}

static int accept_punct(Parser *P, const char *p) {
    if (!tok_is_punct(&P->tok, p)) return 0;
    advance(P);
    return 1;
}

static int expect_punct(Parser *P, const char *p) {
    if (!accept_punct(P, p))
        return fail(P, MV_E_SYNTAX, "expected '%s' but found '%s'", p,
                    P->tok.type == T_EOF ? "<end of statement>" : P->tok.text);
    return MV_OK;
}

static int expect_ident(Parser *P, char *out, size_t outlen) {
    if (P->tok.type != T_IDENT)
        return fail(P, MV_E_SYNTAX, "expected identifier but found '%s'",
                    P->tok.type == T_EOF ? "<end of statement>" : P->tok.text);
    snprintf(out, outlen, "%s", P->tok.text);
    advance(P);
    return MV_OK;
}

/* --------------------------------------------------------------- values */
static int parse_number_value(Parser *P, double *out) {
    if (P->tok.type == T_INT) {
        *out = (double)P->tok.ival;
        advance(P);
        return MV_OK;
    }
    if (P->tok.type == T_DOUBLE) {
        *out = P->tok.dval;
        advance(P);
        return MV_OK;
    }
    return fail(P, MV_E_SYNTAX, "expected a number");
}

static int parse_vector_literal(Parser *P, MvValue *out) {
    float *vals = NULL;
    int32_t count = 0, cap = 0;
    for (;;) {
        double d = 0;
        if (parse_number_value(P, &d) != MV_OK) {
            free(vals);
            return fail(P, MV_E_BAD_VECTOR, "vector elements must be finite numbers");
        }
        if (!isfinite(d) || fabs(d) > (double)FLT_MAX) {
            free(vals);
            return fail(P, MV_E_BAD_VECTOR, "vector element is not a finite float32 value");
        }
        if (count == cap) {
            cap = cap ? cap * 2 : 8;
            vals = mv_xrealloc(vals, (size_t)cap * sizeof(float));
        }
        vals[count++] = (float)d;
        if (accept_punct(P, ",")) continue;
        if (expect_punct(P, "]") != MV_OK) {
            free(vals);
            return MV_ERR;
        }
        break;
    }
    out->type = MV_VAL_VEC;
    out->dim = count;
    out->vec = vals;
    return MV_OK;
}

static int parse_value(Parser *P, MvValue *out) {
    memset(out, 0, sizeof(*out));
    if (tok_is_kw(&P->tok, "NULL")) return fail(P, MV_E_UNSUPPORTED, "NULL is not supported");
    if (tok_is_kw(&P->tok, "TRUE") || tok_is_kw(&P->tok, "FALSE")) {
        out->type = MV_VAL_INT;
        out->i = tok_is_kw(&P->tok, "TRUE") ? 1 : 0;
        advance(P);
        return MV_OK;
    }
    switch (P->tok.type) {
        case T_INT:
            out->type = MV_VAL_INT;
            out->i = P->tok.ival;
            advance(P);
            return MV_OK;
        case T_DOUBLE: {
            double d = P->tok.dval;
            if (!isfinite(d)) return fail(P, MV_E_RANGE, "numeric literal is not finite");
            out->type = MV_VAL_INT;
            out->i = (int64_t)llround(d);
            advance(P);
            return MV_OK;
        }
        case T_STRING:
            out->type = MV_VAL_STR;
            out->s = P->tok.sval;
            out->slen = P->tok.slen;
            P->tok.sval = NULL;
            P->tok.slen = 0;
            advance(P);
            return MV_OK;
        case T_PUNCT:
            if (P->tok.text[0] == '[') {
                advance(P);
                return parse_vector_literal(P, out);
            }
            return fail(P, MV_E_SYNTAX, "unexpected token '%s' in value position", P->tok.text);
        default:
            return fail(P, MV_E_SYNTAX, "unexpected token in value position");
    }
}

/* --------------------------------------------------------------- schema */
static int parse_type(Parser *P, MvColumn *col) {
    if (tok_is_kw(&P->tok, "BIGINT")) {
        col->type = MV_COL_BIGINT;
        advance(P);
        if (accept_kw(P, "UNSIGNED")) { /* accepted; storage stays signed 64-bit */ }
        return MV_OK;
    }
    if (tok_is_kw(&P->tok, "VARCHAR")) {
        col->type = MV_COL_VARCHAR;
        advance(P);
        if (expect_punct(P, "(") != MV_OK) return MV_ERR;
        if (P->tok.type != T_INT) return fail(P, MV_E_SYNTAX, "VARCHAR requires a length");
        if (P->tok.ival < 0 || P->tok.ival > 1048576)
            return fail(P, MV_E_UNSUPPORTED, "unsupported VARCHAR length");
        col->length = (int32_t)P->tok.ival;
        advance(P);
        if (expect_punct(P, ")") != MV_OK) return MV_ERR;
        return MV_OK;
    }
    if (tok_is_kw(&P->tok, "VECTOR")) {
        col->type = MV_COL_VECTOR;
        advance(P);
        if (expect_punct(P, "(") != MV_OK) return MV_ERR;
        if (P->tok.type != T_INT) return fail(P, MV_E_SYNTAX, "VECTOR requires a dimension");
        if (P->tok.ival <= 0 || P->tok.ival > 65536)
            return fail(P, MV_E_UNSUPPORTED, "unsupported VECTOR dimension");
        col->dim = (int32_t)P->tok.ival;
        advance(P);
        if (expect_punct(P, ")") != MV_OK) return MV_ERR;
        return MV_OK;
    }
    if (tok_is_kw(&P->tok, "TEXT") || tok_is_kw(&P->tok, "CHAR") ||
        tok_is_kw(&P->tok, "INT") || tok_is_kw(&P->tok, "INTEGER"))
        return fail(P, MV_E_UNSUPPORTED, "unsupported column type '%s'", P->tok.text);
    return fail(P, MV_E_SYNTAX, "expected a column type");
}

static int parse_create_table(Parser *P, MvStmt *st) {
    if (accept_kw(P, "IF")) {
        if (!accept_kw(P, "NOT")) return fail(P, MV_E_SYNTAX, "expected NOT EXISTS");
        if (!accept_kw(P, "EXISTS")) return fail(P, MV_E_SYNTAX, "expected EXISTS");
    }
    if (expect_ident(P, st->schema.name, sizeof(st->schema.name)) != MV_OK) return MV_ERR;
    if (expect_punct(P, "(") != MV_OK) return MV_ERR;
    st->schema.ncols = 0;
    st->schema.pk_idx = -1;
    st->schema.vec_idx = -1;
    for (;;) {
        if (P->tok.type == T_EOF) return fail(P, MV_E_SYNTAX, "unterminated column definition");
        if (tok_is_kw(&P->tok, "PRIMARY")) {
            advance(P);
            if (!accept_kw(P, "KEY")) return fail(P, MV_E_SYNTAX, "expected KEY");
            if (expect_punct(P, "(") != MV_OK) return MV_ERR;
            char name[64];
            if (expect_ident(P, name, sizeof(name)) != MV_OK) return MV_ERR;
            if (expect_punct(P, ")") != MV_OK) return MV_ERR;
            for (int i = 0; i < st->schema.ncols; i++)
                if (strcmp(st->schema.cols[i].name, name) == 0) st->schema.cols[i].is_pk = 1;
        } else if (tok_is_kw(&P->tok, "FOREIGN") || tok_is_kw(&P->tok, "REFERENCES") ||
                   tok_is_kw(&P->tok, "CONSTRAINT") || tok_is_kw(&P->tok, "INDEX") ||
                   tok_is_kw(&P->tok, "KEY") || tok_is_kw(&P->tok, "CHECK")) {
            return fail(P, MV_E_UNSUPPORTED, "unsupported table constraint");
        } else {
            if (st->schema.ncols >= (int32_t)(sizeof(st->schema.cols) / sizeof(st->schema.cols[0])))
                return fail(P, MV_E_UNSUPPORTED, "too many columns");
            MvColumn *col = &st->schema.cols[st->schema.ncols];
            memset(col, 0, sizeof(*col));
            if (expect_ident(P, col->name, sizeof(col->name)) != MV_OK) return MV_ERR;
            if (parse_type(P, col) != MV_OK) return MV_ERR;
            st->schema.ncols++;
            for (;;) {
                if (accept_kw(P, "PRIMARY")) {
                    if (!accept_kw(P, "KEY")) return fail(P, MV_E_SYNTAX, "expected KEY");
                    col->is_pk = 1;
                } else if (accept_kw(P, "NOT")) {
                    if (!accept_kw(P, "NULL")) return fail(P, MV_E_SYNTAX, "expected NULL");
                } else if (accept_kw(P, "NULL")) {
                    /* accepted; a column never stores NULL in this engine */
                } else if (accept_kw(P, "UNIQUE")) {
                    /* accepted; only the primary key is enforced */
                } else if (accept_kw(P, "AUTO_INCREMENT")) {
                    /* accepted; keys are supplied by the client */
                } else if (accept_kw(P, "DEFAULT") || accept_kw(P, "COMMENT")) {
                    MvValue tmp;
                    if (parse_value(P, &tmp) != MV_OK) return MV_ERR;
                    mv_value_free(&tmp);
                } else if (tok_is_kw(&P->tok, "REFERENCES") ||
                           tok_is_kw(&P->tok, "CHARACTER") || tok_is_kw(&P->tok, "COLLATE")) {
                    return fail(P, MV_E_UNSUPPORTED, "unsupported column attribute");
                } else {
                    break;
                }
            }
        }
        if (accept_punct(P, ",")) continue;
        if (expect_punct(P, ")") != MV_OK) return MV_ERR;
        break;
    }
    int pk_count = 0, vec_count = 0;
    for (int i = 0; i < st->schema.ncols; i++) {
        if (st->schema.cols[i].is_pk) {
            pk_count++;
            st->schema.pk_idx = i;
            if (st->schema.cols[i].type != MV_COL_BIGINT)
                return fail(P, MV_E_UNSUPPORTED, "the primary key must be BIGINT");
        }
        if (st->schema.cols[i].type == MV_COL_VECTOR) {
            vec_count++;
            st->schema.vec_idx = i;
        }
    }
    if (pk_count != 1)
        return fail(P, MV_E_UNSUPPORTED, "exactly one BIGINT primary key is required");
    if (vec_count > 1) return fail(P, MV_E_UNSUPPORTED, "at most one vector column is allowed");
    for (int i = 0; i < st->schema.ncols; i++)
        for (int j = i + 1; j < st->schema.ncols; j++)
            if (strcmp(st->schema.cols[i].name, st->schema.cols[j].name) == 0)
                return fail(P, MV_E_UNSUPPORTED, "duplicate column name");
    st->type = MV_ST_CREATE_TABLE;
    return MV_OK;
}

static int parse_create_index(Parser *P, MvStmt *st) {
    if (!accept_kw(P, "INDEX")) return fail(P, MV_E_UNSUPPORTED, "unsupported CREATE variant");
    if (expect_ident(P, st->index_name, sizeof(st->index_name)) != MV_OK) return MV_ERR;
    if (!accept_kw(P, "ON")) return fail(P, MV_E_SYNTAX, "expected ON");
    if (expect_ident(P, st->table, sizeof(st->table)) != MV_OK) return MV_ERR;
    if (expect_punct(P, "(") != MV_OK) return MV_ERR;
    char col[64];
    if (expect_ident(P, col, sizeof(col)) != MV_OK) return MV_ERR;
    st->schema.ncols = 1;
    snprintf(st->schema.cols[0].name, sizeof(st->schema.cols[0].name), "%s", col);
    st->schema.cols[0].type = MV_COL_VECTOR;
    if (expect_punct(P, ")") != MV_OK) return MV_ERR;
    st->type = MV_ST_CREATE_INDEX;
    return MV_OK;
}

static int parse_insert(Parser *P, MvStmt *st) {
    if (!accept_kw(P, "INTO")) return fail(P, MV_E_SYNTAX, "expected INTO");
    if (expect_ident(P, st->table, sizeof(st->table)) != MV_OK) return MV_ERR;
    if (accept_punct(P, "(")) {
        int cap = 8;
        st->icols = mv_xmalloc((size_t)cap * 64);
        st->nicols = 0;
        for (;;) {
            char name[64];
            if (expect_ident(P, name, sizeof(name)) != MV_OK) return MV_ERR;
            if (st->nicols == cap) {
                cap *= 2;
                st->icols = mv_xrealloc(st->icols, (size_t)cap * 64);
            }
            memcpy(st->icols + (size_t)st->nicols * 64, name, 64);
            st->nicols++;
            if (accept_punct(P, ",")) continue;
            if (expect_punct(P, ")") != MV_OK) return MV_ERR;
            break;
        }
    }
    if (!accept_kw(P, "VALUES")) return fail(P, MV_E_SYNTAX, "expected VALUES");
    int vcap = 16;
    st->vals = mv_xmalloc((size_t)vcap * sizeof(MvValue));
    st->nvals = 0;
    st->nrows = 0;
    st->rowwidth = -1;
    for (;;) {
        if (expect_punct(P, "(") != MV_OK) return MV_ERR;
        int width = 0;
        for (;;) {
            MvValue v;
            if (parse_value(P, &v) != MV_OK) return MV_ERR;
            if (st->nvals == vcap) {
                vcap *= 2;
                st->vals = mv_xrealloc(st->vals, (size_t)vcap * sizeof(MvValue));
            }
            st->vals[st->nvals++] = v;
            width++;
            if (accept_punct(P, ",")) continue;
            if (expect_punct(P, ")") != MV_OK) return MV_ERR;
            break;
        }
        if (st->rowwidth < 0) st->rowwidth = width;
        else if (st->rowwidth != width)
            return fail(P, MV_E_COLCOUNT, "all rows must provide the same number of values");
        st->nrows++;
        if (accept_punct(P, ",")) continue;
        break;
    }
    st->type = MV_ST_INSERT;
    return MV_OK;
}

static int parse_cmp_op(Parser *P, MvCmpOp *op) {
    if (P->tok.type != T_PUNCT) return fail(P, MV_E_SYNTAX, "expected a comparison operator");
    if (strcmp(P->tok.text, "=") == 0) *op = MV_OP_EQ;
    else if (strcmp(P->tok.text, "<>") == 0 || strcmp(P->tok.text, "!=") == 0) *op = MV_OP_NE;
    else if (strcmp(P->tok.text, "<") == 0) *op = MV_OP_LT;
    else if (strcmp(P->tok.text, "<=") == 0) *op = MV_OP_LE;
    else if (strcmp(P->tok.text, ">") == 0) *op = MV_OP_GT;
    else if (strcmp(P->tok.text, ">=") == 0) *op = MV_OP_GE;
    else return fail(P, MV_E_UNSUPPORTED, "unsupported operator '%s'", P->tok.text);
    advance(P);
    return MV_OK;
}

static int parse_condition(Parser *P, MvCond *c) {
    memset(c, 0, sizeof(*c));
    c->col = -1;
    c->col2 = -1;
    if (P->tok.type != T_IDENT)
        return fail(P, MV_E_SYNTAX, "expected a column name in the condition");
    snprintf(c->col_name, sizeof(c->col_name), "%s", P->tok.text);
    advance(P);
    if (parse_cmp_op(P, &c->op) != MV_OK) return MV_ERR;
    if (P->tok.type == T_IDENT) {
        snprintf(c->col2_name, sizeof(c->col2_name), "%s", P->tok.text);
        advance(P);
        return MV_OK;
    }
    if (parse_value(P, &c->val) != MV_OK) return MV_ERR;
    return MV_OK;
}

static int parse_where(Parser *P, MvCond **out, int32_t *count) {
    MvCond *conds = NULL;
    int32_t n = 0, cap = 0;
    for (;;) {
        if (n == cap) {
            cap = cap ? cap * 2 : 4;
            conds = mv_xrealloc(conds, (size_t)cap * sizeof(MvCond));
        }
        if (parse_condition(P, &conds[n]) != MV_OK) {
            free(conds);
            return MV_ERR;
        }
        n++;
        if (accept_kw(P, "AND")) continue;
        if (tok_is_kw(&P->tok, "OR")) {
            free(conds);
            return fail(P, MV_E_UNSUPPORTED, "OR is not supported");
        }
        break;
    }
    *out = conds;
    *count = n;
    return MV_OK;
}

/* L2_DISTANCE(vec_column, vector_literal) [AS alias] */
static int parse_distance(Parser *P, MvProj *proj) {
    advance(P); /* function name */
    if (expect_punct(P, "(") != MV_OK) return MV_ERR;
    char col[64];
    if (expect_ident(P, col, sizeof(col)) != MV_OK) return MV_ERR;
    snprintf(proj->col_name, sizeof(proj->col_name), "%s", col);
    if (expect_punct(P, ",") != MV_OK) return MV_ERR;
    MvValue v;
    if (parse_value(P, &v) != MV_OK) return MV_ERR;
    if (v.type == MV_VAL_STR) {
        /* the literal is a vector written as text, e.g. '[0, 2]' */
        float *vec = NULL;
        int32_t dim = 0;
        MvError tmp;
        mv_err_clear(&tmp);
        if (mv_parse_vector(v.s, v.slen, 0, &vec, &dim, &tmp) != MV_OK) {
            mv_value_free(&v);
            return fail(P, MV_E_BAD_VECTOR, "%s", tmp.message);
        }
        mv_value_free(&v);
        v.type = MV_VAL_VEC;
        v.vec = vec;
        v.dim = dim;
    }
    if (v.type != MV_VAL_VEC) {
        mv_value_free(&v);
        return fail(P, MV_E_BAD_VECTOR, "the second argument of L2_DISTANCE must be a vector");
    }
    proj->is_dist = 1;
    proj->vec = v;
    if (expect_punct(P, ")") != MV_OK) return MV_ERR;
    return MV_OK;
}

static int parse_projection(Parser *P, MvProj *proj) {
    memset(proj, 0, sizeof(*proj));
    proj->col = -1;
    if (tok_is_kw(&P->tok, "L2_DISTANCE")) {
        if (parse_distance(P, proj) != MV_OK) return MV_ERR;
    } else if (P->tok.type == T_IDENT) {
        snprintf(proj->col_name, sizeof(proj->col_name), "%s", P->tok.text);
        advance(P);
        if (accept_punct(P, ".")) {
            char second[64];
            if (expect_ident(P, second, sizeof(second)) != MV_OK) return MV_ERR;
            snprintf(proj->col_name, sizeof(proj->col_name), "%s", second);
        }
    } else {
        return fail(P, MV_E_SYNTAX, "expected a column name");
    }
    if (accept_kw(P, "AS")) {
        if (expect_ident(P, proj->alias, sizeof(proj->alias)) != MV_OK) return MV_ERR;
        proj->has_alias = 1;
    }
    return MV_OK;
}

static int parse_order_item(Parser *P, MvOrder *ord) {
    memset(ord, 0, sizeof(*ord));
    ord->col = -1;
    if (tok_is_kw(&P->tok, "L2_DISTANCE")) {
        MvProj tmp;
        memset(&tmp, 0, sizeof(tmp));
        tmp.col = -1;
        if (parse_distance(P, &tmp) != MV_OK) return MV_ERR;
        ord->is_dist = 1;
        ord->vec = tmp.vec;
        snprintf(ord->col_name, sizeof(ord->col_name), "%s", tmp.col_name);
    } else if (P->tok.type == T_IDENT) {
        snprintf(ord->col_name, sizeof(ord->col_name), "%s", P->tok.text);
        advance(P);
    } else {
        return fail(P, MV_E_SYNTAX, "expected an ORDER BY expression");
    }
    if (accept_kw(P, "ASC")) ord->desc = 0;
    else if (accept_kw(P, "DESC")) ord->desc = 1;
    return MV_OK;
}

static int parse_select(Parser *P, MvStmt *st) {
    if (accept_kw(P, "DISTINCT")) return fail(P, MV_E_UNSUPPORTED, "DISTINCT is not supported");
    if (tok_is_punct(&P->tok, "*")) {
        advance(P);
        st->star = 1;
        st->nprojs = 0;
    } else {
        int cap = 8;
        st->projs = mv_xmalloc((size_t)cap * sizeof(MvProj));
        st->nprojs = 0;
        for (;;) {
            if (st->nprojs == cap) {
                cap *= 2;
                st->projs = mv_xrealloc(st->projs, (size_t)cap * sizeof(MvProj));
            }
            if (parse_projection(P, &st->projs[st->nprojs]) != MV_OK) return MV_ERR;
            st->nprojs++;
            if (accept_punct(P, ",")) continue;
            break;
        }
    }
    if (!accept_kw(P, "FROM")) return fail(P, MV_E_SYNTAX, "expected FROM");
    if (P->tok.type == T_EOF) return fail(P, MV_E_SYNTAX, "expected a table name");
    if (!accept_punct(P, "(")) {
        if (expect_ident(P, st->table, sizeof(st->table)) != MV_OK) return MV_ERR;
    } else {
        return fail(P, MV_E_UNSUPPORTED, "subqueries are not supported");
    }
    if (tok_is_kw(&P->tok, "JOIN") || tok_is_kw(&P->tok, "INNER") || tok_is_kw(&P->tok, "LEFT") ||
        tok_is_kw(&P->tok, "RIGHT") || tok_is_kw(&P->tok, "OUTER") || tok_is_kw(&P->tok, "CROSS") ||
        tok_is_kw(&P->tok, "NATURAL") || tok_is_kw(&P->tok, "STRAIGHT_JOIN"))
        return fail(P, MV_E_UNSUPPORTED, "joins are not supported");
    if (accept_kw(P, "WHERE")) {
        if (parse_where(P, &st->conds, &st->nconds) != MV_OK) return MV_ERR;
    }
    if (accept_kw(P, "GROUP") || accept_kw(P, "HAVING"))
        return fail(P, MV_E_UNSUPPORTED, "grouping is not supported");
    if (accept_kw(P, "ORDER")) {
        if (!accept_kw(P, "BY")) return fail(P, MV_E_SYNTAX, "expected BY");
        int cap = 4;
        st->orders = mv_xmalloc((size_t)cap * sizeof(MvOrder));
        st->norders = 0;
        for (;;) {
            if (st->norders == cap) {
                cap *= 2;
                st->orders = mv_xrealloc(st->orders, (size_t)cap * sizeof(MvOrder));
            }
            if (parse_order_item(P, &st->orders[st->norders]) != MV_OK) return MV_ERR;
            st->norders++;
            if (accept_punct(P, ",")) continue;
            break;
        }
    }
    if (accept_kw(P, "LIMIT")) {
        if (P->tok.type != T_INT) return fail(P, MV_E_SYNTAX, "LIMIT requires an integer");
        if (P->tok.ival < 0) return fail(P, MV_E_SYNTAX, "LIMIT must not be negative");
        st->limit = P->tok.ival;
        st->has_limit = 1;
        advance(P);
        if (accept_kw(P, "OFFSET")) {
            if (P->tok.type != T_INT) return fail(P, MV_E_SYNTAX, "OFFSET requires an integer");
            if (P->tok.ival < 0) return fail(P, MV_E_SYNTAX, "OFFSET must not be negative");
            st->limit += P->tok.ival;
            advance(P);
        }
    }
    st->type = MV_ST_SELECT;
    return MV_OK;
}

static int parse_update(Parser *P, MvStmt *st) {
    if (expect_ident(P, st->table, sizeof(st->table)) != MV_OK) return MV_ERR;
    if (!accept_kw(P, "SET")) return fail(P, MV_E_SYNTAX, "expected SET");
    int cap = 8;
    st->assigns = mv_xmalloc((size_t)cap * sizeof(MvAssign));
    st->nassigns = 0;
    for (;;) {
        if (st->nassigns == cap) {
            cap *= 2;
            st->assigns = mv_xrealloc(st->assigns, (size_t)cap * sizeof(MvAssign));
        }
        MvAssign *a = &st->assigns[st->nassigns];
        memset(a, 0, sizeof(*a));
        a->col = -1;
        if (expect_ident(P, a->col_name, sizeof(a->col_name)) != MV_OK) return MV_ERR;
        if (!accept_punct(P, "=")) {
            if (!tok_is_punct(&P->tok, "=>")) return fail(P, MV_E_SYNTAX, "expected '='");
            advance(P);
        }
        if (parse_value(P, &a->val) != MV_OK) return MV_ERR;
        st->nassigns++;
        if (accept_punct(P, ",")) continue;
        break;
    }
    if (accept_kw(P, "WHERE")) {
        if (parse_where(P, &st->conds, &st->nconds) != MV_OK) return MV_ERR;
    }
    st->type = MV_ST_UPDATE;
    return MV_OK;
}

static int parse_delete(Parser *P, MvStmt *st) {
    if (!accept_kw(P, "FROM")) return fail(P, MV_E_SYNTAX, "expected FROM");
    if (expect_ident(P, st->table, sizeof(st->table)) != MV_OK) return MV_ERR;
    if (accept_kw(P, "WHERE")) {
        if (parse_where(P, &st->conds, &st->nconds) != MV_OK) return MV_ERR;
    }
    st->type = MV_ST_DELETE;
    return MV_OK;
}

int mv_sql_parse(const char *sql, MvStmt *out, MvError *err) {
    memset(out, 0, sizeof(*out));
    mv_err_clear(err);
    Parser P;
    P.p = sql;
    P.end = sql + strlen(sql);
    P.has_ahead = 0;
    P.err = err;
    advance(&P);
    if (P.tok.type == T_EOF) {
        mv_err_set(err, MV_E_SYNTAX, "empty statement");
        return MV_ERR;
    }
    int rc = MV_ERR;
    if (tok_is_kw(&P.tok, "CREATE")) {
        advance(&P);
        if (accept_kw(&P, "TABLE")) rc = parse_create_table(&P, out);
        else if (accept_kw(&P, "VECTOR")) rc = parse_create_index(&P, out);
        else rc = fail(&P, MV_E_UNSUPPORTED, "unsupported CREATE variant");
    } else if (tok_is_kw(&P.tok, "INSERT")) {
        advance(&P);
        rc = parse_insert(&P, out);
    } else if (tok_is_kw(&P.tok, "SELECT")) {
        advance(&P);
        rc = parse_select(&P, out);
    } else if (tok_is_kw(&P.tok, "UPDATE")) {
        advance(&P);
        rc = parse_update(&P, out);
    } else if (tok_is_kw(&P.tok, "DELETE")) {
        advance(&P);
        rc = parse_delete(&P, out);
    } else if (tok_is_kw(&P.tok, "BEGIN") || tok_is_kw(&P.tok, "START")) {
        if (tok_is_kw(&P.tok, "START")) {
            advance(&P);
            if (!accept_kw(&P, "TRANSACTION")) {
                mv_err_set(err, MV_E_SYNTAX, "expected TRANSACTION");
                return MV_ERR;
            }
        } else {
            advance(&P);
        }
        out->type = MV_ST_BEGIN;
        rc = MV_OK;
    } else if (tok_is_kw(&P.tok, "COMMIT")) {
        advance(&P);
        out->type = MV_ST_COMMIT;
        rc = MV_OK;
    } else if (tok_is_kw(&P.tok, "ROLLBACK")) {
        advance(&P);
        out->type = MV_ST_ROLLBACK;
        rc = MV_OK;
    } else {
        rc = fail(&P, MV_E_UNSUPPORTED, "unsupported statement");
    }
    if (rc != MV_OK) return MV_ERR;
    /* only a trailing semicolon may follow */
    if (accept_punct(&P, ";")) { /* optional */ }
    if (P.tok.type != T_EOF) {
        mv_err_set(err, MV_E_SYNTAX, "unexpected trailing token '%s'", P.tok.text);
        return MV_ERR;
    }
    return MV_OK;
}

void mv_stmt_free(MvStmt *st) {
    if (!st) return;
    free(st->icols);
    for (int32_t i = 0; i < st->nvals; i++) mv_value_free(&st->vals[i]);
    free(st->vals);
    for (int32_t i = 0; i < st->nconds; i++) mv_value_free(&st->conds[i].val);
    free(st->conds);
    for (int32_t i = 0; i < st->nprojs; i++) mv_value_free(&st->projs[i].vec);
    free(st->projs);
    for (int32_t i = 0; i < st->norders; i++) mv_value_free(&st->orders[i].vec);
    free(st->orders);
    for (int32_t i = 0; i < st->nassigns; i++) mv_value_free(&st->assigns[i].val);
    free(st->assigns);
    memset(st, 0, sizeof(*st));
}

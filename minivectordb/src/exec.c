/* Statement execution.
 *
 * Semantics implemented here:
 *  - autocommit: every modifying statement is its own transaction and is
 *    fsynced before the response is written;
 *  - one explicit transaction at a time; statements inside it see their own
 *    changes, a failed statement leaves nothing behind, COMMIT makes the
 *    changes durable, ROLLBACK restores the pre-transaction state;
 *  - TopK is computed inside the filtered record set, ties are broken by
 *    ascending primary key, and distances are computed from the stored
 *    float32 vectors.
 */
#include "db.h"
#include "vecidx.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------- results */
void mv_result_init(MvResult *r) { memset(r, 0, sizeof(*r)); }

void mv_result_free(MvResult *r) {
    if (!r) return;
    for (int i = 0; i < r->nrows; i++) {
        if (r->rows[i]) {
            for (int c = 0; c < r->ncols; c++) mv_value_free(&r->rows[i][c]);
            free(r->rows[i]);
        }
    }
    free(r->rows);
    free(r->colnames);
    memset(r, 0, sizeof(*r));
}

int mv_result_add_row(MvResult *r, int ncols) {
    if (r->nrows == r->rowcap) {
        r->rowcap = r->rowcap ? r->rowcap * 2 : 16;
        r->rows = mv_xrealloc(r->rows, (size_t)r->rowcap * sizeof(MvValue *));
    }
    MvValue *row = mv_xmalloc((size_t)ncols * sizeof(MvValue));
    memset(row, 0, (size_t)ncols * sizeof(MvValue));
    r->rows[r->nrows++] = row;
    return r->nrows - 1;
}

/* --------------------------------------------------------------- helpers */
static int resolve_col(const MvSchema *s, const char *name) {
    for (int32_t i = 0; i < s->ncols; i++)
        if (strcmp(s->cols[i].name, name) == 0) return (int)i;
    return -1;
}

static int scalar_index(const MvSchema *s, int col) {
    int vi = 0;
    for (int32_t i = 0; i < s->ncols; i++) {
        if (i == s->pk_idx || i == s->vec_idx) continue;
        if ((int)i == col) return vi;
        vi++;
    }
    return -1;
}

static int str_cmp(const char *a, size_t alen, const char *b, size_t blen) {
    size_t n = alen < blen ? alen : blen;
    int c = n ? memcmp(a, b, n) : 0;
    if (c != 0) return c < 0 ? -1 : 1;
    if (alen == blen) return 0;
    return alen < blen ? -1 : 1;
}

static int64_t str_to_i64(const char *s, size_t len, int *ok) {
    char tmp[64];
    if (len == 0 || len >= sizeof(tmp)) {
        *ok = 0;
        return 0;
    }
    memcpy(tmp, s, len);
    tmp[len] = '\0';
    char *endp = NULL;
    long long v = strtoll(tmp, &endp, 10);
    if (!endp || *endp != '\0') {
        *ok = 0;
        return 0;
    }
    *ok = 1;
    return (int64_t)v;
}

static const MvValue *col_value(const MvSchema *s, const MvValue *vals, int64_t pk, int col,
                                 MvValue *tmp) {
    if (col == s->pk_idx) {
        tmp->type = MV_VAL_INT;
        tmp->i = pk;
        return tmp;
    }
    int vi = scalar_index(s, col);
    if (vi < 0) return NULL;
    return &vals[vi];
}

static int cmp_values(const MvValue *a, const MvValue *b) {
    if (a->type == MV_VAL_INT && b->type == MV_VAL_INT) {
        if (a->i < b->i) return -1;
        if (a->i > b->i) return 1;
        return 0;
    }
    if (a->type == MV_VAL_STR && b->type == MV_VAL_STR)
        return str_cmp(a->s, a->slen, b->s, b->slen);
    if (a->type == MV_VAL_STR && b->type == MV_VAL_INT) {
        int ok = 0;
        int64_t v = str_to_i64(a->s, a->slen, &ok);
        if (ok) return v < b->i ? -1 : (v > b->i ? 1 : 0);
        return -1;
    }
    if (a->type == MV_VAL_INT && b->type == MV_VAL_STR) {
        int ok = 0;
        int64_t v = str_to_i64(b->s, b->slen, &ok);
        if (ok) return a->i < v ? -1 : (a->i > v ? 1 : 0);
        return 1;
    }
    return 0;
}

static int cond_match(const MvSchema *s, const MvCond *c, const MvValue *vals, int64_t pk) {
    MvValue tmp_l, tmp_r;
    const MvValue *l = col_value(s, vals, pk, c->col, &tmp_l);
    if (!l) return 0;
    const MvValue *r;
    if (c->col2 >= 0) {
        r = col_value(s, vals, pk, c->col2, &tmp_r);
        if (!r) return 0;
    } else {
        r = &c->val;
    }
    int cmp = cmp_values(l, r);
    switch (c->op) {
        case MV_OP_EQ: return cmp == 0;
        case MV_OP_NE: return cmp != 0;
        case MV_OP_LT: return cmp < 0;
        case MV_OP_LE: return cmp <= 0;
        case MV_OP_GT: return cmp > 0;
        case MV_OP_GE: return cmp >= 0;
    }
    return 0;
}

static int coerce_value(const MvSchema *s, int col, const MvValue *in, MvValue *out, MvError *err) {
    const MvColumn *c = &s->cols[col];
    memset(out, 0, sizeof(*out));
    if (c->type == MV_COL_BIGINT) {
        if (in->type == MV_VAL_INT) {
            out->type = MV_VAL_INT;
            out->i = in->i;
            return MV_OK;
        }
        if (in->type == MV_VAL_STR) {
            int ok = 0;
            int64_t v = str_to_i64(in->s, in->slen, &ok);
            if (!ok) {
                mv_err_set(err, MV_E_SYNTAX, "value for column '%s' is not an integer", c->name);
                return MV_ERR;
            }
            out->type = MV_VAL_INT;
            out->i = v;
            return MV_OK;
        }
        mv_err_set(err, MV_E_SYNTAX, "value for column '%s' must be an integer", c->name);
        return MV_ERR;
    }
    if (c->type == MV_COL_VARCHAR) {
        if (in->type == MV_VAL_STR) {
            if (mv_utf8_char_count(in->s, in->slen) > c->length) {
                mv_err_set(err, MV_E_TOO_LONG, "value for column '%s' exceeds VARCHAR(%d)", c->name,
                           c->length);
                return MV_ERR;
            }
            out->type = MV_VAL_STR;
            out->s = mv_strndup(in->s, in->slen);
            out->slen = in->slen;
            return MV_OK;
        }
        if (in->type == MV_VAL_INT) {
            char tmp[32];
            int n = snprintf(tmp, sizeof(tmp), "%lld", (long long)in->i);
            out->type = MV_VAL_STR;
            out->s = mv_strndup(tmp, (size_t)n);
            out->slen = (size_t)n;
            return MV_OK;
        }
        mv_err_set(err, MV_E_SYNTAX, "value for column '%s' must be a string", c->name);
        return MV_ERR;
    }
    if (in->type == MV_VAL_VEC) {
        if (in->dim != c->dim) {
            mv_err_set(err, MV_E_DIM, "column '%s' expects %d dimensions but got %d", c->name, c->dim,
                       in->dim);
            return MV_ERR;
        }
        out->type = MV_VAL_VEC;
        out->dim = in->dim;
        out->vec = mv_xmalloc((size_t)in->dim * sizeof(float));
        memcpy(out->vec, in->vec, (size_t)in->dim * sizeof(float));
        return MV_OK;
    }
    if (in->type == MV_VAL_STR) {
        float *vec = NULL;
        int32_t dim = 0;
        if (mv_parse_vector(in->s, in->slen, c->dim, &vec, &dim, err) != MV_OK) return MV_ERR;
        out->type = MV_VAL_VEC;
        out->dim = dim;
        out->vec = vec;
        return MV_OK;
    }
    mv_err_set(err, MV_E_BAD_VECTOR, "value for column '%s' must be a vector", c->name);
    return MV_ERR;
}

/* ---------------------------------------------------------------- insert */
static int exec_insert(MvDb *db, const MvStmt *st, MvResult *res, MvError *err) {
    MvTable *t = mv_db_find_table(db, st->table);
    if (!t) {
        mv_err_set(err, MV_E_TABLE, "table '%s' does not exist", st->table);
        return MV_ERR;
    }
    const MvSchema *s = mv_table_schema(t);
    int ncols = s->ncols;
    int nvals = mv_table_nvals(t);
    int vecdim = s->vec_idx >= 0 ? s->cols[s->vec_idx].dim : 0;

    /* map every column to the value slot that feeds it */
    int *map = mv_xmalloc((size_t)ncols * sizeof(int));
    for (int j = 0; j < ncols; j++) map[j] = -1;
    if (st->icols) {
        if (st->nicols > ncols) {
            free(map);
            mv_err_set(err, MV_E_COLCOUNT, "too many columns in the INSERT column list");
            return MV_ERR;
        }
        for (int k = 0; k < st->nicols; k++) {
            const char *name = st->icols + (size_t)k * 64;
            int col = resolve_col(s, name);
            if (col < 0) {
                free(map);
                mv_err_set(err, MV_E_COLUMN, "unknown column '%s'", name);
                return MV_ERR;
            }
            if (map[col] >= 0) {
                free(map);
                mv_err_set(err, MV_E_COLCOUNT, "column '%s' specified twice", name);
                return MV_ERR;
            }
            map[col] = k;
        }
        if (map[s->pk_idx] < 0) {
            free(map);
            mv_err_set(err, MV_E_COLCOUNT, "the primary key column must be provided");
            return MV_ERR;
        }
    } else {
        if (st->rowwidth != ncols) {
            free(map);
            mv_err_set(err, MV_E_COLCOUNT, "expected %d values but got %d", ncols, st->rowwidth);
            return MV_ERR;
        }
        for (int j = 0; j < ncols; j++) map[j] = j;
    }

    /* Pass one validates the whole statement: a failing statement must not
     * leave any part of itself behind, so nothing is written before every row
     * has been checked (values, lengths, dimensions and duplicate keys). */
    int rc = MV_OK;
    int64_t affected = 0;
    MvValue *rows = mv_xmalloc((size_t)(st->nrows > 0 ? st->nrows : 1) * (size_t)(nvals > 0 ? nvals : 1) *
                                sizeof(MvValue));
    memset(rows, 0, (size_t)(st->nrows > 0 ? st->nrows : 1) * (size_t)(nvals > 0 ? nvals : 1) *
                       sizeof(MvValue));
    float *vecs = mv_xmalloc((size_t)(st->nrows > 0 ? st->nrows : 1) * (size_t)(vecdim > 0 ? vecdim : 1) *
                             sizeof(float));
    int64_t *pks = mv_xmalloc((size_t)(st->nrows > 0 ? st->nrows : 1) * sizeof(int64_t));

    for (int r = 0; r < st->nrows && rc == MV_OK; r++) {
        MvValue *row_vals = rows + (size_t)r * (nvals > 0 ? nvals : 1);
        for (int j = 0; j < ncols; j++) {
            MvValue tmp;
            memset(&tmp, 0, sizeof(tmp));
            if (map[j] >= 0) {
                const MvValue *in = &st->vals[(size_t)r * st->rowwidth + map[j]];
                if (coerce_value(s, j, in, &tmp, err) != MV_OK) {
                    rc = MV_ERR;
                    break;
                }
            } else if (s->cols[j].type == MV_COL_BIGINT) {
                tmp.type = MV_VAL_INT;
                tmp.i = 0;
            } else if (s->cols[j].type == MV_COL_VARCHAR) {
                tmp.type = MV_VAL_STR;
                tmp.s = mv_strdup("");
                tmp.slen = 0;
            } else {
                tmp.type = MV_VAL_VEC;
                tmp.dim = s->cols[j].dim;
                tmp.vec = mv_xmalloc((size_t)tmp.dim * sizeof(float));
                memset(tmp.vec, 0, (size_t)tmp.dim * sizeof(float));
            }
            if (s->cols[j].type == MV_COL_VECTOR) {
                memcpy(vecs + (size_t)r * (vecdim > 0 ? vecdim : 1), tmp.vec, (size_t)tmp.dim * sizeof(float));
                mv_value_free(&tmp);
            } else if (j == s->pk_idx) {
                pks[r] = tmp.i; /* the primary key is not part of the scalar array */
                mv_value_free(&tmp);
            } else {
                int vi = scalar_index(s, j);
                mv_value_free(&row_vals[vi]);
                row_vals[vi] = tmp;
            }
        }
        if (rc != MV_OK) break;
        if (mv_table_slot_of_pk(t, pks[r]) >= 0) {
            mv_err_set(err, MV_E_DUP_KEY, "duplicate primary key %lld", (long long)pks[r]);
            rc = MV_ERR;
            break;
        }
        for (int q = 0; q < r; q++) {
            if (pks[q] == pks[r]) {
                mv_err_set(err, MV_E_DUP_KEY, "duplicate primary key %lld", (long long)pks[r]);
                rc = MV_ERR;
                break;
            }
        }
    }

    /* Pass two writes the rows. */
    if (rc == MV_OK) {
        for (int r = 0; r < st->nrows; r++) {
            MvValue *row_vals = rows + (size_t)r * (nvals > 0 ? nvals : 1);
            const float *vec = s->vec_idx >= 0 ? vecs + (size_t)r * vecdim : NULL;
            if (mv_store_insert(db, t, pks[r], row_vals, nvals, vec, err) != MV_OK) {
                rc = MV_ERR;
                break;
            }
            affected++;
        }
    }

    int cells = (st->nrows > 0 ? st->nrows : 1) * (nvals > 0 ? nvals : 1);
    for (int i = 0; i < cells; i++) mv_value_free(&rows[i]);
    free(rows);
    free(vecs);
    free(pks);
    free(map);
    if (rc != MV_OK) return MV_ERR;
    res->affected_rows = affected;
    return MV_OK;
}

/* ---------------------------------------------------------------- select */
typedef struct {
    int64_t pk;
    int32_t slot;
    double dist;
    uint8_t has_dist;
    MvValue key;
    uint8_t has_key;
} Cand;

typedef struct {
    MvDb *db;
    MvTable *t;
    const MvSchema *s;
    MvCond *conds;
    int nconds;
    int order_kind; /* 0 none, 1 column, 2 distance */
    int order_col;
    uint8_t order_desc;
    Cand *cands;
    int ncands;
    int cands_cap;
    int64_t limit;
    uint8_t has_limit;
    float *query;
    void *filter; /* FilterCtx while a filtered vector query is running */
} SelCtx;

static int cand_better(const SelCtx *c, const Cand *a, const Cand *b) {
    int cmp = 0;
    if (c->order_kind == 2) {
        if (a->dist < b->dist) cmp = -1;
        else if (a->dist > b->dist) cmp = 1;
    } else if (c->order_kind == 1) {
        cmp = cmp_values(&a->key, &b->key);
    }
    if (cmp == 0) {
        if (a->pk < b->pk) cmp = -1;
        else if (a->pk > b->pk) cmp = 1;
    } else if (c->order_desc) {
        cmp = -cmp;
    }
    return cmp < 0;
}

/* Keep at most `limit` candidates, always evicting the worst one. */
static void sel_push(SelCtx *c, const Cand *cand) {
    if (c->has_limit && (int64_t)c->ncands >= c->limit) {
        int worst = 0;
        for (int i = 1; i < c->ncands; i++)
            if (cand_better(c, &c->cands[worst], &c->cands[i])) worst = i;
        if (!cand_better(c, cand, &c->cands[worst])) return;
        mv_value_free(&c->cands[worst].key);
        c->cands[worst] = *cand;
        return;
    }
    if (c->ncands == c->cands_cap) {
        c->cands_cap = c->cands_cap ? c->cands_cap * 2 : 64;
        c->cands = mv_xrealloc(c->cands, (size_t)c->cands_cap * sizeof(Cand));
    }
    c->cands[c->ncands++] = *cand;
}

static int sel_row_cb(void *ctx, const MvRow *row, int64_t pk) {
    SelCtx *c = (SelCtx *)ctx;
    for (int i = 0; i < c->nconds; i++)
        if (!cond_match(c->s, &c->conds[i], row->vals, pk)) return MV_OK;
    Cand cand;
    memset(&cand, 0, sizeof(cand));
    cand.pk = pk;
    cand.slot = row->slot;
    if (c->query) {
        int32_t dim = c->s->cols[c->s->vec_idx].dim;
        float *vec = mv_xmalloc((size_t)dim * sizeof(float));
        if (mv_get_vector(c->db, c->t, row->slot, vec) != MV_OK) {
            free(vec);
            return MV_ERR;
        }
        cand.dist = mv_l2_distance(c->query, vec, dim);
        cand.has_dist = 1;
        free(vec);
    }
    if (c->order_kind == 1) {
        MvValue tmp;
        const MvValue *v = col_value(c->s, row->vals, pk, c->order_col, &tmp);
        if (v) {
            cand.key.type = v->type;
            if (v->type == MV_VAL_STR) {
                cand.key.s = mv_strndup(v->s, v->slen);
                cand.key.slen = v->slen;
            } else if (v->type == MV_VAL_INT) {
                cand.key.i = v->i;
            }
            cand.has_key = 1;
        }
    }
    sel_push(c, &cand);
    return MV_OK;
}

/* Pass one of a filtered vector query: mark the slots that satisfy WHERE. */
typedef struct {
    const MvSchema *s;
    MvCond *conds;
    int nconds;
    uint8_t *match;
} FilterCtx;

static int sel_filter_cb(void *ctx, const MvRow *row, int64_t pk) {
    FilterCtx *c = (FilterCtx *)ctx;
    for (int i = 0; i < c->nconds; i++)
        if (!cond_match(c->s, &c->conds[i], row->vals, pk)) return MV_OK;
    int64_t slot = row->slot;
    c->match[slot >> 3] |= (uint8_t)(1u << (slot & 7));
    return MV_OK;
}

/* Pass two: the vector scan, restricted to the marked slots. */
static int sel_slot_filtered_cb(void *ctx, int32_t slot, int64_t pk, const float *vec) {
    SelCtx *c = (SelCtx *)ctx;
    FilterCtx *f = (FilterCtx *)c->filter;
    if (!((f->match[slot >> 3] >> (slot & 7)) & 1u)) return MV_OK;
    Cand cand;
    memset(&cand, 0, sizeof(cand));
    cand.pk = pk;
    cand.slot = slot;
    cand.dist = mv_l2_distance(c->query, vec, c->s->cols[c->s->vec_idx].dim);
    cand.has_dist = 1;
    sel_push(c, &cand);
    return MV_OK;
}

static int sel_slot_cb(void *ctx, int32_t slot, int64_t pk, const float *vec) {
    SelCtx *c = (SelCtx *)ctx;
    Cand cand;
    memset(&cand, 0, sizeof(cand));
    cand.pk = pk;
    cand.slot = slot;
    cand.dist = mv_l2_distance(c->query, vec, c->s->cols[c->s->vec_idx].dim);
    cand.has_dist = 1;
    sel_push(c, &cand);
    return MV_OK;
}

static void sel_sort(SelCtx *c) {
    for (int i = 1; i < c->ncands; i++) {
        Cand tmp = c->cands[i];
        int j = i - 1;
        while (j >= 0 && cand_better(c, &tmp, &c->cands[j])) {
            c->cands[j + 1] = c->cands[j];
            j--;
        }
        c->cands[j + 1] = tmp;
    }
}

/* Tables below this size are scanned exactly: the approximate index cannot
 * pay for its own construction and an exact answer is always available. */
#define MV_ANN_MIN_ROWS 2048

/* Collect candidates through the approximate index. Returns MV_ERR when the
 * index cannot serve the query; *enough tells the caller whether the result
 * set is complete enough to be used (otherwise the exact scan takes over). */
static int sel_ann_collect(MvDb *db, MvTable *t, SelCtx *ctx, int *enough) {
    MvVecIndex *idx = mv_store_ensure_index(db, t);
    *enough = 0;
    if (!idx) return MV_ERR;
    int32_t ef = 64;
    if (ctx->has_limit && (int32_t)ctx->limit * 4 > ef) ef = (int32_t)ctx->limit * 4;
    if (ef > 1024) ef = 1024;
    int32_t *slots = mv_xmalloc((size_t)ef * sizeof(int32_t));
    double *dists = mv_xmalloc((size_t)ef * sizeof(double));
    int32_t n = mv_vidx_search(idx, ctx->query, ef, slots, dists);
    int nvals = mv_table_nvals(t);
    MvValue *vals = nvals > 0 ? mv_xmalloc((size_t)nvals * sizeof(MvValue)) : NULL;
    if (vals) memset(vals, 0, (size_t)nvals * sizeof(MvValue));
    for (int32_t i = 0; i < n; i++) {
        int32_t slot = slots[i];
        if (!mv_table_slot_alive(t, slot)) continue;
        int64_t pk = mv_table_pk_of_slot(t, slot);
        if (pk < 0) continue;
        if (ctx->nconds > 0) {
            if (!vals) continue;
            for (int v = 0; v < nvals; v++) mv_value_free(&vals[v]);
            if (mv_row_values(db, t, slot, vals) != MV_OK) continue;
            int ok = 1;
            for (int c = 0; c < ctx->nconds; c++) {
                if (!cond_match(ctx->s, &ctx->conds[c], vals, pk)) {
                    ok = 0;
                    break;
                }
            }
            if (!ok) continue;
        }
        Cand cand;
        memset(&cand, 0, sizeof(cand));
        cand.pk = pk;
        cand.slot = slot;
        cand.dist = dists[i];
        cand.has_dist = 1;
        sel_push(ctx, &cand);
    }
    if (vals) {
        for (int v = 0; v < nvals; v++) mv_value_free(&vals[v]);
        free(vals);
    }
    free(slots);
    free(dists);
    *enough = (!ctx->has_limit || (int64_t)ctx->ncands >= ctx->limit) ? 1 : 0;
    return MV_OK;
}

static int exec_select(MvDb *db, const MvStmt *st, const char *search_mode, MvResult *res,
                       MvError *err) {
    (void)search_mode;
    MvTable *t = mv_db_find_table(db, st->table);
    if (!t) {
        mv_err_set(err, MV_E_TABLE, "table '%s' does not exist", st->table);
        return MV_ERR;
    }
    const MvSchema *s = mv_table_schema(t);
    int nvals = mv_table_nvals(t);
    MvValue *vals = NULL; /* one scratch row reused for every result row */

    int nout = 0;
    int *out_col = NULL;
    int *out_dist = NULL;
    char **out_name = NULL;
    float *query = NULL;

    if (st->star) {
        nout = s->ncols;
    } else {
        nout = st->nprojs;
    }
    out_col = mv_xmalloc((size_t)(nout > 0 ? nout : 1) * sizeof(int));
    out_dist = mv_xmalloc((size_t)(nout > 0 ? nout : 1) * sizeof(int));
    out_name = mv_xmalloc((size_t)(nout > 0 ? nout : 1) * sizeof(char *));

    if (st->star) {
        for (int i = 0; i < nout; i++) {
            out_col[i] = i;
            out_dist[i] = 0;
            out_name[i] = mv_strdup(s->cols[i].name);
        }
    } else {
        for (int i = 0; i < nout; i++) {
            const MvProj *p = &st->projs[i];
            if (p->is_dist) {
                int col = resolve_col(s, p->col_name);
                if (col < 0 || s->cols[col].type != MV_COL_VECTOR) {
                    mv_err_set(err, MV_E_COLUMN, "'%s' is not a vector column", p->col_name);
                    goto fail;
                }
                if (p->vec.dim != s->cols[col].dim) {
                    mv_err_set(err, MV_E_DIM,
                               "query vector has %d dimensions but column '%s' expects %d",
                               p->vec.dim, p->col_name, s->cols[col].dim);
                    goto fail;
                }
                out_col[i] = col;
                out_dist[i] = 1;
                out_name[i] = mv_strdup(p->has_alias ? p->alias : "L2_DISTANCE");
                if (!query) {
                    query = mv_xmalloc((size_t)p->vec.dim * sizeof(float));
                    memcpy(query, p->vec.vec, (size_t)p->vec.dim * sizeof(float));
                }
            } else {
                int col = resolve_col(s, p->col_name);
                if (col < 0) {
                    mv_err_set(err, MV_E_COLUMN, "unknown column '%s'", p->col_name);
                    goto fail;
                }
                out_col[i] = col;
                out_dist[i] = 0;
                out_name[i] = mv_strdup(p->has_alias ? p->alias : s->cols[col].name);
            }
        }
    }

    MvCond *conds = NULL;
    if (st->nconds > 0) {
        conds = mv_xmalloc((size_t)st->nconds * sizeof(MvCond));
        for (int i = 0; i < st->nconds; i++) {
            conds[i] = st->conds[i];
            int col = resolve_col(s, st->conds[i].col_name);
            if (col < 0) {
                mv_err_set(err, MV_E_COLUMN, "unknown column '%s'", st->conds[i].col_name);
                goto fail;
            }
            if (s->cols[col].type == MV_COL_VECTOR) {
                mv_err_set(err, MV_E_UNSUPPORTED, "vector columns cannot be used in conditions");
                goto fail;
            }
            conds[i].col = col;
            conds[i].col2 = -1;
            if (st->conds[i].col2_name[0]) {
                int col2 = resolve_col(s, st->conds[i].col2_name);
                if (col2 < 0) {
                    mv_err_set(err, MV_E_COLUMN, "unknown column '%s'", st->conds[i].col2_name);
                    goto fail;
                }
                conds[i].col2 = col2;
            }
        }
    }

    int order_kind = 0, order_col = -1;
    uint8_t order_desc = 0;
    if (st->norders > 0) {
        const MvOrder *o = &st->orders[0];
        int matched = -1;
        for (int i = 0; i < nout && st->projs; i++) {
            if (st->projs[i].has_alias && strcmp(st->projs[i].alias, o->col_name) == 0) {
                matched = i;
                break;
            }
        }
        if (matched >= 0) {
            if (out_dist[matched]) {
                order_kind = 2;
                if (!query) {
                    query = mv_xmalloc((size_t)st->projs[matched].vec.dim * sizeof(float));
                    memcpy(query, st->projs[matched].vec.vec,
                           (size_t)st->projs[matched].vec.dim * sizeof(float));
                }
            } else {
                order_kind = 1;
                order_col = out_col[matched];
            }
        } else {
            int col = resolve_col(s, o->col_name);
            if (col < 0) {
                mv_err_set(err, MV_E_COLUMN, "unknown ORDER BY expression '%s'", o->col_name);
                goto fail;
            }
            if (s->cols[col].type == MV_COL_VECTOR) {
                mv_err_set(err, MV_E_UNSUPPORTED, "vector columns cannot be used in ORDER BY");
                goto fail;
            }
            order_kind = 1;
            order_col = col;
        }
        order_desc = o->desc;
    }

    SelCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.db = db;
    ctx.t = t;
    ctx.s = s;
    ctx.conds = conds;
    ctx.nconds = st->nconds;
    ctx.order_kind = order_kind;
    ctx.order_col = order_col;
    ctx.order_desc = order_desc;
    ctx.limit = st->has_limit ? st->limit : 0;
    ctx.has_limit = st->has_limit;
    ctx.query = query;

    int rc = MV_ERR;
    int ann_used = 0;
    if (query && order_kind != 1 && search_mode && strcmp(search_mode, "ann") == 0 &&
        mv_table_row_count(t) >= MV_ANN_MIN_ROWS) {
        int enough = 0;
        if (sel_ann_collect(db, t, &ctx, &enough) == MV_OK && enough) {
            ann_used = 1;
            rc = MV_OK;
        } else {
            /* the approximate result is not complete enough: fall back to the
             * exact scan so the answer stays correct */
            for (int i = 0; i < ctx.ncands; i++) mv_value_free(&ctx.cands[i].key);
            ctx.ncands = 0;
        }
    }
    if (!ann_used) {
        if (query && order_kind != 1 && st->nconds > 0) {
            /* A filtered TopK is computed inside the matching set: first mark
             * the rows that satisfy WHERE, then walk the vector column once
             * and keep the distances of the marked slots only. */
            int64_t slots = mv_table_next_slot(t);
            uint8_t *match = mv_xmalloc((size_t)((slots + 8) / 8));
            memset(match, 0, (size_t)((slots + 8) / 8));
            FilterCtx fc;
            memset(&fc, 0, sizeof(fc));
            fc.s = s;
            fc.conds = conds;
            fc.nconds = st->nconds;
            fc.match = match;
            rc = mv_scan_rows(db, t, sel_filter_cb, &fc);
            if (rc == MV_OK) {
                ctx.filter = &fc;
                rc = mv_scan_vectors(db, t, sel_slot_filtered_cb, &ctx);
                ctx.filter = NULL;
            }
            free(match);
        } else if (query && st->nconds == 0 && order_kind != 1) {
            rc = mv_scan_vectors(db, t, sel_slot_cb, &ctx);
        } else {
            rc = mv_scan_rows(db, t, sel_row_cb, &ctx);
        }
    }
    if (rc != MV_OK) {
        mv_err_set(err, MV_E_INTERNAL, "scan failed");
        goto fail;
    }
    sel_sort(&ctx);

    res->is_query = 1;
    res->ncols = nout;
    res->colnames = out_name;
    out_name = NULL;

    for (int i = 0; i < ctx.ncands; i++) {
        int rowidx = mv_result_add_row(res, nout);
        MvValue *out = res->rows[rowidx];
        if (nvals > 0) {
            /* single scratch row for the whole loop: mv_row_values frees the
             * previous contents before decoding the next slot */
            if (!vals) {
                vals = mv_xmalloc((size_t)nvals * sizeof(MvValue));
                memset(vals, 0, (size_t)nvals * sizeof(MvValue));
            }
            if (mv_row_values(db, t, ctx.cands[i].slot, vals) != MV_OK) {
                mv_err_set(err, MV_E_INTERNAL, "cannot read row");
                goto fail;
            }
        }
        for (int c = 0; c < nout; c++) {
            if (out_dist[c]) {
                out[c].type = MV_VAL_DOUBLE;
                out[c].d = ctx.cands[i].dist;
            } else if (out_col[c] == s->pk_idx) {
                out[c].type = MV_VAL_INT;
                out[c].i = ctx.cands[i].pk;
            } else if (out_col[c] == s->vec_idx) {
                int32_t dim = s->cols[s->vec_idx].dim;
                out[c].type = MV_VAL_VEC;
                out[c].dim = dim;
                out[c].vec = mv_xmalloc((size_t)dim * sizeof(float));
                if (mv_get_vector(db, t, ctx.cands[i].slot, out[c].vec) != MV_OK) {
                    mv_value_free(&out[c]);
                    mv_err_set(err, MV_E_INTERNAL, "cannot read vector");
                    goto fail;
                }
            } else {
                int vi = scalar_index(s, out_col[c]);
                mv_value_copy(&out[c], &vals[vi]);
            }
        }
    }
    if (vals) {
        for (int v = 0; v < nvals; v++) mv_value_free(&vals[v]);
        free(vals);
        vals = NULL;
    }

    for (int i = 0; i < ctx.ncands; i++) mv_value_free(&ctx.cands[i].key);
    free(ctx.cands);
    free(conds);
    free(out_col);
    free(out_dist);
    if (out_name) {
        for (int i = 0; i < nout; i++) free(out_name[i]);
        free(out_name);
    }
    free(query);
    return MV_OK;
fail:
    if (vals) {
        for (int v = 0; v < nvals; v++) mv_value_free(&vals[v]);
        free(vals);
    }
    for (int i = 0; i < ctx.ncands; i++) mv_value_free(&ctx.cands[i].key);
    free(ctx.cands);
    free(conds);
    free(out_col);
    free(out_dist);
    if (out_name) {
        for (int i = 0; i < nout; i++) free(out_name[i]);
        free(out_name);
    }
    free(query);
    return MV_ERR;
}

/* ---------------------------------------------------------------- update */
typedef struct {
    MvDb *db;
    MvTable *t;
    const MvSchema *s;
    MvCond *conds;
    int nconds;
    MvValue *new_vals; /* the scalar values after the assignment */
    int nvals;
    float *new_vec;    /* NULL when the vector is not modified */
    int64_t affected;
    MvError *err;
} UpdCtx;

static int upd_row_cb(void *ctx, const MvRow *row, int64_t pk) {
    UpdCtx *c = (UpdCtx *)ctx;
    for (int i = 0; i < c->nconds; i++)
        if (!cond_match(c->s, &c->conds[i], row->vals, pk)) return MV_OK;
    MvValue *merged = mv_xmalloc((size_t)(c->nvals > 0 ? c->nvals : 1) * sizeof(MvValue));
    for (int i = 0; i < c->nvals; i++) mv_value_copy(&merged[i], &c->new_vals[i]);
    if (mv_store_update(c->db, c->t, pk, row->slot, merged, c->nvals, c->new_vec, c->err) != MV_OK) {
        for (int i = 0; i < c->nvals; i++) mv_value_free(&merged[i]);
        free(merged);
        return MV_ERR;
    }
    for (int i = 0; i < c->nvals; i++) mv_value_free(&merged[i]);
    free(merged);
    c->affected++;
    return MV_OK;
}

static int exec_update(MvDb *db, const MvStmt *st, MvResult *res, MvError *err) {
    MvTable *t = mv_db_find_table(db, st->table);
    if (!t) {
        mv_err_set(err, MV_E_TABLE, "table '%s' does not exist", st->table);
        return MV_ERR;
    }
    const MvSchema *s = mv_table_schema(t);
    int nvals = mv_table_nvals(t);

    /* the values after the assignment, defaults for the untouched columns */
    MvValue *new_vals = mv_xmalloc((size_t)(nvals > 0 ? nvals : 1) * sizeof(MvValue));
    for (int i = 0; i < nvals; i++) memset(&new_vals[i], 0, sizeof(MvValue));
    int *assigned = mv_xmalloc((size_t)s->ncols * sizeof(int));
    for (int i = 0; i < s->ncols; i++) assigned[i] = 0;
    float *new_vec = NULL;
    MvCond *conds = NULL;

    for (int i = 0; i < st->nassigns; i++) {
        const MvAssign *a = &st->assigns[i];
        int col = resolve_col(s, a->col_name);
        if (col < 0) {
            mv_err_set(err, MV_E_COLUMN, "unknown column '%s'", a->col_name);
            goto fail;
        }
        if (col == s->pk_idx) {
            mv_err_set(err, MV_E_UNSUPPORTED, "updating the primary key is not supported");
            goto fail;
        }
        if (assigned[col]) {
            mv_err_set(err, MV_E_COLCOUNT, "column '%s' assigned twice", a->col_name);
            goto fail;
        }
        assigned[col] = 1;
        if (s->cols[col].type == MV_COL_VECTOR) {
            MvValue tmp;
            if (coerce_value(s, col, &a->val, &tmp, err) != MV_OK) goto fail;
            if (!new_vec) new_vec = mv_xmalloc((size_t)tmp.dim * sizeof(float));
            memcpy(new_vec, tmp.vec, (size_t)tmp.dim * sizeof(float));
            mv_value_free(&tmp);
        } else {
            MvValue tmp;
            if (coerce_value(s, col, &a->val, &tmp, err) != MV_OK) goto fail;
            int vi = scalar_index(s, col);
            mv_value_free(&new_vals[vi]);
            new_vals[vi] = tmp;
        }
    }

    if (st->nconds > 0) {
        conds = mv_xmalloc((size_t)st->nconds * sizeof(MvCond));
        for (int i = 0; i < st->nconds; i++) {
            conds[i] = st->conds[i];
            int col = resolve_col(s, st->conds[i].col_name);
            if (col < 0) {
                mv_err_set(err, MV_E_COLUMN, "unknown column '%s'", st->conds[i].col_name);
                goto fail;
            }
            if (s->cols[col].type == MV_COL_VECTOR) {
                mv_err_set(err, MV_E_UNSUPPORTED, "vector columns cannot be used in conditions");
                goto fail;
            }
            conds[i].col = col;
            conds[i].col2 = -1;
            if (st->conds[i].col2_name[0]) {
                int col2 = resolve_col(s, st->conds[i].col2_name);
                if (col2 < 0) {
                    mv_err_set(err, MV_E_COLUMN, "unknown column '%s'", st->conds[i].col2_name);
                    goto fail;
                }
                conds[i].col2 = col2;
            }
        }
    }

    UpdCtx uc;
    memset(&uc, 0, sizeof(uc));
    uc.db = db;
    uc.t = t;
    uc.s = s;
    uc.conds = conds;
    uc.nconds = st->nconds;
    uc.new_vals = new_vals;
    uc.nvals = nvals;
    uc.new_vec = new_vec;
    uc.err = err;
    if (mv_scan_rows(db, t, upd_row_cb, &uc) != MV_OK) goto fail;
    res->affected_rows = uc.affected;

    free(conds);
    for (int i = 0; i < nvals; i++) mv_value_free(&new_vals[i]);
    free(new_vals);
    free(assigned);
    free(new_vec);
    return MV_OK;
fail:
    free(conds);
    for (int i = 0; i < nvals; i++) mv_value_free(&new_vals[i]);
    free(new_vals);
    free(assigned);
    free(new_vec);
    return MV_ERR;
}

/* ---------------------------------------------------------------- delete */
typedef struct {
    MvDb *db;
    MvTable *t;
    const MvSchema *s;
    MvCond *conds;
    int nconds;
    int64_t affected;
    MvError *err;
} DelCtx;

static int del_row_cb(void *ctx, const MvRow *row, int64_t pk) {
    DelCtx *c = (DelCtx *)ctx;
    for (int i = 0; i < c->nconds; i++)
        if (!cond_match(c->s, &c->conds[i], row->vals, pk)) return MV_OK;
    if (mv_store_delete(c->db, c->t, pk, row->slot, c->err) != MV_OK) return MV_ERR;
    c->affected++;
    return MV_OK;
}

static int exec_delete(MvDb *db, const MvStmt *st, MvResult *res, MvError *err) {
    MvTable *t = mv_db_find_table(db, st->table);
    if (!t) {
        mv_err_set(err, MV_E_TABLE, "table '%s' does not exist", st->table);
        return MV_ERR;
    }
    const MvSchema *s = mv_table_schema(t);
    MvCond *conds = NULL;
    if (st->nconds > 0) {
        conds = mv_xmalloc((size_t)st->nconds * sizeof(MvCond));
        for (int i = 0; i < st->nconds; i++) {
            conds[i] = st->conds[i];
            int col = resolve_col(s, st->conds[i].col_name);
            if (col < 0) {
                mv_err_set(err, MV_E_COLUMN, "unknown column '%s'", st->conds[i].col_name);
                free(conds);
                return MV_ERR;
            }
            if (s->cols[col].type == MV_COL_VECTOR) {
                mv_err_set(err, MV_E_UNSUPPORTED, "vector columns cannot be used in conditions");
                free(conds);
                return MV_ERR;
            }
            conds[i].col = col;
            conds[i].col2 = -1;
            if (st->conds[i].col2_name[0]) {
                int col2 = resolve_col(s, st->conds[i].col2_name);
                if (col2 < 0) {
                    mv_err_set(err, MV_E_COLUMN, "unknown column '%s'", st->conds[i].col2_name);
                    free(conds);
                    return MV_ERR;
                }
                conds[i].col2 = col2;
            }
        }
    }
    DelCtx dc;
    memset(&dc, 0, sizeof(dc));
    dc.db = db;
    dc.t = t;
    dc.s = s;
    dc.conds = conds;
    dc.nconds = st->nconds;
    dc.err = err;
    if (mv_scan_rows(db, t, del_row_cb, &dc) != MV_OK) {
        free(conds);
        return MV_ERR;
    }
    res->affected_rows = dc.affected;
    free(conds);
    return MV_OK;
}

/* --------------------------------------------------------------- dispatch */
int mv_exec(MvDb *db, const MvStmt *st, const char *search_mode, MvResult *res, MvError *err) {
    int explicit_txn = mv_txn_active(db);
    int mark = mv_store_undo_mark(db);
    uint64_t txn = 0;

    if (!explicit_txn) {
        txn = mv_store_next_txn(db);
        mv_store_set_txn(db, txn);
    }

    int rc;
    switch (st->type) {
        case MV_ST_CREATE_TABLE:
            if (explicit_txn) {
                mv_err_set(err, MV_E_UNSUPPORTED, "schema changes inside a transaction are not supported");
                rc = MV_ERR;
            } else {
                rc = mv_create_table(db, &st->schema, err);
            }
            break;
        case MV_ST_CREATE_INDEX:
            if (explicit_txn) {
                mv_err_set(err, MV_E_UNSUPPORTED, "schema changes inside a transaction are not supported");
                rc = MV_ERR;
            } else {
                rc = mv_create_vector_index(db, st->index_name, st->table, st->schema.cols[0].name, err);
            }
            break;
        case MV_ST_INSERT:
            rc = exec_insert(db, st, res, err);
            break;
        case MV_ST_SELECT:
            rc = exec_select(db, st, search_mode, res, err);
            break;
        case MV_ST_UPDATE:
            rc = exec_update(db, st, res, err);
            break;
        case MV_ST_DELETE:
            rc = exec_delete(db, st, res, err);
            break;
        case MV_ST_BEGIN:
            rc = mv_txn_begin(db, err);
            break;
        case MV_ST_COMMIT:
            rc = mv_txn_commit(db, err);
            break;
        case MV_ST_ROLLBACK:
            rc = mv_txn_rollback(db, err);
            break;
        default:
            mv_err_set(err, MV_E_UNSUPPORTED, "unsupported statement");
            rc = MV_ERR;
            break;
    }

    if (rc != MV_OK) {
        /* a failed statement must not leave partial changes behind */
        mv_store_undo_to(db, mark);
        if (!explicit_txn) {
            mv_store_clear_undo(db);
        }
        return MV_ERR;
    }
    if (!explicit_txn && (st->type == MV_ST_INSERT || st->type == MV_ST_UPDATE ||
                           st->type == MV_ST_DELETE)) {
        if (mv_store_commit_current(db, err) != MV_OK) {
            mv_store_undo_to(db, mark);
            mv_store_clear_undo(db);
            return MV_ERR;
        }
        mv_store_clear_undo(db);
    }
    return MV_OK;
}

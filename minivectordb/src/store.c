/* Storage engine: catalog, append-only row log, dense vector slots, commit log.
 *
 * Durability model
 * ----------------
 * Every row version and every vector slot is appended to a per-table file and
 * tagged with a transaction id. A transaction becomes durable only when its id
 * is appended to commit.log and that file is fsynced; the data files are
 * fsynced before the commit record, so a committed transaction always has its
 * payload on disk.
 *
 * Because the engine allows a single explicit transaction at a time, the
 * uncommitted records of an open transaction always sit at the tail of the
 * files. Recovery therefore replays exactly the committed prefix and discards
 * the tail, which is what makes "committed survives a crash, uncommitted never
 * partially applies" hold.
 */
#include "db.h"
#include "vecidx.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MV_REC_UPSERT 1
#define MV_REC_DELETE 2
#define MV_HDR_BYTES 25 /* txn(8) + flags(1) + pk(8) + slot(8) */

typedef struct {
    int table;
    int64_t pk;
    int64_t old_slot; /* -1 when the key did not exist before the change */
    uint8_t old_alive;
} MvUndo;

typedef struct {
    int table;
    int64_t rows_size;
    int64_t vec_slots;
    int64_t row_slots;
} MvSizeBak;

struct MvTable {
    MvSchema schema;
    int rows_fd;
    int vec_fd;
    int64_t rows_size;
    int64_t vec_slots; /* slots written to the vector file */
    int64_t row_slots; /* row versions for tables without a vector column */
    int32_t dim;
    int nvals; /* scalar columns: every column except the primary key/vector */
    MvMap pk2slot;
    int64_t *slot_pk;
    int64_t *slot_off;
    size_t slot_cap;
    uint8_t *alive;
    size_t alive_bytes;
    char index_name[64];
    uint8_t has_index;
    MvVecIndex *vindex;
    int64_t vindex_stamp;
    /* Hot copy of the vector file. Exact scans would otherwise re-read the
     * whole vector column for every query; the cache is capped so that tables
     * larger than the memory budget keep reading from disk. */
    float *vec_cache;
    int64_t vec_cache_slots;
    int64_t vec_cache_cap;
};

struct MvDb {
    char dir[512];
    MvTable *tables;
    int ntables;
    int tablecap;
    int commit_fd;
    uint64_t next_txn;

    int txn_active;
    uint64_t txn_id;
    MvUndo *undo;
    int undo_n;
    int undo_cap;
    int64_t commit_size_bak;
    MvSizeBak *sizes_bak;
    int n_bak;
    int *dirty;
    int n_dirty;
    int dirty_cap;
};

/* ------------------------------------------------------- buffered reader */
typedef struct {
    int fd;
    int64_t size;
    int64_t pos; /* file offset of buf[0] */
    unsigned char *buf;
    size_t cap, len, off;
} MvReader;

static void reader_init(MvReader *r, int fd, size_t cap) {
    r->fd = fd;
    r->size = mv_os_size(fd);
    r->pos = 0;
    r->buf = mv_xmalloc(cap);
    r->cap = cap;
    r->len = 0;
    r->off = 0;
}

static void reader_free(MvReader *r) { free(r->buf); r->buf = NULL; }

static int reader_need(MvReader *r, size_t n) {
    if (r->len - r->off >= n) return MV_OK;
    if (n > r->cap) {
        r->cap = n * 2;
        r->buf = mv_xrealloc(r->buf, r->cap);
    }
    size_t remain = r->len - r->off;
    if (remain) memmove(r->buf, r->buf + r->off, remain);
    r->pos += (int64_t)r->off;
    r->off = 0;
    r->len = remain;
    while (r->len < n) {
        int64_t want = (int64_t)(r->cap - r->len);
        int64_t left = r->size - (r->pos + (int64_t)r->len);
        if (want > left) want = left;
        if (want <= 0) return MV_ERR;
        int64_t got = mv_os_read(r->fd, r->buf + r->len, want, r->pos + (int64_t)r->len);
        if (got <= 0) return MV_ERR;
        r->len += (size_t)got;
    }
    return MV_OK;
}

static uint32_t rd_u32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd_u64(const unsigned char *p) {
    uint64_t v = 0;
    for (int b = 7; b >= 0; b--) v = (v << 8) | p[b];
    return v;
}

static void wr_u32(unsigned char *p, uint32_t v) {
    for (int b = 0; b < 4; b++) p[b] = (unsigned char)((v >> (8 * b)) & 0xFF);
}

static void wr_u64(unsigned char *p, uint64_t v) {
    for (int b = 0; b < 8; b++) p[b] = (unsigned char)((v >> (8 * b)) & 0xFF);
}

/* --------------------------------------------------------------- helpers */
static void table_path(const MvDb *db, const MvTable *t, const char *suffix, char *out,
                       size_t outlen) {
    snprintf(out, outlen, "%s/%s%s", db->dir, t->schema.name, suffix);
}

static void table_grow_slots(MvTable *t, int64_t need) {
    if (need <= (int64_t)t->slot_cap) return;
    size_t cap = t->slot_cap ? t->slot_cap : 64;
    while ((int64_t)cap < need) cap *= 2;
    t->slot_pk = mv_xrealloc(t->slot_pk, cap * sizeof(int64_t));
    t->slot_off = mv_xrealloc(t->slot_off, cap * sizeof(int64_t));
    t->alive = mv_xrealloc(t->alive, cap);
    memset(t->alive + t->alive_bytes, 0, cap - t->alive_bytes);
    t->alive_bytes = cap;
    t->slot_cap = cap;
}

static void table_set_alive(MvTable *t, int64_t slot, uint8_t v) {
    if (slot < 0) return;
    table_grow_slots(t, slot + 1);
    t->alive[slot] = v ? 1 : 0;
}

static int table_alive(const MvTable *t, int64_t slot) {
    if (slot < 0 || (size_t)slot >= t->alive_bytes) return 0;
    return t->alive[slot] != 0;
}

#define MV_VEC_CACHE_LIMIT (32LL * 1024 * 1024)

/* Load the vector file into memory when it fits into the cache budget. */
static void table_cache_load(MvTable *t) {
    if (t->vec_cache || t->dim <= 0 || t->vec_slots <= 0 || t->vec_fd < 0) return;
    int64_t bytes = t->vec_slots * (int64_t)t->dim * 4;
    if (bytes > MV_VEC_CACHE_LIMIT) return;
    float *cache = mv_xmalloc((size_t)bytes);
    if (mv_os_read(t->vec_fd, cache, bytes, 0) != bytes) {
        free(cache);
        return;
    }
    t->vec_cache = cache;
    t->vec_cache_slots = t->vec_slots;
    t->vec_cache_cap = t->vec_slots;
}

/* Keep the cache in step with an appended vector slot. The buffer grows
 * geometrically: one realloc per slot would copy the whole cache per row. */
static void table_cache_append(MvTable *t, int64_t slot, const float *vec) {
    if (!t->vec_cache || slot != t->vec_cache_slots) return;
    if ((slot + 1) * (int64_t)t->dim * 4 > MV_VEC_CACHE_LIMIT) return;
    if (slot + 1 > t->vec_cache_cap) {
        int64_t cap = t->vec_cache_cap ? t->vec_cache_cap : 64;
        while (cap < slot + 1) cap *= 2;
        if (cap * (int64_t)t->dim * 4 > MV_VEC_CACHE_LIMIT) cap = (MV_VEC_CACHE_LIMIT / ((int64_t)t->dim * 4));
        if (cap < slot + 1) return;
        t->vec_cache = mv_xrealloc(t->vec_cache, (size_t)cap * (size_t)t->dim * sizeof(float));
        t->vec_cache_cap = cap;
    }
    memcpy(t->vec_cache + (size_t)slot * t->dim, vec, (size_t)t->dim * sizeof(float));
    t->vec_cache_slots = slot + 1;
}

static int64_t table_alloc_slot(MvTable *t) {
    if (t->dim > 0) return t->vec_slots++;
    return t->row_slots++;
}

static void table_encode_scalars(const MvTable *t, const MvValue *vals, MvBuf *out) {
    int vi = 0;
    for (int32_t i = 0; i < t->schema.ncols; i++) {
        if (i == t->schema.pk_idx) continue;
        if (i == t->schema.vec_idx) continue;
        const MvValue *v = &vals[vi++];
        if (t->schema.cols[i].type == MV_COL_BIGINT) {
            unsigned char tmp[8];
            wr_u64(tmp, (uint64_t)v->i);
            mv_buf_append(out, tmp, 8);
        } else {
            unsigned char tmp[4];
            wr_u32(tmp, (uint32_t)v->slen);
            mv_buf_append(out, tmp, 4);
            if (v->slen) mv_buf_append(out, v->s, v->slen);
        }
    }
}

/* Decode the scalar payload into `out`. The caller owns the array and may
 * pass the same one for every row of a scan: existing string buffers are
 * reused (grown only when a longer value appears), so a full-table scan
 * neither allocates nor leaks per row. Callers must hand in zeroed slots
 * (or slots holding previously decoded values of the same schema). */
static int table_decode_scalars(const MvTable *t, const unsigned char *rec, size_t rec_len,
                                 size_t *pos, MvValue *out) {
    int vi = 0;
    for (int32_t i = 0; i < t->schema.ncols; i++) {
        if (i == t->schema.pk_idx) continue;
        if (i == t->schema.vec_idx) continue;
        MvValue *v = &out[vi++];
        if (t->schema.cols[i].type == MV_COL_BIGINT) {
            if (*pos + 8 > rec_len) return MV_ERR;
            v->type = MV_VAL_INT;
            v->i = (int64_t)rd_u64(rec + *pos);
            *pos += 8;
        } else {
            if (*pos + 4 > rec_len) return MV_ERR;
            uint32_t len = rd_u32(rec + *pos);
            *pos += 4;
            if (*pos + len > rec_len) return MV_ERR;
            v->type = MV_VAL_STR;
            if (!v->s || (uint32_t)v->slen < len) v->s = mv_xrealloc(v->s, (size_t)len + 1);
            memcpy(v->s, (const char *)rec + *pos, len);
            v->s[len] = '\0';
            v->slen = (int32_t)len;
            *pos += len;
        }
    }
    return MV_OK;
}

/* ------------------------------------------------------------ undo stack */
static void db_undo_push(MvDb *db, int table, int64_t pk, int64_t old_slot, uint8_t old_alive) {
    if (db->undo_n == db->undo_cap) {
        db->undo_cap = db->undo_cap ? db->undo_cap * 2 : 64;
        db->undo = mv_xrealloc(db->undo, (size_t)db->undo_cap * sizeof(MvUndo));
    }
    MvUndo *u = &db->undo[db->undo_n++];
    u->table = table;
    u->pk = pk;
    u->old_slot = old_slot;
    u->old_alive = old_alive;
}

static void db_mark_dirty(MvDb *db, int table) {
    for (int i = 0; i < db->n_dirty; i++)
        if (db->dirty[i] == table) return;
    if (db->n_dirty == db->dirty_cap) {
        db->dirty_cap = db->dirty_cap ? db->dirty_cap * 2 : 8;
        db->dirty = mv_xrealloc(db->dirty, (size_t)db->dirty_cap * sizeof(int));
    }
    db->dirty[db->n_dirty++] = table;
}

static void db_undo_to(MvDb *db, int mark) {
    while (db->undo_n > mark) {
        MvUndo *u = &db->undo[--db->undo_n];
        MvTable *t = &db->tables[u->table];
        if (u->old_slot < 0) {
            mv_map_del(&t->pk2slot, u->pk);
        } else {
            int64_t prev = 0;
            if (mv_map_get(&t->pk2slot, u->pk, &prev) == MV_OK) {
                mv_map_del(&t->pk2slot, u->pk);
                mv_map_put(&t->pk2slot, u->pk, u->old_slot);
            }
            table_set_alive(t, u->old_slot, u->old_alive);
        }
    }
}

/* --------------------------------------------------------------- catalog */
static void catalog_format(const MvDb *db, MvBuf *out) {
    mv_buf_printf(out, "MVDB 1\n");
    for (int i = 0; i < db->ntables; i++) {
        const MvTable *t = &db->tables[i];
        mv_buf_printf(out, "T %s %d %d %d\n", t->schema.name, t->schema.ncols, t->schema.pk_idx,
                      t->schema.vec_idx);
        for (int32_t c = 0; c < t->schema.ncols; c++) {
            const MvColumn *col = &t->schema.cols[c];
            const char *type = col->type == MV_COL_BIGINT   ? "BIGINT"
                               : col->type == MV_COL_VARCHAR ? "VARCHAR"
                                                             : "VECTOR";
            int32_t param = col->type == MV_COL_VARCHAR ? col->length : col->dim;
            mv_buf_printf(out, "C %s %s %d %d\n", col->name, type, param, col->is_pk ? 1 : 0);
        }
        if (t->has_index && t->schema.vec_idx >= 0)
            mv_buf_printf(out, "I %s %s %s\n", t->index_name, t->schema.name,
                          t->schema.cols[t->schema.vec_idx].name);
    }
}

static int db_write_catalog(MvDb *db, MvError *err) {
    MvBuf b;
    mv_buf_init(&b);
    catalog_format(db, &b);
    int64_t len = (int64_t)b.len;
    char tmp[600], dst[600];
    snprintf(tmp, sizeof(tmp), "%s/catalog.tmp", db->dir);
    snprintf(dst, sizeof(dst), "%s/%s", db->dir, "catalog.dat");
    int fd = mv_os_open(tmp, 1);
    if (fd < 0) {
        mv_buf_free(&b);
        mv_err_set(err, MV_E_IO, "cannot write catalog");
        return MV_ERR;
    }
    int64_t w = mv_os_write(fd, b.data ? b.data : "", len, 0);
    mv_os_fsync(fd);
    mv_os_close(fd);
    mv_buf_free(&b);
    if (w != len) {
        mv_err_set(err, MV_E_IO, "short catalog write");
        return MV_ERR;
    }
    if (mv_os_rename(tmp, dst) != 0) {
        mv_err_set(err, MV_E_IO, "cannot rename catalog");
        return MV_ERR;
    }
    mv_os_sync_dir(db->dir);
    return MV_OK;
}

/* ------------------------------------------------------------- commit log */
static int db_append_commit(MvDb *db, uint64_t txn_id, MvError *err) {
    unsigned char rec[12];
    wr_u64(rec, txn_id);
    wr_u32(rec + 8, mv_crc32(rec, 8, 0));
    int64_t off = mv_os_size(db->commit_fd);
    if (mv_os_write(db->commit_fd, rec, 12, off) != 12) {
        mv_err_set(err, MV_E_IO, "cannot append to commit log");
        return MV_ERR;
    }
    return MV_OK;
}

/* ------------------------------------------------------------- open/close */
static int db_recover(MvDb *db, MvError *err);

static int db_add_table(MvDb *db, const MvSchema *schema) {
    if (db->ntables == db->tablecap) {
        db->tablecap = db->tablecap ? db->tablecap * 2 : 8;
        db->tables = mv_xrealloc(db->tables, (size_t)db->tablecap * sizeof(MvTable));
    }
    MvTable *t = &db->tables[db->ntables];
    memset(t, 0, sizeof(*t));
    t->schema = *schema;
    t->rows_fd = -1;
    t->vec_fd = -1;
    t->dim = schema->vec_idx >= 0 ? schema->cols[schema->vec_idx].dim : 0;
    t->nvals = schema->ncols - (schema->pk_idx >= 0 ? 1 : 0) - (schema->vec_idx >= 0 ? 1 : 0);
    t->vindex_stamp = -1;
    mv_map_init(&t->pk2slot);
    db->ntables++;
    return db->ntables - 1;
}

static void db_free_table(MvTable *t) {
    if (t->rows_fd >= 0) mv_os_close(t->rows_fd);
    if (t->vec_fd >= 0) mv_os_close(t->vec_fd);
    mv_map_free(&t->pk2slot);
    free(t->slot_pk);
    free(t->slot_off);
    free(t->alive);
    free(t->vec_cache);
    if (t->vindex) mv_vidx_free(t->vindex);
}

MvDb *mv_db_open(const char *dir, MvError *err) {
    MvDb *db = mv_xmalloc(sizeof(MvDb));
    memset(db, 0, sizeof(*db));
    snprintf(db->dir, sizeof(db->dir), "%s", dir);
    db->commit_fd = -1;
    mv_os_mkdir(dir);

    char path[600];
    snprintf(path, sizeof(path), "%s/catalog.dat", dir);
    if (mv_os_exists(path)) {
        int fd = mv_os_open(path, 0);
        if (fd >= 0) {
            int64_t size = mv_os_size(fd);
            char *text = mv_xmalloc((size_t)size + 1);
            int64_t got = mv_os_read(fd, text, size, 0);
            text[got > 0 ? got : 0] = '\0';
            mv_os_close(fd);
            char *save = NULL;
            for (char *line = strtok_r(text, "\n", &save); line;
                 line = strtok_r(NULL, "\n", &save)) {
                if (line[0] == 'T' && line[1] == ' ') {
                    MvSchema s;
                    memset(&s, 0, sizeof(s));
                    int ncols = 0, pk = -1, vec = -1;
                    if (sscanf(line + 2, "%63s %d %d %d", s.name, &ncols, &pk, &vec) >= 4) {
                        /* the column count is rebuilt from the C lines */
                        s.ncols = 0;
                        s.pk_idx = pk;
                        s.vec_idx = vec;
                        db_add_table(db, &s);
                    }
                } else if (line[0] == 'C' && line[1] == ' ') {
                    int idx = db->ntables - 1;
                    if (idx < 0) continue;
                    char name[64], type[16];
                    int param = 0, ispk = 0;
                    if (sscanf(line + 2, "%63s %15s %d %d", name, type, &param, &ispk) >= 3) {
                        MvTable *t = &db->tables[idx];
                        int32_t c = t->schema.ncols;
                        if (c >= (int32_t)(sizeof(t->schema.cols) / sizeof(t->schema.cols[0])))
                            continue;
                        snprintf(t->schema.cols[c].name, sizeof(t->schema.cols[c].name), "%s", name);
                        if (strcmp(type, "BIGINT") == 0) {
                            t->schema.cols[c].type = MV_COL_BIGINT;
                        } else if (strcmp(type, "VARCHAR") == 0) {
                            t->schema.cols[c].type = MV_COL_VARCHAR;
                            t->schema.cols[c].length = param;
                        } else {
                            t->schema.cols[c].type = MV_COL_VECTOR;
                            t->schema.cols[c].dim = param;
                        }
                        t->schema.cols[c].is_pk = (uint8_t)ispk;
                        t->schema.ncols = c + 1;
                    }
                } else if (line[0] == 'I' && line[1] == ' ') {
                    int idx = db->ntables - 1;
                    if (idx < 0) continue;
                    char iname[64], tname[64], cname[64];
                    if (sscanf(line + 2, "%63s %63s %63s", iname, tname, cname) == 3) {
                        MvTable *t = &db->tables[idx];
                        snprintf(t->index_name, sizeof(t->index_name), "%s", iname);
                        t->has_index = 1;
                    }
                }
            }
            free(text);
        }
    }

    for (int i = 0; i < db->ntables; i++) {
        MvTable *t = &db->tables[i];
        t->dim = t->schema.vec_idx >= 0 ? t->schema.cols[t->schema.vec_idx].dim : 0;
        t->nvals = t->schema.ncols - (t->schema.pk_idx >= 0 ? 1 : 0) -
                   (t->schema.vec_idx >= 0 ? 1 : 0);
        mv_map_init(&t->pk2slot);
        char rp[600], vp[600];
        table_path(db, t, ".rows", rp, sizeof(rp));
        table_path(db, t, ".vec", vp, sizeof(vp));
        t->rows_fd = mv_os_open(rp, 1);
        t->vec_fd = t->dim > 0 ? mv_os_open(vp, 1) : -1;
        if (t->rows_fd < 0) {
            mv_err_set(err, MV_E_IO, "cannot open %s", rp);
            mv_db_close(db);
            return NULL;
        }
    }

    snprintf(path, sizeof(path), "%s/commit.log", dir);
    db->commit_fd = mv_os_open(path, 1);
    if (db->commit_fd < 0) {
        mv_err_set(err, MV_E_IO, "cannot open commit log");
        mv_db_close(db);
        return NULL;
    }

    if (db_recover(db, err) != MV_OK) {
        mv_db_close(db);
        return NULL;
    }
    return db;
}

void mv_db_close(MvDb *db) {
    if (!db) return;
    if (db->txn_active) mv_txn_rollback(db, NULL);
    for (int i = 0; i < db->ntables; i++) db_free_table(&db->tables[i]);
    free(db->tables);
    if (db->commit_fd >= 0) mv_os_close(db->commit_fd);
    free(db->undo);
    free(db->sizes_bak);
    free(db->dirty);
    free(db);
}

const char *mv_db_dir(const MvDb *db) { return db->dir; }
int mv_db_table_count(const MvDb *db) { return db->ntables; }

MvTable *mv_db_find_table(MvDb *db, const char *name) {
    for (int i = 0; i < db->ntables; i++)
        if (strcmp(db->tables[i].schema.name, name) == 0) return &db->tables[i];
    return NULL;
}

/* ------------------------------------------------------------- recovery */
static int u64_in_sorted(const uint64_t *arr, size_t n, uint64_t key) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (arr[mid] == key) return 1;
        if (arr[mid] < key) lo = mid + 1;
        else hi = mid;
    }
    return 0;
}

static int db_recover(MvDb *db, MvError *err) {
    /* 1. committed transaction ids (the log is written in increasing order) */
    int64_t clog_size = mv_os_size(db->commit_fd);
    uint64_t *committed = NULL;
    size_t n_committed = 0, cap = 0;
    if (clog_size >= 12) {
        unsigned char *buf = mv_xmalloc((size_t)clog_size);
        int64_t got = mv_os_read(db->commit_fd, buf, clog_size, 0);
        if (got < 0) got = 0;
        for (int64_t off = 0; off + 12 <= got;) {
            uint64_t txn = rd_u64(buf + off);
            uint32_t crc = rd_u32(buf + off + 8);
            if (crc != mv_crc32(buf + off, 8, 0)) break;
            if (n_committed == cap) {
                cap = cap ? cap * 2 : 64;
                committed = mv_xrealloc(committed, cap * sizeof(uint64_t));
            }
            committed[n_committed++] = txn;
            off += 12;
        }
        free(buf);
    }

    /* 2. replay the row logs */
    uint64_t max_txn = 0;
    for (int ti = 0; ti < db->ntables; ti++) {
        MvTable *t = &db->tables[ti];
        int64_t size = mv_os_size(t->rows_fd);
        int64_t last_good = 0;
        int64_t max_slot = -1;
        if (size > 0) {
            MvReader r;
            reader_init(&r, t->rows_fd, 1 << 20);
            int64_t off = 0;
            for (;;) {
                if (reader_need(&r, 4) != MV_OK) break;
                uint32_t rec_len = rd_u32(r.buf + r.off);
                if (rec_len < MV_HDR_BYTES + 4) break;
                if (reader_need(&r, 4 + rec_len) != MV_OK) break;
                unsigned char *rec = r.buf + r.off + 4;
                uint32_t crc = rd_u32(rec + rec_len - 4);
                if (crc != mv_crc32(rec, rec_len - 4, 0)) break;
                uint64_t txn = rd_u64(rec);
                uint8_t flags = rec[8];
                int64_t pk = (int64_t)rd_u64(rec + 9);
                int64_t slot = (int64_t)rd_u64(rec + 17);
                r.off += 4 + rec_len;
                off += 4 + rec_len;
                if (txn > max_txn) max_txn = txn;
                if (!u64_in_sorted(committed, n_committed, txn)) continue;
                last_good = off;
                if (slot > max_slot) max_slot = slot;
                if (flags == MV_REC_DELETE) {
                    mv_map_del(&t->pk2slot, pk);
                    table_set_alive(t, slot, 0);
                } else {
                    table_grow_slots(t, slot + 1);
                    t->slot_pk[slot] = pk;
                    t->slot_off[slot] = off - (int64_t)rec_len - 4;
                    table_set_alive(t, slot, 1);
                    int64_t prev = 0;
                    if (mv_map_get(&t->pk2slot, pk, &prev) == MV_OK)
                        mv_map_del(&t->pk2slot, pk);
                    mv_map_put(&t->pk2slot, pk, slot);
                }
            }
            reader_free(&r);
        }
        if (last_good < size) mv_os_truncate(t->rows_fd, last_good);
        t->rows_size = last_good;
        if (t->dim > 0 && t->vec_fd >= 0) {
            int64_t want = (max_slot + 1) * (int64_t)t->dim * 4;
            int64_t have = mv_os_size(t->vec_fd);
            if (have > want) mv_os_truncate(t->vec_fd, want);
            t->vec_slots = want / ((int64_t)t->dim * 4);
        } else {
            t->vec_slots = 0;
        }
        t->row_slots = t->dim > 0 ? t->vec_slots : (max_slot + 1);
    }
    free(committed);
    db->next_txn = max_txn + 1;
    (void)err;
    return MV_OK;
}

/* ------------------------------------------------------------- DDL */
int mv_create_table(MvDb *db, const MvSchema *schema, MvError *err) {
    if (mv_db_find_table(db, schema->name)) {
        mv_err_set(err, MV_E_DUP_TABLE, "table '%s' already exists", schema->name);
        return MV_ERR;
    }
    int idx = db_add_table(db, schema);
    MvTable *t = &db->tables[idx];
    char rp[600], vp[600];
    table_path(db, t, ".rows", rp, sizeof(rp));
    table_path(db, t, ".vec", vp, sizeof(vp));
    t->rows_fd = mv_os_open(rp, 1);
    if (t->rows_fd < 0) {
        mv_err_set(err, MV_E_IO, "cannot create table file");
        return MV_ERR;
    }
    if (t->dim > 0) {
        t->vec_fd = mv_os_open(vp, 1);
        if (t->vec_fd < 0) {
            mv_err_set(err, MV_E_IO, "cannot create vector file");
            return MV_ERR;
        }
    }
    t->rows_size = 0;
    t->vec_slots = 0;
    t->row_slots = 0;
    return db_write_catalog(db, err);
}

int mv_create_vector_index(MvDb *db, const char *index_name, const char *table, const char *column,
                           MvError *err) {
    MvTable *t = mv_db_find_table(db, table);
    if (!t) {
        mv_err_set(err, MV_E_TABLE, "table '%s' does not exist", table);
        return MV_ERR;
    }
    if (t->schema.vec_idx < 0) {
        mv_err_set(err, MV_E_COLUMN, "table '%s' has no vector column", table);
        return MV_ERR;
    }
    if (strcmp(t->schema.cols[t->schema.vec_idx].name, column) != 0) {
        mv_err_set(err, MV_E_COLUMN, "column '%s' is not the vector column", column);
        return MV_ERR;
    }
    for (int i = 0; i < db->ntables; i++) {
        if (db->tables[i].has_index && strcmp(db->tables[i].index_name, index_name) == 0) {
            mv_err_set(err, MV_E_DUP_INDEX, "index '%s' already exists", index_name);
            return MV_ERR;
        }
    }
    snprintf(t->index_name, sizeof(t->index_name), "%s", index_name);
    t->has_index = 1;
    return db_write_catalog(db, err);
}

/* ----------------------------------------------------------- row writing */
static int64_t table_append_vector(MvTable *t, int64_t slot, const float *vec) {
    if (t->dim <= 0 || t->vec_fd < 0) return -1;
    if (mv_os_write(t->vec_fd, vec, (int64_t)t->dim * 4, slot * (int64_t)t->dim * 4) !=
        (int64_t)t->dim * 4)
        return -1;
    return slot;
}

static int64_t table_append_row(MvTable *t, uint64_t txn, uint8_t flags, int64_t pk, int64_t slot,
                                 const MvValue *scalars) {
    MvBuf payload;
    mv_buf_init(&payload);
    if (flags == MV_REC_UPSERT) table_encode_scalars(t, scalars, &payload);
    unsigned char header[MV_HDR_BYTES];
    wr_u64(header, txn);
    header[8] = flags;
    wr_u64(header + 9, (uint64_t)pk);
    wr_u64(header + 17, (uint64_t)slot);
    uint32_t rec_len = (uint32_t)(MV_HDR_BYTES + payload.len + 4);
    unsigned char lenbuf[4], crcb[4];
    wr_u32(lenbuf, rec_len);
    uint32_t crc = mv_crc32(header, MV_HDR_BYTES, 0);
    crc = mv_crc32(payload.data ? payload.data : "", payload.len, crc);
    wr_u32(crcb, crc);

    int64_t off = t->rows_size;
    int rc = MV_OK;
    if (mv_os_write(t->rows_fd, lenbuf, 4, off) != 4) rc = MV_ERR;
    else if (mv_os_write(t->rows_fd, header, MV_HDR_BYTES, off + 4) != MV_HDR_BYTES) rc = MV_ERR;
    else if (payload.len && mv_os_write(t->rows_fd, payload.data, (int64_t)payload.len,
                                        off + 4 + MV_HDR_BYTES) != (int64_t)payload.len)
        rc = MV_ERR;
    else if (mv_os_write(t->rows_fd, crcb, 4, off + 4 + MV_HDR_BYTES + (int64_t)payload.len) != 4)
        rc = MV_ERR;
    mv_buf_free(&payload);
    if (rc != MV_OK) return -1;
    t->rows_size = off + 4 + rec_len;
    return off;
}

/* --------------------------------------------------------------- queries */
int mv_scan_rows(MvDb *db, MvTable *t, MvRowFn fn, void *ctx) {
    (void)db;
    int64_t size = mv_os_size(t->rows_fd);
    if (size <= 0) return MV_OK;
    MvReader r;
    reader_init(&r, t->rows_fd, 1 << 20);
    MvValue *vals = mv_xmalloc((size_t)(t->nvals > 0 ? t->nvals : 1) * sizeof(MvValue));
    memset(vals, 0, (size_t)(t->nvals > 0 ? t->nvals : 1) * sizeof(MvValue));
    int rc = MV_OK;
    for (;;) {
        if (reader_need(&r, 4) != MV_OK) break;
        uint32_t rec_len = rd_u32(r.buf + r.off);
        if (rec_len < MV_HDR_BYTES + 4) break;
        if (reader_need(&r, 4 + rec_len) != MV_OK) break;
        unsigned char *rec = r.buf + r.off + 4;
        uint8_t flags = rec[8];
        int64_t pk = (int64_t)rd_u64(rec + 9);
        int64_t slot = (int64_t)rd_u64(rec + 17);
        if (flags == MV_REC_UPSERT) {
            int64_t cur = 0;
            if (mv_map_get(&t->pk2slot, pk, &cur) == MV_OK && cur == slot &&
                table_alive(t, slot)) {
                size_t p = MV_HDR_BYTES;
                /* the decoder overwrites the previous row in place */
                if (table_decode_scalars(t, rec, rec_len - 4, &p, vals) == MV_OK) {
                    MvRow row;
                    row.pk = pk;
                    row.slot = (int32_t)slot;
                    row.nvals = t->nvals;
                    row.vals = vals;
                    rc = fn(ctx, &row, pk);
                    if (rc != MV_OK) break;
                }
            }
        }
        r.off += 4 + rec_len;
    }
    for (int32_t v = 0; v < t->nvals; v++) mv_value_free(&vals[v]);
    free(vals);
    reader_free(&r);
    return rc;
}

int mv_scan_vectors(MvDb *db, MvTable *t, MvSlotFn fn, void *ctx) {
    (void)db;
    if (t->dim <= 0 || t->vec_fd < 0 || t->vec_slots <= 0) return MV_OK;
    table_cache_load(t);
    if (t->vec_cache) {
        for (int64_t slot = 0; slot < t->vec_cache_slots; slot++) {
            if (!table_alive(t, slot)) continue;
            int64_t pk = t->slot_pk[slot];
            int64_t cur = 0;
            if (mv_map_get(&t->pk2slot, pk, &cur) != MV_OK || cur != slot) continue;
            if (fn(ctx, (int32_t)slot, pk, t->vec_cache + (size_t)slot * t->dim) != MV_OK) return MV_ERR;
        }
        return MV_OK;
    }
    const int64_t CHUNK = 1 << 20;
    int64_t slots_per_chunk = CHUNK / ((int64_t)t->dim * 4);
    if (slots_per_chunk < 1) slots_per_chunk = 1;
    float *buf = mv_xmalloc((size_t)slots_per_chunk * (size_t)t->dim * sizeof(float));
    int64_t done = 0;
    int rc = MV_OK;
    while (done < t->vec_slots) {
        int64_t n = t->vec_slots - done;
        if (n > slots_per_chunk) n = slots_per_chunk;
        int64_t bytes = n * (int64_t)t->dim * 4;
        if (mv_os_read(t->vec_fd, buf, bytes, done * (int64_t)t->dim * 4) != bytes) break;
        for (int64_t i = 0; i < n; i++) {
            int64_t slot = done + i;
            if (!table_alive(t, slot)) continue;
            int64_t pk = t->slot_pk[slot];
            int64_t cur = 0;
            if (mv_map_get(&t->pk2slot, pk, &cur) != MV_OK || cur != slot) continue;
            rc = fn(ctx, (int32_t)slot, pk, buf + i * t->dim);
            if (rc != MV_OK) {
                free(buf);
                return MV_ERR;
            }
        }
        done += n;
    }
    free(buf);
    return rc;
}

int mv_get_vector(MvDb *db, MvTable *t, int32_t slot, float *out) {
    (void)db;
    if (t->dim <= 0 || t->vec_fd < 0 || slot < 0) return MV_ERR;
    if (mv_os_read(t->vec_fd, out, (int64_t)t->dim * 4, (int64_t)slot * t->dim * 4) !=
        (int64_t)t->dim * 4)
        return MV_ERR;
    return MV_OK;
}

int mv_row_values(MvDb *db, MvTable *t, int32_t slot, MvValue *out) {
    (void)db;
    if (slot < 0 || (size_t)slot >= t->slot_cap) return MV_ERR;
    int64_t off = t->slot_off[slot];
    if (off < 0) return MV_ERR;
    unsigned char lenbuf[4];
    if (mv_os_read(t->rows_fd, lenbuf, 4, off) != 4) return MV_ERR;
    uint32_t rec_len = rd_u32(lenbuf);
    if (rec_len < MV_HDR_BYTES + 4 || rec_len > (1u << 24)) return MV_ERR;
    unsigned char *rec = mv_xmalloc(rec_len);
    int rc = MV_ERR;
    /* release whatever the caller's scratch row still holds, then decode
     * into the same slots (the string buffers are reused, not leaked) */
    for (int i = 0; i < t->nvals; i++) mv_value_free(&out[i]);
    if (mv_os_read(t->rows_fd, rec, rec_len, off + 4) == (int64_t)rec_len) {
        size_t p = MV_HDR_BYTES;
        if (table_decode_scalars(t, rec, rec_len - 4, &p, out) == MV_OK) rc = MV_OK;
    }
    free(rec);
    return rc;
}

/* -------------------------------------------------------------- mutation */
int mv_store_insert(MvDb *db, MvTable *t, int64_t pk, const MvValue *scalars, int nvals,
                    const float *vec, MvError *err) {
    (void)nvals;
    int64_t existing = 0;
    if (mv_map_get(&t->pk2slot, pk, &existing) == MV_OK) {
        mv_err_set(err, MV_E_DUP_KEY, "duplicate primary key %lld", (long long)pk);
        return MV_ERR;
    }
    int64_t slot = table_alloc_slot(t);
    if (t->dim > 0) {
        if (table_append_vector(t, slot, vec) < 0) {
            mv_err_set(err, MV_E_IO, "cannot append vector");
            return MV_ERR;
        }
        table_cache_append(t, slot, vec);
    }
    int64_t off = table_append_row(t, db->txn_id, MV_REC_UPSERT, pk, slot, scalars);
    if (off < 0) {
        mv_err_set(err, MV_E_IO, "cannot append row");
        return MV_ERR;
    }
    table_grow_slots(t, slot + 1);
    t->slot_pk[slot] = pk;
    t->slot_off[slot] = off;
    table_set_alive(t, slot, 1);
    mv_map_put(&t->pk2slot, pk, slot);
    db_undo_push(db, (int)(t - db->tables), pk, -1, 0);
    db_mark_dirty(db, (int)(t - db->tables));
    mv_store_invalidate_index(t);
    return MV_OK;
}

int mv_store_update(MvDb *db, MvTable *t, int64_t pk, int32_t old_slot, const MvValue *scalars,
                     int nvals, const float *vec, MvError *err) {
    (void)nvals;
    int64_t slot = table_alloc_slot(t);
    if (t->dim > 0) {
        float *keep = NULL;
        if (!vec) {
            keep = mv_xmalloc((size_t)t->dim * sizeof(float));
            if (mv_get_vector(db, t, old_slot, keep) != MV_OK) {
                free(keep);
                mv_err_set(err, MV_E_INTERNAL, "cannot read vector");
                return MV_ERR;
            }
            vec = keep;
        }
        int ok = table_append_vector(t, slot, vec) >= 0;
        if (ok) table_cache_append(t, slot, vec);
        free(keep);
        if (!ok) {
            mv_err_set(err, MV_E_IO, "cannot append vector");
            return MV_ERR;
        }
    }
    int64_t off = table_append_row(t, db->txn_id, MV_REC_UPSERT, pk, slot, scalars);
    if (off < 0) {
        mv_err_set(err, MV_E_IO, "cannot append row");
        return MV_ERR;
    }
    uint8_t old_alive = (uint8_t)table_alive(t, old_slot);
    table_grow_slots(t, slot + 1);
    t->slot_pk[slot] = pk;
    t->slot_off[slot] = off;
    table_set_alive(t, slot, 1);
    table_set_alive(t, old_slot, 0);
    int64_t prev = 0;
    if (mv_map_get(&t->pk2slot, pk, &prev) == MV_OK) mv_map_del(&t->pk2slot, pk);
    mv_map_put(&t->pk2slot, pk, slot);
    db_undo_push(db, (int)(t - db->tables), pk, old_slot, old_alive);
    db_mark_dirty(db, (int)(t - db->tables));
    mv_store_invalidate_index(t);
    return MV_OK;
}

int mv_store_delete(MvDb *db, MvTable *t, int64_t pk, int32_t old_slot, MvError *err) {
    int64_t off = table_append_row(t, db->txn_id, MV_REC_DELETE, pk, old_slot, NULL);
    if (off < 0) {
        mv_err_set(err, MV_E_IO, "cannot append row");
        return MV_ERR;
    }
    uint8_t old_alive = (uint8_t)table_alive(t, old_slot);
    table_set_alive(t, old_slot, 0);
    mv_map_del(&t->pk2slot, pk);
    db_undo_push(db, (int)(t - db->tables), pk, old_slot, old_alive);
    db_mark_dirty(db, (int)(t - db->tables));
    mv_store_invalidate_index(t);
    return MV_OK;
}

/* ------------------------------------------------------------ transaction */
int mv_txn_active(const MvDb *db) { return db->txn_active; }

int mv_txn_begin(MvDb *db, MvError *err) {
    if (db->txn_active) {
        mv_err_set(err, MV_E_UNSUPPORTED, "nested transactions are not supported");
        return MV_ERR;
    }
    db->txn_active = 1;
    db->txn_id = db->next_txn++;
    db->undo_n = 0;
    db->n_dirty = 0;
    db->commit_size_bak = mv_os_size(db->commit_fd);
    free(db->sizes_bak);
    db->sizes_bak = mv_xmalloc((size_t)(db->ntables ? db->ntables : 1) * sizeof(MvSizeBak));
    db->n_bak = 0;
    for (int i = 0; i < db->ntables; i++) {
        db->sizes_bak[db->n_bak].table = i;
        db->sizes_bak[db->n_bak].rows_size = db->tables[i].rows_size;
        db->sizes_bak[db->n_bak].vec_slots = db->tables[i].vec_slots;
        db->sizes_bak[db->n_bak].row_slots = db->tables[i].row_slots;
        db->n_bak++;
    }
    return MV_OK;
}

static int db_commit_current(MvDb *db, MvError *err) {
    for (int i = 0; i < db->n_dirty; i++) {
        MvTable *t = &db->tables[db->dirty[i]];
        if (t->rows_fd >= 0) mv_os_fsync(t->rows_fd);
        if (t->vec_fd >= 0) mv_os_fsync(t->vec_fd);
    }
    if (db_append_commit(db, db->txn_id, err) != MV_OK) return MV_ERR;
    if (mv_os_fsync(db->commit_fd) != 0) {
        mv_err_set(err, MV_E_IO, "cannot fsync commit log");
        return MV_ERR;
    }
    return MV_OK;
}

int mv_txn_commit(MvDb *db, MvError *err) {
    if (!db->txn_active) {
        mv_err_set(err, MV_E_NO_TXN, "no transaction is active");
        return MV_ERR;
    }
    if (db_commit_current(db, err) != MV_OK) return MV_ERR;
    db->txn_active = 0;
    db->undo_n = 0;
    db->n_dirty = 0;
    free(db->sizes_bak);
    db->sizes_bak = NULL;
    db->n_bak = 0;
    return MV_OK;
}

int mv_txn_rollback(MvDb *db, MvError *err) {
    if (!db->txn_active) {
        mv_err_set(err, MV_E_NO_TXN, "no transaction is active");
        return MV_ERR;
    }
    db_undo_to(db, 0);
    for (int i = 0; i < db->n_bak; i++) {
        MvSizeBak *b = &db->sizes_bak[i];
        MvTable *t = &db->tables[b->table];
        if (t->rows_fd >= 0) {
            mv_os_truncate(t->rows_fd, b->rows_size);
            t->rows_size = b->rows_size;
        }
        if (t->vec_fd >= 0) {
            mv_os_truncate(t->vec_fd, b->vec_slots * (int64_t)t->dim * 4);
            t->vec_slots = b->vec_slots;
        }
        if (t->vec_cache && t->vec_cache_slots > b->vec_slots) t->vec_cache_slots = b->vec_slots;
        t->row_slots = b->row_slots;
        mv_store_invalidate_index(t);
    }
    if (db->commit_fd >= 0) mv_os_truncate(db->commit_fd, db->commit_size_bak);
    db->txn_active = 0;
    db->undo_n = 0;
    db->n_dirty = 0;
    free(db->sizes_bak);
    db->sizes_bak = NULL;
    db->n_bak = 0;
    (void)err;
    return MV_OK;
}

/* Build (or reuse) the approximate index of a table from its vector file. */
MvVecIndex *mv_store_ensure_index(MvDb *db, MvTable *t) {
    (void)db;
    if (t->vindex && t->vindex_stamp == t->vec_slots) return t->vindex;
    if (t->vindex) mv_vidx_free(t->vindex);
    t->vindex = NULL;
    t->vindex_stamp = -1;
    if (t->dim <= 0 || t->vec_slots <= 0 || t->vec_fd < 0) return NULL;

    int64_t n = t->vec_slots;
    float *base = mv_xmalloc((size_t)n * (size_t)t->dim * sizeof(float));
    if (mv_os_read(t->vec_fd, base, n * (int64_t)t->dim * 4, 0) != n * (int64_t)t->dim * 4) {
        free(base);
        return NULL;
    }
    int64_t *ids = mv_xmalloc((size_t)n * sizeof(int64_t));
    for (int64_t i = 0; i < n; i++) ids[i] = i;
    MvVecIndex *idx = mv_vidx_build(base, n, t->dim, ids);
    free(base);
    free(ids);
    if (!idx) return NULL;
    mv_vidx_build_graph(idx);
    t->vindex = idx;
    t->vindex_stamp = t->vec_slots;
    return idx;
}

/* -------------------------------------------------- executor-facing hooks */
const MvSchema *mv_table_schema(const MvTable *t) { return &t->schema; }
int32_t mv_table_dim(const MvTable *t) { return t->dim; }
int mv_table_nvals(const MvTable *t) { return t->nvals; }
int mv_table_has_index(const MvTable *t) { return t->has_index ? 1 : 0; }
int64_t mv_table_row_count(const MvTable *t) { return (int64_t)t->pk2slot.count; }
int64_t mv_table_slot_of_pk(const MvTable *t, int64_t pk) {
    int64_t slot = -1;
    if (mv_map_get(&t->pk2slot, pk, &slot) != MV_OK) return -1;
    return slot;
}
int64_t mv_table_pk_of_slot(const MvTable *t, int32_t slot) {
    if (slot < 0 || (size_t)slot >= t->slot_cap) return -1;
    return t->slot_pk[slot];
}
int mv_table_slot_alive(const MvTable *t, int32_t slot) {
    return table_alive(t, slot);
}
int64_t mv_table_next_slot(const MvTable *t) {
    return t->dim > 0 ? t->vec_slots : t->row_slots;
}

void mv_store_set_txn(MvDb *db, uint64_t txn_id) { db->txn_id = txn_id; }
uint64_t mv_store_next_txn(MvDb *db) { return db->next_txn++; }
int mv_store_undo_mark(const MvDb *db) { return db->undo_n; }
void mv_store_undo_to(MvDb *db, int mark) { db_undo_to(db, mark); }
void mv_store_clear_undo(MvDb *db) { db->undo_n = 0; }
int mv_store_commit_current(MvDb *db, MvError *err) { return db_commit_current(db, err); }
void mv_store_mark_dirty(MvDb *db, MvTable *t) { db_mark_dirty(db, (int)(t - db->tables)); }
void mv_store_invalidate_index(MvTable *t) {
    if (t->vindex) mv_vidx_free(t->vindex);
    t->vindex = NULL;
    t->vindex_stamp = -1;
}
int64_t mv_store_row_count(const MvTable *t) { return t->pk2slot.count; }
int mv_store_table_index(const MvDb *db, const MvTable *t) { return (int)(t - db->tables); }

/* MiniVectorDB - shared declarations.
 *
 * A single-process, single-client vector database that speaks the JSON-Lines
 * stdin/stdout protocol described in STDIO_PROTOCOL.md (2026-competition-round2).
 *
 * Storage layout under the data directory (VECTORDB_DATA_DIR, default /data):
 *   catalog.dat      text, rewritten atomically on DDL
 *   commit.log       append-only, one record per committed transaction
 *   <table>.rows     append-only row versions
 *   <table>.vec      dense float32 vector slots, slot i at offset i*dim*4
 *
 * A transaction appends row/vector records tagged with a transaction id and
 * becomes durable only when its id is appended to commit.log and that file is
 * fsynced. Recovery replays only records whose transaction id appears in
 * commit.log, so an uncommitted transaction can never survive a crash.
 */
#ifndef MINIVDB_DB_H
#define MINIVDB_DB_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#define MV_OK 0
#define MV_ERR (-1)

/* ------------------------------------------------------------------ errors */
#define MV_E_UNSUPPORTED "unsupported_sql"
#define MV_E_SYNTAX "syntax_error"
#define MV_E_DUP_KEY "duplicate_key"
#define MV_E_DIM "dimension_mismatch"
#define MV_E_TABLE "table_not_found"
#define MV_E_COLUMN "column_not_found"
#define MV_E_DUP_TABLE "duplicate_table"
#define MV_E_BAD_VECTOR "invalid_vector"
#define MV_E_TOO_LONG "value_too_long"
#define MV_E_RANGE "value_out_of_range"
#define MV_E_COLCOUNT "column_count_mismatch"
#define MV_E_NO_TXN "no_active_transaction"
#define MV_E_DUP_INDEX "index_already_exists"
#define MV_E_INDEX "index_not_found"
#define MV_E_IO "io_error"
#define MV_E_INTERNAL "internal_error"

typedef struct {
    char code[64];
    char message[256];
} MvError;

void mv_err_set(MvError *err, const char *code, const char *fmt, ...);
void mv_err_clear(MvError *err);

/* --------------------------------------------------------------- os layer */
int mv_os_open(const char *path, int create);
int mv_os_close(int fd);
int64_t mv_os_size(int fd);
int64_t mv_os_read(int fd, void *buf, int64_t len, int64_t off);
int64_t mv_os_write(int fd, const void *buf, int64_t len, int64_t off);
int mv_os_fsync(int fd);
int mv_os_truncate(int fd, int64_t len);
int mv_os_rename(const char *from, const char *to);
int mv_os_unlink(const char *path);
int mv_os_mkdir(const char *path);
void mv_os_sync_dir(const char *path);
int mv_os_exists(const char *path);

/* -------------------------------------------------------------- utilities */
void *mv_xmalloc(size_t n);
void *mv_xrealloc(void *p, size_t n);
char *mv_strdup(const char *s);
char *mv_strndup(const char *s, size_t n);
uint32_t mv_crc32(const void *data, size_t len, uint32_t seed);

/* Growable byte buffer. */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} MvBuf;

void mv_buf_init(MvBuf *b);
void mv_buf_free(MvBuf *b);
void mv_buf_append(MvBuf *b, const void *data, size_t len);
void mv_buf_printf(MvBuf *b, const char *fmt, ...);

/* int64 -> slot hash map (open addressing, linear probing). */
typedef struct {
    int64_t *keys;
    int64_t *vals;
    uint8_t *state; /* 0 empty, 1 live, 2 tombstone */
    size_t cap;
    size_t count;
} MvMap;

void mv_map_init(MvMap *m);
void mv_map_free(MvMap *m);
int mv_map_get(const MvMap *m, int64_t key, int64_t *out);
int mv_map_put(MvMap *m, int64_t key, int64_t val); /* MV_ERR if key exists */
int mv_map_del(MvMap *m, int64_t key);

/* ------------------------------------------------------------------- JSON */
typedef enum {
    MV_JSON_NULL,
    MV_JSON_BOOL,
    MV_JSON_INT,
    MV_JSON_DOUBLE,
    MV_JSON_STRING,
    MV_JSON_ARRAY,
    MV_JSON_OBJECT
} MvJsonType;

typedef struct MvJson MvJson;

struct MvJson {
    MvJsonType type;
    int bval;
    int64_t ival;
    double dval;
    char *sval;
    size_t slen;
    MvJson **items;
    char **keys;
    size_t count;
};

MvJson *mv_json_parse(const char *text, size_t len, char *errbuf, size_t errlen);
void mv_json_free(MvJson *j);
const MvJson *mv_json_get(const MvJson *obj, const char *key);
void mv_json_escape(MvBuf *out, const char *s, size_t len);
void mv_json_double(MvBuf *out, double v);

/* ------------------------------------------------------------------- SQL */
typedef enum { MV_COL_BIGINT = 1, MV_COL_VARCHAR = 2, MV_COL_VECTOR = 3 } MvColType;

typedef struct {
    char name[64];
    MvColType type;
    int32_t length; /* VARCHAR: max characters */
    int32_t dim;    /* VECTOR: dimensions */
    uint8_t is_pk;
} MvColumn;

typedef struct {
    char name[64];
    int32_t ncols;
    MvColumn cols[32];
    int32_t pk_idx;
    int32_t vec_idx;
} MvSchema;

typedef enum { MV_VAL_INT, MV_VAL_STR, MV_VAL_VEC, MV_VAL_DOUBLE } MvValType;

typedef struct {
    MvValType type;
    int64_t i;
    double d;
    char *s; /* owned */
    size_t slen;
    float *vec; /* owned */
    int32_t dim;
} MvValue;

void mv_value_free(MvValue *v);
void mv_value_copy(MvValue *dst, const MvValue *src);

typedef enum {
    MV_ST_CREATE_TABLE = 1,
    MV_ST_CREATE_INDEX,
    MV_ST_INSERT,
    MV_ST_SELECT,
    MV_ST_UPDATE,
    MV_ST_DELETE,
    MV_ST_BEGIN,
    MV_ST_COMMIT,
    MV_ST_ROLLBACK
} MvStmtType;

typedef enum { MV_OP_EQ, MV_OP_NE, MV_OP_LT, MV_OP_LE, MV_OP_GT, MV_OP_GE } MvCmpOp;

/* Names are kept textual in the AST and resolved against the schema when the
 * statement executes. */
typedef struct {
    char col_name[64];
    char col2_name[64];
    int32_t col;
    int32_t col2;
    MvCmpOp op;
    MvValue val;
} MvCond;

typedef struct {
    char col_name[64];
    int32_t col;
    uint8_t is_dist;
    MvValue vec;
    char alias[64];
    uint8_t has_alias;
} MvProj;

typedef struct {
    char col_name[64];
    int32_t col;
    uint8_t is_dist;
    MvValue vec;
    char alias[64];
    uint8_t has_alias;
    uint8_t desc;
} MvOrder;

typedef struct {
    char col_name[64];
    int32_t col;
    MvValue val;
} MvAssign;

typedef struct {
    MvStmtType type;
    MvSchema schema; /* CREATE TABLE */
    char index_name[64];
    char table[64];
    char *icols;      /* INSERT column list: nicols fixed-size names */
    int32_t nicols;
    MvValue *vals;   /* flattened row-major values */
    int32_t nvals;
    int32_t nrows;
    int32_t rowwidth;
    MvProj *projs;
    int32_t nprojs;
    uint8_t star;
    MvCond *conds;
    int32_t nconds;
    MvOrder *orders;
    int32_t norders;
    int64_t limit;
    uint8_t has_limit;
    MvAssign *assigns;
    int32_t nassigns;
} MvStmt;

int mv_sql_parse(const char *sql, MvStmt *out, MvError *err);
void mv_stmt_free(MvStmt *st);

/* ------------------------------------------------------------------ store */
typedef struct MvTable MvTable;
typedef struct MvDb MvDb;

typedef struct {
    int64_t pk;
    int32_t slot;
    int32_t nvals;
    MvValue *vals; /* scalars of every column except the vector column */
} MvRow;

typedef struct {
    int is_query;
    int64_t affected_rows;
    int ncols;
    char **colnames;
    MvValue **rows;
    int nrows;
    int rowcap;
} MvResult;

void mv_result_init(MvResult *r);
void mv_result_free(MvResult *r);
int mv_result_add_row(MvResult *r, int ncols);

MvDb *mv_db_open(const char *dir, MvError *err);
void mv_db_close(MvDb *db);
const char *mv_db_dir(const MvDb *db);
MvTable *mv_db_find_table(MvDb *db, const char *name);
int mv_db_table_count(const MvDb *db);

int mv_create_table(MvDb *db, const MvSchema *schema, MvError *err);
int mv_create_vector_index(MvDb *db, const char *index_name, const char *table,
                           const char *column, MvError *err);

/* Row iteration over the live rows of a table. */
typedef int (*MvRowFn)(void *ctx, const MvRow *row, int64_t pk);
int mv_scan_rows(MvDb *db, MvTable *t, MvRowFn fn, void *ctx);

/* Vector slot iteration: fn receives the slot, its primary key and pointer. */
typedef int (*MvSlotFn)(void *ctx, int32_t slot, int64_t pk, const float *vec);
int mv_scan_vectors(MvDb *db, MvTable *t, MvSlotFn fn, void *ctx);

int mv_get_vector(MvDb *db, MvTable *t, int32_t slot, float *out);
int mv_row_values(MvDb *db, MvTable *t, int32_t slot, MvValue *out);

/* Table introspection used by the executor. */
const MvSchema *mv_table_schema(const MvTable *t);
int32_t mv_table_dim(const MvTable *t);
int mv_table_nvals(const MvTable *t);
int mv_table_has_index(const MvTable *t);
int64_t mv_table_row_count(const MvTable *t);
int64_t mv_table_slot_of_pk(const MvTable *t, int64_t pk);
int64_t mv_table_pk_of_slot(const MvTable *t, int32_t slot);
int mv_table_slot_alive(const MvTable *t, int32_t slot);
int64_t mv_table_next_slot(const MvTable *t);

/* Mutation primitives. The caller validates values before calling these. */
int mv_store_insert(MvDb *db, MvTable *t, int64_t pk, const MvValue *scalars, int nvals,
                    const float *vec, MvError *err);
int mv_store_update(MvDb *db, MvTable *t, int64_t pk, int32_t old_slot, const MvValue *scalars,
                    int nvals, const float *vec, MvError *err);
int mv_store_delete(MvDb *db, MvTable *t, int64_t pk, int32_t old_slot, MvError *err);

/* Approximate index of a table (built lazily from the vector file). */
struct MvVecIndex;
struct MvVecIndex *mv_store_ensure_index(MvDb *db, MvTable *t);

/* Transaction plumbing used by the executor. */
void mv_store_set_txn(MvDb *db, uint64_t txn_id);
uint64_t mv_store_next_txn(MvDb *db);
int mv_store_undo_mark(const MvDb *db);
void mv_store_undo_to(MvDb *db, int mark);
void mv_store_clear_undo(MvDb *db);
int mv_store_commit_current(MvDb *db, MvError *err);
void mv_store_mark_dirty(MvDb *db, MvTable *t);
void mv_store_invalidate_index(MvTable *t);

/* -------------------------------------------------------------- executor */
int mv_exec(MvDb *db, const MvStmt *st, const char *search_mode, MvResult *res,
            MvError *err);

/* ------------------------------------------------------------ transaction */
int mv_txn_begin(MvDb *db, MvError *err);
int mv_txn_commit(MvDb *db, MvError *err);
int mv_txn_rollback(MvDb *db, MvError *err);
int mv_txn_active(const MvDb *db);

/* ----------------------------------------------------------- vector math */
double mv_l2_distance(const float *a, const float *b, int32_t dim);
int mv_parse_vector(const char *text, size_t len, int32_t dim, float **out,
                    int32_t *out_dim, MvError *err);
int mv_utf8_char_count(const char *s, size_t len);

#endif /* MINIVDB_DB_H */

/* Small helpers: allocation, growable buffer, CRC32, int64 hash map. */
#include "db.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

void mv_err_set(MvError *err, const char *code, const char *fmt, ...) {
    if (!err) return;
    snprintf(err->code, sizeof(err->code), "%s", code);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err->message, sizeof(err->message), fmt, ap);
    va_end(ap);
}

void mv_err_clear(MvError *err) {
    if (!err) return;
    err->code[0] = '\0';
    err->message[0] = '\0';
}

void *mv_xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "minivectordb: out of memory\n");
        exit(70);
    }
    return p;
}

void *mv_xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        fprintf(stderr, "minivectordb: out of memory\n");
        exit(70);
    }
    return q;
}

char *mv_strdup(const char *s) {
    size_t n = strlen(s);
    char *p = mv_xmalloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

char *mv_strndup(const char *s, size_t n) {
    char *p = mv_xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

uint32_t mv_crc32(const void *data, size_t len, uint32_t seed) {
    static uint32_t table[256];
    static int ready = 0;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        ready = 1;
    }
    uint32_t crc = seed ^ 0xFFFFFFFFu;
    const unsigned char *p = (const unsigned char *)data;
    for (size_t i = 0; i < len; i++) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

/* ------------------------------------------------------------- MvBuf */
void mv_buf_init(MvBuf *b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void mv_buf_free(MvBuf *b) {
    free(b->data);
    mv_buf_init(b);
}

void mv_buf_append(MvBuf *b, const void *data, size_t len) {
    if (b->len + len + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 64;
        while (cap < b->len + len + 1) cap *= 2;
        b->data = mv_xrealloc(b->data, cap);
        b->cap = cap;
    }
    memcpy(b->data + b->len, data, len);
    b->len += len;
    b->data[b->len] = '\0';
}

void mv_buf_printf(MvBuf *b, const char *fmt, ...) {
    char stack[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(stack, sizeof(stack), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof(stack)) {
        mv_buf_append(b, stack, (size_t)n);
        return;
    }
    char *heap = mv_xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(heap, (size_t)n + 1, fmt, ap);
    va_end(ap);
    mv_buf_append(b, heap, (size_t)n);
    free(heap);
}

/* ------------------------------------------------------------- MvMap */
static uint64_t mv_mix64(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

void mv_map_init(MvMap *m) {
    m->cap = 16;
    m->count = 0;
    m->keys = mv_xmalloc(m->cap * sizeof(int64_t));
    m->vals = mv_xmalloc(m->cap * sizeof(int64_t));
    m->state = mv_xmalloc(m->cap);
    memset(m->state, 0, m->cap);
}

void mv_map_free(MvMap *m) {
    free(m->keys);
    free(m->vals);
    free(m->state);
    m->keys = NULL;
    m->vals = NULL;
    m->state = NULL;
    m->cap = m->count = 0;
}

static void mv_map_grow(MvMap *m) {
    size_t ncap = m->cap * 2;
    int64_t *nkeys = mv_xmalloc(ncap * sizeof(int64_t));
    int64_t *nvals = mv_xmalloc(ncap * sizeof(int64_t));
    uint8_t *nstate = mv_xmalloc(ncap);
    memset(nstate, 0, ncap);
    size_t mask = ncap - 1;
    for (size_t i = 0; i < m->cap; i++) {
        if (m->state[i] != 1) continue;
        size_t j = (size_t)mv_mix64((uint64_t)m->keys[i]) & mask;
        while (nstate[j] == 1) j = (j + 1) & mask;
        nstate[j] = 1;
        nkeys[j] = m->keys[i];
        nvals[j] = m->vals[i];
    }
    free(m->keys);
    free(m->vals);
    free(m->state);
    m->keys = nkeys;
    m->vals = nvals;
    m->state = nstate;
    m->cap = ncap;
}

int mv_map_get(const MvMap *m, int64_t key, int64_t *out) {
    if (!m->cap) return MV_ERR;
    size_t mask = m->cap - 1;
    size_t i = (size_t)mv_mix64((uint64_t)key) & mask;
    size_t probes = 0;
    /* probing must continue past tombstones: a deleted slot may still sit
     * between the hash position and a live entry */
    while (probes < m->cap) {
        if (m->state[i] == 0) return MV_ERR; /* empty slot ends the chain */
        if (m->state[i] == 1 && m->keys[i] == key) {
            if (out) *out = m->vals[i];
            return MV_OK;
        }
        i = (i + 1) & mask;
        probes++;
    }
    return MV_ERR;
}

int mv_map_put(MvMap *m, int64_t key, int64_t val) {
    if ((m->count + 1) * 10 >= m->cap * 7) mv_map_grow(m);
    size_t mask = m->cap - 1;
    size_t i = (size_t)mv_mix64((uint64_t)key) & mask;
    size_t probes = 0;
    size_t tomb = (size_t)-1;
    while (probes < m->cap) {
        if (m->state[i] == 0) break;
        if (m->state[i] == 2 && tomb == (size_t)-1) tomb = i;
        if (m->state[i] == 1 && m->keys[i] == key) return MV_ERR;
        i = (i + 1) & mask;
        probes++;
    }
    if (tomb != (size_t)-1) i = tomb; /* reuse the tombstone */
    m->state[i] = 1;
    m->keys[i] = key;
    m->vals[i] = val;
    m->count++;
    return MV_OK;
}

int mv_map_del(MvMap *m, int64_t key) {
    if (!m->cap) return MV_ERR;
    size_t mask = m->cap - 1;
    size_t i = (size_t)mv_mix64((uint64_t)key) & mask;
    size_t probes = 0;
    while (probes < m->cap) {
        if (m->state[i] == 0) return MV_ERR;
        if (m->state[i] == 1 && m->keys[i] == key) {
            m->state[i] = 2;
            m->count--;
            return MV_OK;
        }
        i = (i + 1) & mask;
        probes++;
    }
    return MV_ERR;
}

/* ------------------------------------------------------------ MvValue */
void mv_value_free(MvValue *v) {
    if (!v) return;
    free(v->s);
    free(v->vec);
    v->s = NULL;
    v->vec = NULL;
    v->type = MV_VAL_INT;
    v->slen = 0;
    v->dim = 0;
    v->i = 0;
}

void mv_value_copy(MvValue *dst, const MvValue *src) {
    dst->type = src->type;
    dst->i = src->i;
    dst->slen = src->slen;
    dst->dim = src->dim;
    dst->s = src->s ? mv_strndup(src->s, src->slen) : NULL;
    dst->vec = NULL;
    if (src->vec && src->dim > 0) {
        dst->vec = mv_xmalloc((size_t)src->dim * sizeof(float));
        memcpy(dst->vec, src->vec, (size_t)src->dim * sizeof(float));
    }
}

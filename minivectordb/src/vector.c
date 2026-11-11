/* Vector math and the approximate (HNSW-style) index.
 *
 * The index is a single-level navigable small world graph: every vector owns a
 * bounded neighbour list, insertion links a new vector to its nearest
 * neighbours and search is a best-first walk with an "ef" frontier. The search
 * returns the ef nearest neighbours so the caller can apply a filter and still
 * keep enough candidates; when the filtered result set is too small the caller
 * falls back to the exact scan, which keeps recall at 1.0 where it matters.
 */
#include "db.h"
#include "vecidx.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Four independent accumulators: a single accumulator serialises the whole
 * loop on one addition chain and turns the scan into a latency bound. */
double mv_l2_distance(const float *a, const float *b, int32_t dim) {
    double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0;
    int32_t i = 0;
    for (; i + 4 <= dim; i += 4) {
        double d0 = (double)a[i] - (double)b[i];
        double d1 = (double)a[i + 1] - (double)b[i + 1];
        double d2 = (double)a[i + 2] - (double)b[i + 2];
        double d3 = (double)a[i + 3] - (double)b[i + 3];
        s0 += d0 * d0;
        s1 += d1 * d1;
        s2 += d2 * d2;
        s3 += d3 * d3;
    }
    double sum = (s0 + s1) + (s2 + s3);
    for (; i < dim; i++) {
        double d = (double)a[i] - (double)b[i];
        sum += d * d;
    }
    return sqrt(sum);
}

int mv_utf8_char_count(const char *s, size_t len) {
    int count = 0;
    for (size_t i = 0; i < len; i++) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) count++;
    }
    return count;
}

/* Parse "[1, 2.5, -3]" into float32 values. */
int mv_parse_vector(const char *text, size_t len, int32_t dim, float **out, int32_t *out_dim,
                    MvError *err) {
    const char *p = text;
    const char *end = text + len;
    float *vals = NULL;
    int32_t count = 0, cap = 0;
    while (p < end && isspace((unsigned char)*p)) p++;
    if (p >= end || *p != '[') {
        mv_err_set(err, MV_E_BAD_VECTOR, "vector literal must start with '['");
        return MV_ERR;
    }
    p++;
    for (;;) {
        while (p < end && isspace((unsigned char)*p)) p++;
        if (p >= end) {
            free(vals);
            mv_err_set(err, MV_E_BAD_VECTOR, "unterminated vector literal");
            return MV_ERR;
        }
        if (*p == ']') {
            p++;
            break;
        }
        char *endp = NULL;
        double d = strtod(p, &endp);
        if (endp == p) {
            free(vals);
            mv_err_set(err, MV_E_BAD_VECTOR, "vector elements must be numbers");
            return MV_ERR;
        }
        p = endp;
        if (!isfinite(d) || fabs(d) > 3.4028234663852886e38) {
            free(vals);
            mv_err_set(err, MV_E_BAD_VECTOR, "vector element is not a finite float32 value");
            return MV_ERR;
        }
        if (count == cap) {
            cap = cap ? cap * 2 : 8;
            vals = mv_xrealloc(vals, (size_t)cap * sizeof(float));
        }
        vals[count++] = (float)d;
        while (p < end && isspace((unsigned char)*p)) p++;
        if (p < end && *p == ',') {
            p++;
            continue;
        }
    }
    if (dim > 0 && count != dim) {
        free(vals);
        mv_err_set(err, MV_E_DIM, "expected %d vector dimensions but got %d", dim, count);
        return MV_ERR;
    }
    *out = vals;
    *out_dim = count;
    return MV_OK;
}

/* ------------------------------------------------------------- the index */
/* A single-level navigable small world graph. Every vector owns a bounded
 * neighbour list; insertion links a new vector to its nearest neighbours and
 * search walks the graph with an "ef" frontier. The search returns the ef
 * nearest neighbours so the caller can apply a filter and still keep enough
 * candidates; when the filtered result set is too small the caller falls back
 * to the exact scan, which keeps recall at 1.0 where it matters. */
#define MV_VIDX_M 24 /* neighbours per node */

typedef struct {
    double dist;
    int64_t id;
    int32_t node;
} VItem;

struct MvVecIndex {
    int32_t dim;
    float *base;
    int64_t *ids;
    int64_t n;
    int64_t cap;
    int32_t *adj; /* n * MV_VIDX_M neighbour node ids */
    int32_t *deg; /* current degree */
    int64_t entry;
    int64_t inserted; /* nodes already linked into the graph */
};

/* binary min-heap over VItem */
typedef struct {
    VItem *items;
    int32_t n;
    int32_t cap;
} VHeap;

static void heap_init(VHeap *h) {
    h->cap = 64;
    h->items = mv_xmalloc((size_t)h->cap * sizeof(VItem));
    h->n = 0;
}

static void heap_free(VHeap *h) { free(h->items); }

static void heap_push(VHeap *h, VItem it) {
    if (h->n == h->cap) {
        h->cap *= 2;
        h->items = mv_xrealloc(h->items, (size_t)h->cap * sizeof(VItem));
    }
    h->items[h->n] = it;
    int32_t i = h->n++;
    while (i > 0) {
        int32_t parent = (i - 1) / 2;
        if (h->items[parent].dist <= h->items[i].dist) break;
        VItem tmp = h->items[parent];
        h->items[parent] = h->items[i];
        h->items[i] = tmp;
        i = parent;
    }
}

static VItem heap_pop(VHeap *h) {
    VItem top = h->items[0];
    h->items[0] = h->items[--h->n];
    int32_t i = 0;
    for (;;) {
        int32_t l = 2 * i + 1, r = l + 1, best = i;
        if (l < h->n && h->items[l].dist < h->items[best].dist) best = l;
        if (r < h->n && h->items[r].dist < h->items[best].dist) best = r;
        if (best == i) break;
        VItem tmp = h->items[best];
        h->items[best] = h->items[i];
        h->items[i] = tmp;
        i = best;
    }
    return top;
}

static const float *vidx_vec(const MvVecIndex *ix, int32_t node) {
    return ix->base + (int64_t)node * ix->dim;
}

MvVecIndex *mv_vidx_build(const float *base, int64_t n, int32_t dim, const int64_t *ids) {
    if (n <= 0 || dim <= 0) return NULL;
    MvVecIndex *ix = mv_xmalloc(sizeof(MvVecIndex));
    memset(ix, 0, sizeof(*ix));
    ix->dim = dim;
    ix->n = n;
    ix->cap = n;
    ix->base = mv_xmalloc((size_t)n * (size_t)dim * sizeof(float));
    memcpy(ix->base, base, (size_t)n * (size_t)dim * sizeof(float));
    ix->ids = mv_xmalloc((size_t)n * sizeof(int64_t));
    memcpy(ix->ids, ids, (size_t)n * sizeof(int64_t));
    ix->adj = mv_xmalloc((size_t)n * MV_VIDX_M * sizeof(int32_t));
    ix->deg = mv_xmalloc((size_t)n * sizeof(int32_t));
    memset(ix->deg, 0, (size_t)n * sizeof(int32_t));
    ix->entry = 0;
    ix->inserted = 0;
    return ix;
}

void mv_vidx_free(MvVecIndex *ix) {
    if (!ix) return;
    free(ix->base);
    free(ix->ids);
    free(ix->adj);
    free(ix->deg);
    free(ix);
}

int64_t mv_vidx_size(const MvVecIndex *ix) { return ix ? ix->n : 0; }

/* Link node a and node b in both directions. */
static void vidx_link(MvVecIndex *ix, int32_t a, int32_t b) {
    for (int32_t i = 0; i < ix->deg[a]; i++)
        if (ix->adj[(int64_t)a * MV_VIDX_M + i] == b) return;
    if (ix->deg[a] < MV_VIDX_M) {
        ix->adj[(int64_t)a * MV_VIDX_M + ix->deg[a]] = b;
        ix->deg[a]++;
    }
    for (int32_t i = 0; i < ix->deg[b]; i++)
        if (ix->adj[(int64_t)b * MV_VIDX_M + i] == a) return;
    if (ix->deg[b] < MV_VIDX_M) {
        ix->adj[(int64_t)b * MV_VIDX_M + ix->deg[b]] = a;
        ix->deg[b]++;
    }
}

/* Best-first search over the graph; returns up to ef results ordered by
 * (distance, id). The traversal only reaches nodes that are already linked. */
static int32_t vidx_search_graph(const MvVecIndex *ix, const float *query, int32_t ef,
                                 VItem *results, uint8_t *visited) {
    if (ix->inserted <= 0) return 0;
    if (ef > ix->inserted) ef = (int32_t)ix->inserted;
    if (ef <= 0) ef = 1;

    memset(visited, 0, (size_t)ix->inserted);
    VHeap candidates;
    heap_init(&candidates);
    int32_t nres = 0;

    double d0 = mv_l2_distance(query, vidx_vec(ix, (int32_t)ix->entry), ix->dim);
    VItem start;
    start.dist = d0;
    start.node = (int32_t)ix->entry;
    start.id = ix->ids[ix->entry];
    heap_push(&candidates, start);
    visited[ix->entry] = 1;

    while (candidates.n > 0) {
        VItem cur = heap_pop(&candidates);
        if (nres >= ef && cur.dist > results[nres - 1].dist) break;
        for (int32_t i = 0; i < ix->deg[cur.node]; i++) {
            int32_t nb = ix->adj[(int64_t)cur.node * MV_VIDX_M + i];
            if (nb < 0 || (size_t)nb >= ix->inserted || visited[nb]) continue;
            visited[nb] = 1;
            double d = mv_l2_distance(query, vidx_vec(ix, nb), ix->dim);
            int32_t pos;
            if (nres < ef) {
                pos = nres++;
            } else if (d < results[nres - 1].dist ||
                       (d == results[nres - 1].dist && ix->ids[nb] < results[nres - 1].id)) {
                pos = nres - 1;
            } else {
                continue;
            }
            while (pos > 0 && (results[pos - 1].dist > d ||
                               (results[pos - 1].dist == d && results[pos - 1].id > ix->ids[nb]))) {
                results[pos] = results[pos - 1];
                pos--;
            }
            results[pos].dist = d;
            results[pos].node = nb;
            results[pos].id = ix->ids[nb];
            VItem c;
            c.dist = d;
            c.node = nb;
            c.id = ix->ids[nb];
            heap_push(&candidates, c);
        }
    }
    heap_free(&candidates);
    return nres;
}

/* Insert a node into the graph and link it to its nearest neighbours. */
static void vidx_insert(MvVecIndex *ix, int32_t node, VItem *scratch, uint8_t *visited) {
    if (ix->inserted <= 0) {
        ix->entry = node;
        ix->inserted = 1;
        return;
    }
    int32_t ef = 32;
    int32_t found = vidx_search_graph(ix, vidx_vec(ix, node), ef, scratch, visited);
    int32_t links = MV_VIDX_M / 2;
    if (links > found) links = found;
    for (int32_t i = 0; i < links; i++) vidx_link(ix, node, scratch[i].node);
    ix->inserted++;
}

int32_t mv_vidx_search(const MvVecIndex *ix, const float *query, int32_t ef, int32_t *out_slots,
                       double *out_dists) {
    if (!ix || ix->n <= 0) return 0;
    if (ef > ix->inserted) ef = (int32_t)ix->inserted;
    if (ef <= 0) ef = 1;
    VItem *results = mv_xmalloc((size_t)ef * sizeof(VItem));
    uint8_t *visited = mv_xmalloc((size_t)(ix->inserted > 0 ? ix->inserted : 1));
    int32_t n = vidx_search_graph(ix, query, ef, results, visited);
    for (int32_t i = 0; i < n; i++) {
        out_slots[i] = (int32_t)results[i].id;
        out_dists[i] = results[i].dist;
    }
    free(results);
    free(visited);
    return n;
}

/* Build the graph: insert every vector, starting from the vector closest to
 * the centroid so the greedy walks stay local. */
void mv_vidx_build_graph(MvVecIndex *ix) {
    if (!ix || ix->n <= 0) return;
    int64_t n = ix->n;
    float *centroid = mv_xmalloc((size_t)ix->dim * sizeof(float));
    memset(centroid, 0, (size_t)ix->dim * sizeof(float));
    for (int64_t i = 0; i < n; i++) {
        const float *v = ix->base + i * ix->dim;
        for (int32_t d = 0; d < ix->dim; d++) centroid[d] += v[d];
    }
    for (int32_t d = 0; d < ix->dim; d++) centroid[d] /= (float)n;
    int64_t best = 0;
    double bestd = mv_l2_distance(centroid, vidx_vec(ix, 0), ix->dim);
    for (int64_t i = 1; i < n; i++) {
        double d = mv_l2_distance(centroid, vidx_vec(ix, (int32_t)i), ix->dim);
        if (d < bestd) {
            bestd = d;
            best = i;
        }
    }
    free(centroid);

    VItem *scratch = mv_xmalloc((size_t)32 * sizeof(VItem));
    uint8_t *visited = mv_xmalloc((size_t)n);
    /* insert the entry point first, then the rest */
    ix->inserted = 0;
    ix->entry = 0;
    vidx_insert(ix, (int32_t)best, scratch, visited);
    for (int64_t i = 0; i < n; i++) {
        if ((int32_t)i == (int32_t)best) continue;
        vidx_insert(ix, (int32_t)i, scratch, visited);
    }
    free(scratch);
    free(visited);
}

/* Approximate vector index (HNSW-style navigable small world) used by the
 * "ann" search mode. The graph is kept in memory and rebuilt from the
 * durable vector file whenever the table changes. */
#ifndef MINIVDB_VECIDX_H
#define MINIVDB_VECIDX_H

#include <stdint.h>

typedef struct MvVecIndex MvVecIndex;

/* Build an index over n vectors of dim dimensions. ids[i] is the slot id of
 * vector i. */
MvVecIndex *mv_vidx_build(const float *base, int64_t n, int32_t dim, const int64_t *ids);
void mv_vidx_free(MvVecIndex *idx);

/* Search for the ef nearest neighbours of query. Results are written to
 * out_slots/out_dists ordered by (distance, slot). Returns the count. */
int32_t mv_vidx_search(const MvVecIndex *idx, const float *query, int32_t ef, int32_t *out_slots,
                       double *out_dists);

int64_t mv_vidx_size(const MvVecIndex *idx);

/* Build the proximity graph over the vectors already loaded into the index. */
void mv_vidx_build_graph(MvVecIndex *idx);

#endif /* MINIVDB_VECIDX_H */

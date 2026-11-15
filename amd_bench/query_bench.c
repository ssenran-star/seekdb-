/* Per-query latency benchmark: build once, then issue NQ single-vector
 * queries (nq=1, matching the contest's per-probe path) and report the
 * average per-query cost. Used to quantify the search-buffer reuse win,
 * which batch bridge_test cannot see.
 *
 * Usage: ./query_bench [so] [n] [dim] [nq] [topk]
 * Env: BASE_F32 / QUERY_F32
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dlfcn.h>

static double now_ms(void) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

int main(int argc, char **argv) {
  const char *so = argc > 1 ? argv[1] : "/tmp/bridge/libseekdb_cuvs_bridge.so";
  int n = argc > 2 ? atoi(argv[2]) : 250000;
  int dim = argc > 3 ? atoi(argv[3]) : 256;
  int total = argc > 4 ? atoi(argv[4]) : 3000;   /* total single queries to issue */
  int topk = argc > 5 ? atoi(argv[5]) : 10;
  int nq = 100;  /* d256 query.f32 holds 100 probe vectors (queried modulo nq) */
  const char *base_path = getenv("BASE_F32");
  const char *qry_path = getenv("QUERY_F32");
  if (!base_path) base_path = "/workspace/work/d256/base.f32";
  if (!qry_path) qry_path = "/workspace/work/d256/query.f32";

  void *h = dlopen(so, RTLD_NOW);
  if (!h) { fprintf(stderr, "DLOPEN_FAIL %s\n", dlerror()); return 2; }
  void *(*build)(const float *, long, long) = dlsym(h, "seekdb_cuvs_build");
  int (*search)(void *, const float *, long, long, unsigned *, float *) = dlsym(h, "seekdb_cuvs_search");
  void (*fre)(void *) = dlsym(h, "seekdb_cuvs_free");
  if (!build || !search || !fre) { fprintf(stderr, "DLSYM_FAIL\n"); return 3; }

  float *base = (float *)malloc((size_t)n * dim * 4);
  float *qry = (float *)malloc((size_t)nq * dim * 4);
  unsigned *ids = (unsigned *)malloc((size_t)topk * 4);
  if (!base || !qry || !ids) { fprintf(stderr, "OOM\n"); return 4; }

  FILE *f = fopen(base_path, "rb");
  if (!f) { fprintf(stderr, "NO_BASE\n"); return 5; }
  fread(base, 4, (size_t)n * dim, f); fclose(f);
  f = fopen(qry_path, "rb");
  if (!f) { fprintf(stderr, "NO_QUERY\n"); return 6; }
  fread(qry, 4, (size_t)nq * dim, f); fclose(f);

  void *idx = build(base, n, dim);
  if (!idx) { fprintf(stderr, "BUILD_FAIL\n"); return 9; }

  /* warm up (first call pays cuVS context / lazy init) */
  search(idx, qry, 1, topk, ids, NULL);

  double t0 = now_ms();
  for (int q = 0; q < total; q++) {
    if (search(idx, qry + (size_t)(q % nq) * dim, 1, topk, ids, NULL) != 0) {
      fprintf(stderr, "SEARCH_FAIL q=%d\n", q); return 11;
    }
  }
  double elapsed = now_ms() - t0;
  printf("SO=%s TOTAL=%d TOPK=%d TOTAL_MS=%.3f AVG_US=%.2f\n",
         so, total, topk, elapsed, elapsed * 1000.0 / total);

  fre(idx);
  free(base); free(qry); free(ids);
  return 0;
}

/* Direct bridge probe: measure CAGRA build time AND recall without SQL.
 *
 * Usage: ./bridge_test [so_path] [n] [dim] [nq] [topk]
 *
 * Env:
 *   BASE_F32 / QUERY_F32 / GT_I32   dataset paths
 *   GT_Q / GT_K                      ground truth shape (default 100 / 10)
 *   SNAP_PATH                        snapshot file for the serialize probe
 *
 * Reports recall@10 and recall@topk separately. Both come from a single
 * topk search: the first 10 entries of the result are the approximate
 * top-10, so
 *   recall@10  = |ANN_top10  n GT_top10|  / 10
 *   recall@100 = |ANN_top100 n GT_top100| / 100
 * The contest gates on BOTH, and on the drop versus the frozen baseline, so
 * both must be measured with GT_K >= 100.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dlfcn.h>

static double now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static int env_int(const char *name, int dflt) {
  const char *e = getenv(name);
  return e ? atoi(e) : dflt;
}

int main(int argc, char **argv) {
  const char *so = argc > 1 ? argv[1] : "/seekdb_workspace/bridge/libseekdb_cuvs_bridge.so";
  int n = argc > 2 ? atoi(argv[2]) : 10000;
  int dim = argc > 3 ? atoi(argv[3]) : 128;
  int nq = argc > 4 ? atoi(argv[4]) : 100;
  int topk = argc > 5 ? atoi(argv[5]) : 100;
  const char *base_path = getenv("BASE_F32");
  const char *qry_path = getenv("QUERY_F32");
  const char *gt_path = getenv("GT_I32");
  int gt_q = env_int("GT_Q", 100);
  int gt_k = env_int("GT_K", 10);
  if (!base_path) base_path = "/seekdb_workspace/datasets/base.f32";
  if (!qry_path) qry_path = "/seekdb_workspace/datasets/query.f32";
  if (!gt_path) gt_path = "/seekdb_workspace/datasets/gt_100x10.i32";

  void *h = dlopen(so, RTLD_NOW);
  if (!h) { fprintf(stderr, "DLOPEN_FAIL %s\n", dlerror()); return 2; }

  void *(*build)(const float *, long, long) = dlsym(h, "seekdb_cuvs_build");
  int (*search)(void *, const float *, long, long, unsigned *, float *) =
      dlsym(h, "seekdb_cuvs_search");
  void (*fre)(void *) = dlsym(h, "seekdb_cuvs_free");
  int (*serialize)(void *, const char *) = dlsym(h, "seekdb_cuvs_serialize");
  void *(*deserialize)(const char *, long, long) = dlsym(h, "seekdb_cuvs_deserialize");
  if (!build || !search || !fre) { fprintf(stderr, "DLSYM_FAIL\n"); return 3; }

  float *base = (float *)malloc((size_t)n * dim * 4);
  float *qry = (float *)malloc((size_t)nq * dim * 4);
  if (!base || !qry) { fprintf(stderr, "OOM\n"); return 4; }

  FILE *f = fopen(base_path, "rb");
  if (!f) { fprintf(stderr, "NO_BASE %s\n", base_path); return 5; }
  if (fread(base, 4, (size_t)n * dim, f) != (size_t)n * dim) { fprintf(stderr, "BASE_SHORT\n"); return 6; }
  fclose(f);

  f = fopen(qry_path, "rb");
  if (!f) { fprintf(stderr, "NO_QUERY %s\n", qry_path); return 7; }
  if (fread(qry, 4, (size_t)nq * dim, f) != (size_t)nq * dim) { fprintf(stderr, "QUERY_SHORT\n"); return 8; }
  fclose(f);

  double t0 = now_ms();
  void *idx = build(base, n, dim);
  double build_ms = now_ms() - t0;
  if (!idx) { fprintf(stderr, "BUILD_FAIL\n"); return 9; }

  unsigned *ids = (unsigned *)malloc((size_t)nq * topk * 4);
  if (!ids) { fprintf(stderr, "OOM2\n"); return 10; }

  t0 = now_ms();
  int rc = search(idx, qry, nq, topk, ids, NULL);
  double search_ms = now_ms() - t0;
  if (rc != 0) { fprintf(stderr, "SEARCH_FAIL rc=%d\n", rc); return 11; }

  int *gt = (int *)malloc((size_t)gt_q * gt_k * 4);
  if (!gt) { fprintf(stderr, "OOM3\n"); return 12; }
  int nread = 0;
  f = fopen(gt_path, "rb");
  if (f) {
    nread = (int)fread(gt, 4, (size_t)gt_q * gt_k, f);
    fclose(f);
  }
  if (nread != gt_q * gt_k) {
    fprintf(stderr, "GT_SHORT read=%d expect=%d\n", nread, gt_q * gt_k);
  }

  /* recall@10 and recall@topk */
  double r10_hit = 0, r10_tot = 0, rk_hit = 0, rk_tot = 0;
  int qmax = (nq < gt_q) ? nq : gt_q;
  int k10 = (gt_k < 10) ? gt_k : 10;
  if (k10 > topk) k10 = topk;
  int kk = (topk < gt_k) ? topk : gt_k;
  for (int q = 0; q < qmax; q++) {
    for (int k = 0; k < k10; k++) {
      unsigned want = (unsigned)gt[(size_t)q * gt_k + k];
      for (int j = 0; j < k10; j++) {
        if (ids[(size_t)q * topk + j] == want) { r10_hit += 1; break; }
      }
    }
    r10_tot += k10;
    for (int k = 0; k < kk; k++) {
      unsigned want = (unsigned)gt[(size_t)q * gt_k + k];
      for (int j = 0; j < kk; j++) {
        if (ids[(size_t)q * topk + j] == want) { rk_hit += 1; break; }
      }
    }
    rk_tot += kk;
  }

  /* Snapshot round-trip: the contest timer includes persisting the CAGRA
   * graph to storage, so measure serialize (and optionally reload) here. */
  double ser_ms = -1.0, deser_ms = -1.0;
  long long snap_bytes = -1;
  if (serialize) {
    const char *snap = getenv("SNAP_PATH");
    if (!snap) snap = "/tmp/seekdb_probe_snapshot.bin";
    t0 = now_ms();
    int src = serialize(idx, snap);
    ser_ms = now_ms() - t0;
    if (src != 0) {
      fprintf(stderr, "SERIALIZE_FAIL rc=%d\n", src);
    } else {
      FILE *sf = fopen(snap, "rb");
      if (sf) { fseek(sf, 0, SEEK_END); snap_bytes = ftell(sf); fclose(sf); }
      if (deserialize) {
        t0 = now_ms();
        void *h2 = deserialize(snap, n, dim);
        deser_ms = now_ms() - t0;
        if (h2) fre(h2);
        else fprintf(stderr, "DESERIALIZE_FAIL\n");
      }
    }
  }

  printf("BUILD_MS=%.1f SEARCH_MS=%.1f SER_MS=%.1f DESER_MS=%.1f SNAP_BYTES=%lld "
         "RECALL10=%.4f RECALLK=%.4f GTK=%d TOPK=%d N=%d DIM=%d NQ=%d\n",
         build_ms, search_ms, ser_ms, deser_ms, snap_bytes,
         r10_tot > 0 ? r10_hit / r10_tot : 0.0,
         rk_tot > 0 ? rk_hit / rk_tot : 0.0,
         gt_k, topk, n, dim, nq);

  fre(idx);
  free(base); free(qry); free(ids); free(gt);
  return 0;
}

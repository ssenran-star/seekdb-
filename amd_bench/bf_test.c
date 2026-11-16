/* bf_test: build a CAGRA graph from an EXACT brute-force kNN graph.
 *
 * Why: probe_api.sh confirmed /opt/hipvs exports cuvsBruteForceBuild/Search
 * and cuvsCagraIndexFromArgs. Exact top-k neighbours should beat NN-Descent
 * quality (no convergence problem) at a fraction of the build time, and the
 * graph is available in host memory -> the snapshot can store it directly.
 *
 * Pipeline:  BF build (attach dataset) -> BF self-search top-k
 *            -> uint32 graph -> cuvsCagraIndexFromArgs -> cuvsCagraSearch
 *
 * Sanity: each row's nearest neighbour must be itself -> SELF_HIT ~1.0.
 * If SELF_HIT is ~0 the neighbours dtype is wrong (try NEIGHBOR_BITS=32).
 *
 * Usage: ./bf_test <n> <dim> [topk]
 * Env:   BASE_F32 / QUERY_F32 / GT_I32 / GT_Q / GT_K / NEIGHBOR_BITS (64|32)
 */
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/brute_force.h>
#include <cuvs/neighbors/cagra.h>
#include <dlpack/dlpack.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

int main(int argc, char **argv) {
  int n = argc > 1 ? atoi(argv[1]) : 250000;
  int dim = argc > 2 ? atoi(argv[2]) : 256;
  int gdeg = argc > 3 ? atoi(argv[3]) : 128;
  int nq = 100, topk = 100;
  int nbits = 64;
  const char *e = getenv("NEIGHBOR_BITS");
  if (e) nbits = atoi(e);

  const char *base_path = getenv("BASE_F32");
  const char *qry_path = getenv("QUERY_F32");
  const char *gt_path = getenv("GT_I32");
  if (!base_path) base_path = "/workspace/work/d256/base.f32";
  if (!qry_path) qry_path = "/workspace/work/d256/query.f32";
  if (!gt_path) gt_path = "/workspace/work/d256/gt.i32";
  int gt_q = getenv("GT_Q") ? atoi(getenv("GT_Q")) : 100;
  int gt_k = getenv("GT_K") ? atoi(getenv("GT_K")) : 100;

  float *base = malloc((size_t)n * dim * 4);
  float *qry = malloc((size_t)nq * dim * 4);
  int *gt = malloc((size_t)nq * gt_k * 4);
  FILE *f = fopen(base_path, "rb"); if (!f) { fprintf(stderr, "NO_BASE\n"); return 2; }
  fread(base, 4, (size_t)n * dim, f); fclose(f);
  f = fopen(qry_path, "rb"); if (!f) { fprintf(stderr, "NO_QUERY\n"); return 3; }
  fread(qry, 4, (size_t)nq * dim, f); fclose(f);
  f = fopen(gt_path, "rb"); if (!f) { fprintf(stderr, "NO_GT\n"); return 4; }
  fread(gt, 4, (size_t)nq * gt_k, f); fclose(f);

  cuvsResources_t res;
  if (cuvsResourcesCreate(&res) != CUVS_SUCCESS) { fprintf(stderr, "RES_FAIL\n"); return 5; }

  DLManagedTensor dataset; long ds[2] = {n, dim};
  memset(&dataset, 0, sizeof(dataset));
  dataset.dl_tensor.data = base;
  dataset.dl_tensor.device.device_type = kDLCPU;
  dataset.dl_tensor.ndim = 2;
  dataset.dl_tensor.dtype.code = kDLFloat; dataset.dl_tensor.dtype.bits = 32;
  dataset.dl_tensor.shape = ds;

  /* ---- 1. brute-force build (attach dataset) ---- */
  setbuf(stdout, NULL); setbuf(stderr, NULL);
  printf("stage: bf_build ...\n");
  cuvsBruteForceIndex_t bf = 0;
  if (cuvsBruteForceIndexCreate(&bf) != CUVS_SUCCESS) { fprintf(stderr, "BF_INDEX_FAIL\n"); return 6; }
  double t0 = now_ms();
  if (cuvsBruteForceBuild(res, &dataset, L2Expanded, 0.f, bf) != CUVS_SUCCESS) {
    fprintf(stderr, "BF_BUILD_FAIL: %s\n", cuvsGetLastErrorText()); return 7;
  }
  double bf_build_ms = now_ms() - t0;
  printf("stage: bf_build OK (%.0f ms); bf_self_search ...\n", bf_build_ms);

  /* ---- 2. self-search exact top-gdeg ---- */
  long nn[2] = {n, gdeg};
  size_t nb_bytes = (size_t)n * gdeg * (nbits == 32 ? 4 : 8);
  void *nb = malloc(nb_bytes);
  float *ndist = malloc((size_t)n * gdeg * 4);
  DLManagedTensor queries = dataset;   /* same host tensor */
  DLManagedTensor neighbors; memset(&neighbors, 0, sizeof(neighbors));
  neighbors.dl_tensor.data = nb;
  neighbors.dl_tensor.device.device_type = kDLCPU;
  neighbors.dl_tensor.ndim = 2;
  neighbors.dl_tensor.dtype.code = (nbits == 32) ? kDLUInt : kDLInt;
  neighbors.dl_tensor.dtype.bits = nbits;
  neighbors.dl_tensor.shape = nn;
  DLManagedTensor distances = neighbors;
  distances.dl_tensor.data = ndist;
  distances.dl_tensor.dtype.code = kDLFloat; distances.dl_tensor.dtype.bits = 32;

  cuvsFilter flt; flt.type = NO_FILTER; flt.addr = (uintptr_t)0;
  t0 = now_ms();
  if (cuvsBruteForceSearch(res, bf, &queries, &neighbors, &distances, flt) != CUVS_SUCCESS) {
    fprintf(stderr, "BF_SEARCH_FAIL: %s\n", cuvsGetLastErrorText()); return 8;
  }
  double bf_search_ms = now_ms() - t0;
  printf("stage: bf_self_search OK (%.0f ms); convert+fromargs ...\n", bf_search_ms);

  /* self-hit sanity: row i's top-1 neighbour must be i */
  long self_hit = 0;
  for (int i = 0; i < n; i++) {
    long long first = (nbits == 32) ? ((unsigned *)nb)[(size_t)i * gdeg]
                                    : ((long long *)nb)[(size_t)i * gdeg];
    if (first == i) self_hit++;
  }

  /* ---- 3. convert to uint32 CAGRA graph + FromArgs ---- */
  unsigned *graph = malloc((size_t)n * gdeg * 4);
  for (size_t j = 0; j < (size_t)n * gdeg; j++) {
    graph[j] = (nbits == 32) ? ((unsigned *)nb)[j] : (unsigned)((long long *)nb)[j];
  }
  DLManagedTensor g32; memset(&g32, 0, sizeof(g32));
  long gs[2] = {n, gdeg};
  g32.dl_tensor.data = graph;
  g32.dl_tensor.device.device_type = kDLCPU;
  g32.dl_tensor.ndim = 2;
  g32.dl_tensor.dtype.code = kDLUInt; g32.dl_tensor.dtype.bits = 32;
  g32.dl_tensor.shape = gs;

  cuvsCagraIndex_t cidx = 0;
  if (cuvsCagraIndexCreate(&cidx) != CUVS_SUCCESS) { fprintf(stderr, "CAGRA_INDEX_FAIL\n"); return 9; }
  t0 = now_ms();
  if (cuvsCagraIndexFromArgs(res, L2Expanded, &g32, &dataset, cidx) != CUVS_SUCCESS) {
    fprintf(stderr, "FROMARGS_FAIL: %s\n", cuvsGetLastErrorText()); return 10;
  }
  double fromargs_ms = now_ms() - t0;
  printf("stage: fromargs OK (%.0f ms); cagra_search+recall ...\n", fromargs_ms);

  /* ---- 4. CAGRA search + recall ---- */
  unsigned *ids = malloc((size_t)nq * topk * 4);
  float *qd = 0, *dd = 0; unsigned *nd = 0;
  cuvsRMMAlloc(res, (void **)&qd, sizeof(float) * nq * dim);
  cuvsRMMAlloc(res, (void **)&nd, sizeof(unsigned) * nq * topk);
  cuvsRMMAlloc(res, (void **)&dd, sizeof(float) * nq * topk);
  cudaMemcpy(qd, qry, sizeof(float) * nq * dim, cudaMemcpyDefault);
  DLManagedTensor qt, nt, dt;
  long qs[2] = {nq, dim}, nsh[2] = {nq, topk}, dsh[2] = {nq, topk};
  memset(&qt, 0, sizeof(qt)); memset(&nt, 0, sizeof(nt)); memset(&dt, 0, sizeof(dt));
  qt.dl_tensor.data = qd; qt.dl_tensor.device.device_type = kDLCUDA;
  qt.dl_tensor.ndim = 2; qt.dl_tensor.dtype.code = kDLFloat; qt.dl_tensor.dtype.bits = 32;
  qt.dl_tensor.shape = qs;
  nt = qt; nt.dl_tensor.data = nd; nt.dl_tensor.dtype.code = kDLUInt; nt.dl_tensor.shape = nsh;
  dt = qt; dt.dl_tensor.data = dd; dt.dl_tensor.dtype.code = kDLFloat; dt.dl_tensor.shape = dsh;
  cuvsCagraSearchParams_t sp = 0;
  cuvsCagraSearchParamsCreate(&sp);
  { size_t it = (size_t)topk * 4; if (it < 512) it = 512; sp->itopk_size = it; sp->search_width = 4; }
  t0 = now_ms();
  if (cuvsCagraSearch(res, sp, cidx, &qt, &nt, &dt, flt) != CUVS_SUCCESS) {
    fprintf(stderr, "SEARCH_FAIL: %s\n", cuvsGetLastErrorText()); return 11;
  }
  double search_ms = now_ms() - t0;
  cudaMemcpy(ids, nd, sizeof(unsigned) * nq * topk, cudaMemcpyDefault);

  double r10_hit = 0, r10_tot = 0, rk_hit = 0, rk_tot = 0;
  int k10 = topk < 10 ? topk : 10;
  for (int q = 0; q < nq; q++) {
    for (int k = 0; k < k10; k++) {
      unsigned want = (unsigned)gt[(size_t)q * gt_k + k];
      for (int j = 0; j < k10; j++) if (ids[(size_t)q * topk + j] == want) { r10_hit++; break; }
    }
    r10_tot += k10;
    for (int k = 0; k < topk; k++) {
      unsigned want = (unsigned)gt[(size_t)q * gt_k + k];
      for (int j = 0; j < topk; j++) if (ids[(size_t)q * topk + j] == want) { rk_hit++; break; }
    }
    rk_tot += topk;
  }

  printf("N=%d DIM=%d GDEG=%d NBITS=%d BF_BUILD_MS=%.1f BF_SEARCH_MS=%.1f FROMARGS_MS=%.1f "
         "SEARCH_MS=%.1f SELF_HIT=%.4f RECALL10=%.4f RECALLK=%.4f\n",
         n, dim, gdeg, nbits, bf_build_ms, bf_search_ms, fromargs_ms, search_ms,
         (double)self_hit / n,
         r10_tot ? r10_hit / r10_tot : 0, rk_tot ? rk_hit / rk_tot : 0);
  return 0;
}

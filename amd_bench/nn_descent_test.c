/* nn_descent + FromArgs experiment: build a knn graph with the standalone
 * nn_descent C API (which has termination_threshold / max_iterations, unlike
 * CAGRA's fixed nn_descent_niter), then load it into a CAGRA index via
 * cuvsCagraIndexFromArgs and search with the official cuvsCagraSearch.
 *
 * Usage: ./nn_descent_test <n> <dim> <max_iter> <term_th>
 * Env: BASE_F32 / QUERY_F32 / GT_I32 (default d256), GT_Q/GT_K
 */
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/nn_descent.h>
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
  int max_iter = argc > 3 ? atoi(argv[3]) : 20;
  float term_th = argc > 4 ? (float)atof(argv[4]) : 0.0001f;
  int graph_degree = 128, igdeg = 128;
  int nq = 100, topk = 100;

  const char *base_path = getenv("BASE_F32");
  const char *qry_path = getenv("QUERY_F32");
  const char *gt_path = getenv("GT_I32");
  if (!base_path) base_path = "/workspace/work/d256/base.f32";
  if (!qry_path) qry_path = "/workspace/work/d256/query.f32";
  if (!gt_path) gt_path = "/workspace/work/d256/gt.i32";

  float *base = (float *)malloc((size_t)n * dim * 4);
  float *qry = (float *)malloc((size_t)nq * dim * 4);
  int *gt = (int *)malloc((size_t)nq * topk * 4);
  FILE *f = fopen(base_path, "rb"); if (!f) { fprintf(stderr, "NO_BASE\n"); return 2; }
  fread(base, 4, (size_t)n * dim, f); fclose(f);
  f = fopen(qry_path, "rb"); if (!f) { fprintf(stderr, "NO_QUERY\n"); return 3; }
  fread(qry, 4, (size_t)nq * dim, f); fclose(f);
  f = fopen(gt_path, "rb"); if (!f) { fprintf(stderr, "NO_GT\n"); return 4; }
  fread(gt, 4, (size_t)nq * topk, f); fclose(f);

  cuvsResources_t res;
  if (cuvsResourcesCreate(&res) != CUVS_SUCCESS) { fprintf(stderr, "RES_FAIL\n"); return 5; }

  /* dataset on host CPU */
  DLManagedTensor dataset; long ds[2] = {n, dim};
  memset(&dataset, 0, sizeof(dataset));
  dataset.dl_tensor.data = base;
  dataset.dl_tensor.device.device_type = kDLCPU; dataset.dl_tensor.device.device_id = 0;
  dataset.dl_tensor.ndim = 2;
  dataset.dl_tensor.dtype.code = kDLFloat; dataset.dl_tensor.dtype.bits = 32; dataset.dl_tensor.dtype.lanes = 1;
  dataset.dl_tensor.shape = ds; dataset.dl_tensor.strides = 0; dataset.dl_tensor.byte_offset = 0;

  /* nn_descent build */
  cuvsNNDescentIndexParams_t np = 0;
  cuvsNNDescentIndexParamsCreate(&np);
  np->metric = L2Expanded;
  np->metric_arg = 2.0f;
  np->graph_degree = graph_degree;
  np->intermediate_graph_degree = igdeg;
  np->max_iterations = max_iter;
  np->termination_threshold = term_th;
  np->return_distances = false;

  cuvsNNDescentIndex_t nidx = 0;
  cuvsNNDescentIndexCreate(&nidx);
  double t0 = now_ms();
  cuvsError_t st = cuvsNNDescentBuild(res, np, &dataset, NULL, nidx);
  double build_ms = now_ms() - t0;
  if (st != CUVS_SUCCESS) { fprintf(stderr, "NNDESCENT_BUILD_FAIL: %s\n", cuvsGetLastErrorText()); return 6; }

  /* get graph (host memory, uint32) */
  DLManagedTensor graph; long gs[2] = {n, graph_degree};
  memset(&graph, 0, sizeof(graph));
  graph.dl_tensor.data = malloc((size_t)n * graph_degree * 4);
  graph.dl_tensor.device.device_type = kDLCPU; graph.dl_tensor.device.device_id = 0;
  graph.dl_tensor.ndim = 2;
  graph.dl_tensor.dtype.code = kDLUInt; graph.dl_tensor.dtype.bits = 32; graph.dl_tensor.dtype.lanes = 1;
  graph.dl_tensor.shape = gs; graph.dl_tensor.strides = 0; graph.dl_tensor.byte_offset = 0;
  st = cuvsNNDescentIndexGetGraph(res, nidx, &graph);
  if (st != CUVS_SUCCESS) { fprintf(stderr, "GET_GRAPH_FAIL: %s\n", cuvsGetLastErrorText()); return 7; }

  /* FromArgs: load graph + dataset into CAGRA index */
  cuvsCagraIndex_t cidx = 0;
  cuvsCagraIndexCreate(&cidx);
  t0 = now_ms();
  st = cuvsCagraIndexFromArgs(res, L2Expanded, &graph, &dataset, cidx);
  double fromargs_ms = now_ms() - t0;
  if (st != CUVS_SUCCESS) { fprintf(stderr, "FROMARGS_FAIL: %s\n", cuvsGetLastErrorText()); return 8; }

  /* search */
  unsigned *ids = (unsigned *)malloc((size_t)nq * topk * 4);
  float *qd = 0; unsigned *nd = 0; float *dd = 0; cuvsCagraSearchParams_t sp = 0;
  if (cuvsRMMAlloc(res, (void **)&qd, sizeof(float) * nq * dim) != CUVS_SUCCESS) { fprintf(stderr, "ALLOC_QD\n"); return 9; }
  if (cuvsRMMAlloc(res, (void **)&nd, sizeof(unsigned) * nq * topk) != CUVS_SUCCESS) { fprintf(stderr, "ALLOC_ND\n"); return 10; }
  if (cuvsRMMAlloc(res, (void **)&dd, sizeof(float) * nq * topk) != CUVS_SUCCESS) { fprintf(stderr, "ALLOC_DD\n"); return 11; }
  if (cudaMemcpy(qd, qry, sizeof(float) * nq * dim, cudaMemcpyDefault) != cudaSuccess) { fprintf(stderr, "H2D\n"); return 12; }
  DLManagedTensor qt, nt, dt; long qs[2] = {nq, dim}, nsh[2] = {nq, topk}, dsh[2] = {nq, topk};
  memset(&qt, 0, sizeof(qt)); memset(&nt, 0, sizeof(nt)); memset(&dt, 0, sizeof(dt));
  qt.dl_tensor.data = qd; qt.dl_tensor.device.device_type = kDLCUDA; qt.dl_tensor.device.device_id = 0;
  qt.dl_tensor.ndim = 2; qt.dl_tensor.dtype.code = kDLFloat; qt.dl_tensor.dtype.bits = 32; qt.dl_tensor.dtype.lanes = 1;
  qt.dl_tensor.shape = qs; qt.dl_tensor.strides = 0; qt.dl_tensor.byte_offset = 0;
  nt = qt; nt.dl_tensor.data = nd; nt.dl_tensor.dtype.code = kDLUInt; nt.dl_tensor.shape = nsh;
  dt = qt; dt.dl_tensor.data = dd; dt.dl_tensor.dtype.code = kDLFloat; dt.dl_tensor.shape = dsh;
  if (cuvsCagraSearchParamsCreate(&sp) != CUVS_SUCCESS) { fprintf(stderr, "SP_CREATE\n"); return 13; }
  { size_t it = (size_t)topk * 4; if (it < 512) it = 512; if (sp->itopk_size < it) sp->itopk_size = it; sp->search_width = 4; }
  cuvsFilter flt; flt.type = NO_FILTER; flt.addr = (uintptr_t)0;
  if (cuvsCagraSearch(res, sp, cidx, &qt, &nt, &dt, flt) != CUVS_SUCCESS) { fprintf(stderr, "SEARCH_FAIL: %s\n", cuvsGetLastErrorText()); return 14; }
  if (cudaMemcpy(ids, nd, sizeof(unsigned) * nq * topk, cudaMemcpyDefault) != cudaSuccess) { fprintf(stderr, "D2H\n"); return 15; }

  /* recall@10 and recall@topk */
  double r10_hit = 0, r10_tot = 0, rk_hit = 0, rk_tot = 0;
  int k10 = topk < 10 ? topk : 10;
  for (int q = 0; q < nq; q++) {
    for (int k = 0; k < k10; k++) {
      unsigned want = (unsigned)gt[(size_t)q * topk + k];
      for (int j = 0; j < k10; j++) if (ids[(size_t)q * topk + j] == want) { r10_hit++; break; }
    }
    r10_tot += k10;
    for (int k = 0; k < topk; k++) {
      unsigned want = (unsigned)gt[(size_t)q * topk + k];
      for (int j = 0; j < topk; j++) if (ids[(size_t)q * topk + j] == want) { rk_hit++; break; }
    }
    rk_tot += topk;
  }

  printf("MAX_ITER=%d TERM_TH=%g BUILD_MS=%.1f FROMARGS_MS=%.1f RECALL10=%.4f RECALLK=%.4f N=%d DIM=%d\n",
         max_iter, term_th, build_ms, fromargs_ms,
         r10_tot ? r10_hit / r10_tot : 0.0, rk_tot ? rk_hit / rk_tot : 0.0, n, dim);

  free(base); free(qry); free(gt); free(ids);
  return 0;
}

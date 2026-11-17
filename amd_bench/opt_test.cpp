/* opt_test.cpp v1 (2026-11-17 09:40) -- RobustPrune probe for the bf pipeline
 *
 * Pipeline:  BF exact self-search top-(kin+1) -> drop self -> uint32 knn (n x kin)
 *            -> cuvs::neighbors::cagra::helpers::optimize (RobustPrune)
 *            -> uint32 cagra graph (n x kout) -> cuvsCagraIndexFromArgs
 *            -> cuvsCagraSearch recall (same params as the bridge)
 * Reference: same run WITHOUT optimize (truncate knn to kout columns).
 *
 * Signature (from /opt/hipvs/include/cuvs/neighbors/cagra_optimize.hpp):
 *   void cuvs::neighbors::cagra::helpers::optimize(
 *       raft::resources const&,
 *       raft::host_matrix_view<uint32_t,int64_t,raft::row_major> knn_graph,
 *       raft::host_matrix_view<uint32_t,int64_t,raft::row_major> new_graph);
 *
 * Usage: ./opt_test <n> <dim> [kin] [kout]
 *   env: BASE_F32 QUERY_F32 GT_I32 GT_Q GT_K
 * Exit codes: 2-13 setup/API failures (each prints a label).
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/time.h>
#include <vector>

#include <dlpack/dlpack.h>
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/brute_force.h>
#include <cuvs/neighbors/cagra.h>

/* C++ side: the RobustPrune wrapper. Requires raft headers on the include
 * path (dump_hipvs_headers.sh section 7 locates them). */
#include <raft/core/resources.hpp>
#include <raft/core/host_mdspan.hpp>
#include <cuvs/neighbors/cagra_optimize.hpp>

static double now_ms(void) {
  struct timeval tv; gettimeofday(&tv, 0);
  return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

static void init_tensor(DLManagedTensor *t, void *data, long rows, long cols,
                        int dev, int code, int bits) {
  memset(t, 0, sizeof(*t));
  t->dl_tensor.data = data;
  t->dl_tensor.device.device_type = dev ? kDLCUDA : kDLCPU;
  t->dl_tensor.device.device_id = 0;
  t->dl_tensor.ndim = 2;
  t->dl_tensor.dtype.code = code;
  t->dl_tensor.dtype.bits = bits;
  t->dl_tensor.dtype.lanes = 1;
  t->dl_tensor.shape = (int64_t *)malloc(2 * sizeof(int64_t));
  ((int64_t *)t->dl_tensor.shape)[0] = rows;
  ((int64_t *)t->dl_tensor.shape)[1] = cols;
}

/* Borrow the raft::resources behind a cuvsResources_t (opaque handle for
 * raft::resources per cuvs/core/c_api.h). No ctor symbol needed. */
static raft::resources &as_res(cuvsResources_t h) {
  return *reinterpret_cast<raft::resources *>(reinterpret_cast<void *>(h));
}

struct Recall { double r10, rk; };

static Recall measure(cuvsResources_t res, DLManagedTensor *dset_host,
                      DLManagedTensor &qt, unsigned *gt, int nq, int topk,
                      int gt_k, unsigned *graph, long n, long gdeg, long dim,
                      double *search_ms) {
  Recall out{0, 0};
  DLManagedTensor g32;
  init_tensor(&g32, graph, n, gdeg, 0, kDLUInt, 32);
  cuvsCagraIndex_t idx = 0;
  if (cuvsCagraIndexCreate(&idx) != CUVS_SUCCESS) { fprintf(stderr, "CAGRA_INDEX_FAIL\n"); return out; }
  if (cuvsCagraIndexFromArgs(res, L2Expanded, &g32, dset_host, idx) != CUVS_SUCCESS) {
    fprintf(stderr, "FROMARGS_FAIL: %s\n", cuvsGetLastErrorText());
    cuvsCagraIndexDestroy(idx); return out;
  }
  unsigned *ids = (unsigned *)malloc((size_t)nq * topk * 4);
  float *dd = 0; unsigned *nd = 0;
  cuvsRMMAlloc(res, (void **)&nd, sizeof(unsigned) * nq * topk);
  cuvsRMMAlloc(res, (void **)&dd, sizeof(float) * nq * topk);
  DLManagedTensor nt, dt;
  init_tensor(&nt, nd, nq, topk, 1, kDLUInt, 32);
  init_tensor(&dt, dd, nq, topk, 1, kDLFloat, 32);
  cuvsCagraSearchParams_t sp = 0;
  cuvsCagraSearchParamsCreate(&sp);
  { size_t it = (size_t)topk * 4; if (it < 512) it = 512; sp->itopk_size = it; sp->search_width = 4; }
  double t0 = now_ms();
  cuvsFilter flt; flt.type = NO_FILTER; flt.addr = (uintptr_t)0;
  if (cuvsCagraSearch(res, sp, idx, &qt, &nt, &dt, flt) != CUVS_SUCCESS) {
    fprintf(stderr, "SEARCH_FAIL: %s\n", cuvsGetLastErrorText());
    cuvsCagraSearchParamsDestroy(sp); cuvsCagraIndexDestroy(idx);
    free(ids); cuvsRMMFree(res, nd, sizeof(unsigned) * nq * topk); cuvsRMMFree(res, dd, sizeof(float) * nq * topk);
    return out;
  }
  *search_ms = now_ms() - t0;
  cudaMemcpy(ids, nd, sizeof(unsigned) * (size_t)nq * topk, cudaMemcpyDeviceToHost);
  int k10 = topk < 10 ? topk : 10;
  double h10 = 0, t10 = 0, hk = 0, tk = 0;
  for (int q = 0; q < nq; q++) {
    for (int k = 0; k < k10; k++) {
      unsigned want = (unsigned)gt[(size_t)q * gt_k + k];
      for (int j = 0; j < k10; j++) if (ids[(size_t)q * topk + j] == want) { h10++; break; }
    }
    t10 += k10;
    for (int k = 0; k < topk; k++) {
      unsigned want = (unsigned)gt[(size_t)q * gt_k + k];
      for (int j = 0; j < topk; j++) if (ids[(size_t)q * topk + j] == want) { hk++; break; }
    }
    tk += topk;
  }
  out.r10 = h10 / t10; out.rk = hk / tk;
  free(ids); cuvsRMMFree(res, nd, sizeof(unsigned) * nq * topk); cuvsRMMFree(res, dd, sizeof(float) * nq * topk);
  cuvsCagraSearchParamsDestroy(sp); cuvsCagraIndexDestroy(idx);
  return out;
}

int main(int argc, char **argv) {
  int n = argc > 1 ? atoi(argv[1]) : 250000;
  int dim = argc > 2 ? atoi(argv[2]) : 256;
  int kin = argc > 3 ? atoi(argv[3]) : 256;
  int kout = argc > 4 ? atoi(argv[4]) : 128;
  int nq = 100, topk = 100;
  const char *base_path = getenv("BASE_F32");
  const char *qry_path = getenv("QUERY_F32");
  const char *gt_path = getenv("GT_I32");
  if (!base_path) base_path = "/workspace/work/d256/base.f32";
  if (!qry_path) qry_path = "/workspace/work/d256/query.f32";
  if (!gt_path) gt_path = "/workspace/work/d256/gt.i32";
  int gt_k = getenv("GT_K") ? atoi(getenv("GT_K")) : 100;

  setbuf(stdout, NULL); setbuf(stderr, NULL);

  float *base = (float *)malloc((size_t)n * dim * 4);
  float *qry = (float *)malloc((size_t)nq * dim * 4);
  unsigned *gt = (unsigned *)malloc((size_t)nq * gt_k * 4);
  FILE *f = fopen(base_path, "rb"); if (!f) { fprintf(stderr, "NO_BASE\n"); return 2; }
  fread(base, 4, (size_t)n * dim, f); fclose(f);
  f = fopen(qry_path, "rb"); if (!f) { fprintf(stderr, "NO_QUERY\n"); return 3; }
  fread(qry, 4, (size_t)nq * dim, f); fclose(f);
  f = fopen(gt_path, "rb"); if (!f) { fprintf(stderr, "NO_GT\n"); return 4; }
  fread(gt, 4, (size_t)nq * gt_k, f); fclose(f);

  cuvsResources_t res;
  if (cuvsResourcesCreate(&res) != CUVS_SUCCESS) { fprintf(stderr, "RES_FAIL\n"); return 5; }

  /* dataset on DEVICE for BF */
  float *d_base = 0;
  if (cuvsRMMAlloc(res, (void **)&d_base, (size_t)n * dim * 4) != CUVS_SUCCESS) {
    fprintf(stderr, "RMM_FAIL\n"); return 5;
  }
  cudaMemcpy(d_base, base, (size_t)n * dim * 4, cudaMemcpyHostToDevice);
  DLManagedTensor dataset_dev;
  init_tensor(&dataset_dev, d_base, n, dim, 1, kDLFloat, 32);
  DLManagedTensor dataset_host;
  init_tensor(&dataset_host, base, n, dim, 0, kDLFloat, 32);

  printf("stage: bf_build ...\n");
  cuvsBruteForceIndex_t bf = 0;
  if (cuvsBruteForceIndexCreate(&bf) != CUVS_SUCCESS) { fprintf(stderr, "BF_INDEX_FAIL\n"); return 6; }
  double t0 = now_ms();
  if (cuvsBruteForceBuild(res, &dataset_dev, L2Expanded, 0.f, bf) != CUVS_SUCCESS) {
    fprintf(stderr, "BF_BUILD_FAIL: %s\n", cuvsGetLastErrorText()); return 7;
  }
  double bf_build_ms = now_ms() - t0;

  printf("stage: bf_self_search top-%d ...\n", kin + 1);
  size_t k1 = (size_t)kin + 1;
  size_t cells = (size_t)n * k1;
  int64_t *d_nb = 0; float *d_nd = 0;
  cuvsRMMAlloc(res, (void **)&d_nb, cells * sizeof(int64_t));
  cuvsRMMAlloc(res, (void **)&d_nd, cells * sizeof(float));
  DLManagedTensor queries = dataset_dev;
  DLManagedTensor nt, dt;
  init_tensor(&nt, d_nb, n, k1, 1, kDLInt, 64);
  init_tensor(&dt, d_nd, n, k1, 1, kDLFloat, 32);
  cuvsFilter flt; flt.type = NO_FILTER; flt.addr = (uintptr_t)0;
  t0 = now_ms();
  if (cuvsBruteForceSearch(res, bf, &queries, &nt, &dt, flt) != CUVS_SUCCESS) {
    fprintf(stderr, "BF_SEARCH_FAIL: %s\n", cuvsGetLastErrorText()); return 8;
  }
  double bf_search_ms = now_ms() - t0;
  std::vector<int64_t> h_nb(cells);
  cudaMemcpy(h_nb.data(), d_nb, cells * sizeof(int64_t), cudaMemcpyDeviceToHost);

  long self_hit = 0;
  for (int i = 0; i < n; i++) if (h_nb[(size_t)i * k1] == (int64_t)i) self_hit++;

  /* uint32 knn (n x kin), self dropped */
  std::vector<unsigned> knn((size_t)n * kin);
  for (size_t i = 0; i < (size_t)n; i++) {
    size_t out = 0;
    for (size_t c = 0; c < k1 && out < (size_t)kin; c++) {
      int64_t nb = h_nb[i * k1 + c];
      if (nb == (int64_t)i) continue;
      knn[i * kin + out++] = (unsigned)nb;
    }
    while (out < (size_t)kin) knn[i * kin + out++] = (unsigned)i;
  }

  /* query tensor for recall */
  float *qd = 0;
  cuvsRMMAlloc(res, (void **)&qd, sizeof(float) * nq * dim);
  cudaMemcpy(qd, qry, sizeof(float) * nq * dim, cudaMemcpyHostToDevice);
  DLManagedTensor qt;
  init_tensor(&qt, qd, nq, dim, 1, kDLFloat, 32);

  /* ---- reference: no optimize (truncate knn to kout) ---- */
  std::vector<unsigned> trunc((size_t)n * kout);
  for (size_t i = 0; i < (size_t)n; i++)
    for (int j = 0; j < kout; j++) trunc[i * kout + j] = knn[i * kin + j];
  double ref_search = 0;
  printf("stage: noopt fromargs+search ...\n");
  Recall ref = measure(res, &dataset_host, qt, gt, nq, topk, gt_k,
                       trunc.data(), n, kout, dim, &ref_search);

  /* ---- optimize (RobustPrune) ---- */
  printf("stage: helpers::optimize (%dx%d -> %dx%d) ...\n", n, kin, n, kout);
  std::vector<unsigned> pruned((size_t)n * kout);
  t0 = now_ms();
  int opt_rc = 0;
  try {
    auto &res_cpp = as_res(res);
    auto v_in = raft::make_host_matrix_view<uint32_t, int64_t>(knn.data(), n, kin);
    auto v_out = raft::make_host_matrix_view<uint32_t, int64_t>(pruned.data(), n, kout);
    cuvs::neighbors::cagra::helpers::optimize(res_cpp, v_in, v_out);
  } catch (std::exception &e) {
    fprintf(stderr, "OPTIMIZE_EXC: %s\n", e.what());
    opt_rc = 1;
  } catch (...) {
    fprintf(stderr, "OPTIMIZE_EXC: unknown\n");
    opt_rc = 1;
  }
  double opt_ms = now_ms() - t0;
  if (opt_rc != 0) return 12;

  double opt_search = 0;
  printf("stage: opt fromargs+search ...\n");
  Recall opt = measure(res, &dataset_host, qt, gt, nq, topk, gt_k,
                       pruned.data(), n, kout, dim, &opt_search);

  printf("N=%d DIM=%d KIN=%d KOUT=%d SELF_HIT=%.4f "
         "BF_BUILD_MS=%.1f BF_SEARCH_MS=%.1f OPT_MS=%.1f "
         "NOOPT_R10=%.4f NOOPT_RK=%.4f OPT_R10=%.4f OPT_RK=%.4f "
         "NOOPT_SEARCH_MS=%.1f OPT_SEARCH_MS=%.1f\n",
         n, dim, kin, kout, (double)self_hit / n,
         bf_build_ms, bf_search_ms, opt_ms,
         ref.r10, ref.rk, opt.r10, opt.rk, ref_search, opt_search);
  return 0;
}

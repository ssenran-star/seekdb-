/* bf_test: build a CAGRA graph from an EXACT brute-force kNN graph.
 *
 * Pipeline:  BF build (device dataset) -> BF self-search top-k
 *            -> uint32 graph -> cuvsCagraIndexFromArgs (host tensors)
 *            -> cuvsCagraSearch recall vs exact GT.
 *
 * Notes learned from the RAFT interop layer (errors from prior runs):
 *   - cuvsBruteForceBuild wants a DEVICE tensor (memory_type=device) with
 *     dtype.lanes == 1; host tensors fail with "device_type mismatch".
 *   - cuvsNNDescentBuild / cuvsCagraIndexFromArgs accept HOST tensors.
 *   - outputs of cuvsBruteForceSearch are attempted host-first, then device.
 *
 * Sanity: each row's nearest neighbour must be itself -> SELF_HIT ~1.0.
 * If SELF_HIT is ~0 the neighbours dtype is wrong (try NEIGHBOR_BITS=32).
 *
 * Usage: ./bf_test <n> <dim> [gdeg]
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

int main(int argc, char **argv) {
  int n = argc > 1 ? atoi(argv[1]) : 250000;
  int dim = argc > 2 ? atoi(argv[2]) : 256;
  int gdeg = argc > 3 ? atoi(argv[3]) : 128;
  int nq = 100, topk = 100;
  int nbits = getenv("NEIGHBOR_BITS") ? atoi(getenv("NEIGHBOR_BITS")) : 64;

  const char *base_path = getenv("BASE_F32");
  const char *qry_path = getenv("QUERY_F32");
  const char *gt_path = getenv("GT_I32");
  if (!base_path) base_path = "/workspace/work/d256/base.f32";
  if (!qry_path) qry_path = "/workspace/work/d256/query.f32";
  if (!gt_path) gt_path = "/workspace/work/d256/gt.i32";
  int gt_q = getenv("GT_Q") ? atoi(getenv("GT_Q")) : 100;
  int gt_k = getenv("GT_K") ? atoi(getenv("GT_K")) : 100;

  setbuf(stdout, NULL); setbuf(stderr, NULL);

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

  /* ---- 0. dataset on DEVICE (cuvsBruteForceBuild requires it) ----
   * FP16=1: feed the BF distance pass a float16 dataset (memory-bound self
   * search is ~2x faster). The CAGRA graph is still FromArgs'd with the
   * float32 host dataset, so query-time refinement and semantics are
   * unchanged -- only graph construction sees the reduced precision. */
  int use_fp16 = (getenv("FP16") && atoi(getenv("FP16"))) ? 1 : 0;
  if (use_fp16) printf("mode: FP16 distance pass (graph FromArgs stays fp32)\n");
  float *d_base = 0;
  size_t dev_bytes = (size_t)n * dim * (use_fp16 ? 2 : 4);
  if (cuvsRMMAlloc(res, (void **)&d_base, dev_bytes) != CUVS_SUCCESS) {
    fprintf(stderr, "RMM_FAIL\n"); return 5;
  }
  if (use_fp16) {
    unsigned short *h16 = (unsigned short *)malloc((size_t)n * dim * 2);
    for (size_t i = 0; i < (size_t)n * dim; i++) {
      float f = base[i];
      unsigned u; memcpy(&u, &f, 4);
      unsigned s = (u >> 16) & 0x8000u, e = (u >> 23) & 0xffu, m = u & 0x7fffffu;
      unsigned short h;
      if (e == 255) { h = (unsigned short)(s | 0x7c00u | (m ? 0x200u : 0)); }
      else {
        int he = (int)e - 127 + 15;
        if (he >= 31) { h = (unsigned short)(s | 0x7c00u); }
        else if (he <= 0) {
          if (he < -10) { h = (unsigned short)s; }
          else {
            m |= 0x800000u;
            unsigned hm = m >> ((unsigned)(1 - he) + 13);
            if ((m >> ((unsigned)(1 - he) + 12)) & 1u) hm++;
            h = (unsigned short)(s | hm);
          }
        } else {
          unsigned hm = m >> 13;
          if (m & 0x1000u) hm++;
          if (hm == 0x400u) { hm = 0; he++; if (he >= 31) h = (unsigned short)(s | 0x7c00u); else h = (unsigned short)(s | ((unsigned)he << 10) | hm); }
          else h = (unsigned short)(s | ((unsigned)he << 10) | hm);
        }
      }
      h16[i] = h;
    }
    cudaMemcpy(d_base, h16, dev_bytes, cudaMemcpyHostToDevice);
    free(h16);
  } else {
    cudaMemcpy(d_base, base, dev_bytes, cudaMemcpyHostToDevice);
  }

  DLManagedTensor dataset;
  init_tensor(&dataset, d_base, n, dim, 1, kDLFloat, use_fp16 ? 16 : 32);

  /* ---- 1. brute-force build ---- */
  printf("stage: bf_build (device dataset) ...\n");
  cuvsBruteForceIndex_t bf = 0;
  if (cuvsBruteForceIndexCreate(&bf) != CUVS_SUCCESS) { fprintf(stderr, "BF_INDEX_FAIL\n"); return 6; }
  double t0 = now_ms();
  if (cuvsBruteForceBuild(res, &dataset, L2Expanded, 0.f, bf) != CUVS_SUCCESS) {
    fprintf(stderr, "BF_BUILD_FAIL(metric_arg=0.f): %s\n", cuvsGetLastErrorText());
    if (cuvsBruteForceBuild(res, &dataset, L2Expanded, 2.0f, bf) != CUVS_SUCCESS) {
      fprintf(stderr, "BF_BUILD_FAIL(metric_arg=2.0f): %s\n", cuvsGetLastErrorText());
      return 7;
    }
  }
  double bf_build_ms = now_ms() - t0;
  printf("stage: bf_build OK (%.0f ms); bf_self_search ...\n", bf_build_ms);

  /* ---- 2. self-search exact top-(gdeg+1) (queries = same device dataset) ----
   * One extra candidate per row lets us drop the self-loop (row i is always
   * its own nearest neighbour at distance 0) and keep gdeg real neighbours. */
  size_t k1 = (size_t)gdeg + 1;
  size_t ncells = (size_t)n * k1;
  size_t nb_bytes = ncells * (nbits == 32 ? 4 : 8);
  void *h_nb = malloc(nb_bytes);
  float *h_ndist = malloc(ncells * 4);
  int nb_code = (nbits == 32) ? kDLUInt : kDLInt;

  DLManagedTensor queries = dataset;      /* device */
  DLManagedTensor n_h, d_h, n_d, d_d;
  init_tensor(&n_h, h_nb, n, k1, 0, nb_code, nbits);
  init_tensor(&d_h, h_ndist, n, k1, 0, kDLFloat, 32);
  void *d_nb = 0, *d_ndist = 0;
  cuvsRMMAlloc(res, &d_nb, nb_bytes);
  cuvsRMMAlloc(res, &d_ndist, ncells * 4);
  init_tensor(&n_d, d_nb, n, k1, 1, nb_code, nbits);
  init_tensor(&d_d, d_ndist, n, k1, 1, kDLFloat, 32);

  cuvsFilter flt; flt.type = NO_FILTER; flt.addr = (uintptr_t)0;
  t0 = now_ms();
  int used_host_out = 1;
  if (cuvsBruteForceSearch(res, bf, &queries, &n_h, &d_h, flt) != CUVS_SUCCESS) {
    fprintf(stderr, "BF_SEARCH_FAIL(host out): %s -- retrying device outputs\n",
            cuvsGetLastErrorText());
    used_host_out = 0;
    if (cuvsBruteForceSearch(res, bf, &queries, &n_d, &d_d, flt) != CUVS_SUCCESS) {
      fprintf(stderr, "BF_SEARCH_FAIL(device out): %s\n", cuvsGetLastErrorText());
      return 8;
    }
    cudaMemcpy(h_nb, d_nb, nb_bytes, cudaMemcpyDeviceToHost);
    cudaMemcpy(h_ndist, d_ndist, ncells * 4, cudaMemcpyDeviceToHost);
  }
  double bf_search_ms = now_ms() - t0;
  printf("stage: bf_self_search OK (%.0f ms, out=%s); convert+fromargs ...\n",
         bf_search_ms, used_host_out ? "host" : "device");

  /* self-hit sanity: row i's top-1 neighbour (of the raw k1 search) must be i */
  long self_hit = 0;
  for (int i = 0; i < n; i++) {
    long long first = (nbits == 32) ? ((unsigned *)h_nb)[(size_t)i * k1]
                                    : ((long long *)h_nb)[(size_t)i * k1];
    if (first == i) self_hit++;
  }

  /* ---- 3. convert to uint32 host graph (drop self) + FromArgs (host tensors) ---- */
  unsigned *graph = malloc((size_t)n * gdeg * 4);
  for (size_t i = 0; i < (size_t)n; i++) {
    size_t out = 0;
    for (size_t c = 0; c < k1 && out < (size_t)gdeg; c++) {
      long long nb = (nbits == 32) ? (long long)((unsigned *)h_nb)[i * k1 + c]
                                   : ((long long *)h_nb)[i * k1 + c];
      if (nb == (long long)i) continue;
      graph[i * gdeg + out++] = (unsigned)nb;
    }
    while (out < (size_t)gdeg) graph[i * gdeg + out++] = (unsigned)i; /* unreachable */
  }
  DLManagedTensor g32, dset_host;
  init_tensor(&g32, graph, n, gdeg, 0, kDLUInt, 32);
  init_tensor(&dset_host, base, n, dim, 0, kDLFloat, 32);

  cuvsCagraIndex_t cidx = 0;
  if (cuvsCagraIndexCreate(&cidx) != CUVS_SUCCESS) { fprintf(stderr, "CAGRA_INDEX_FAIL\n"); return 9; }
  t0 = now_ms();
  if (cuvsCagraIndexFromArgs(res, L2Expanded, &g32, &dset_host, cidx) != CUVS_SUCCESS) {
    fprintf(stderr, "FROMARGS_FAIL: %s\n", cuvsGetLastErrorText()); return 10;
  }
  double fromargs_ms = now_ms() - t0;
  printf("stage: fromargs OK (%.0f ms); cagra_search+recall ...\n", fromargs_ms);

  /* ---- 4. CAGRA search + recall (bridge pattern: device query) ---- */
  unsigned *ids = malloc((size_t)nq * topk * 4);
  float *qd = 0, *dd = 0; unsigned *nd = 0;
  cuvsRMMAlloc(res, (void **)&qd, sizeof(float) * nq * dim);
  cuvsRMMAlloc(res, (void **)&nd, sizeof(unsigned) * nq * topk);
  cuvsRMMAlloc(res, (void **)&dd, sizeof(float) * nq * topk);
  cudaMemcpy(qd, qry, sizeof(float) * nq * dim, cudaMemcpyHostToDevice);
  DLManagedTensor qt, nt, dt;
  init_tensor(&qt, qd, nq, dim, 1, kDLFloat, 32);
  init_tensor(&nt, nd, nq, topk, 1, kDLUInt, 32);
  init_tensor(&dt, dd, nq, topk, 1, kDLFloat, 32);
  cuvsCagraSearchParams_t sp = 0;
  cuvsCagraSearchParamsCreate(&sp);
  { size_t it = (size_t)topk * 4; if (it < 512) it = 512; sp->itopk_size = it; sp->search_width = 4; }
  t0 = now_ms();
  if (cuvsCagraSearch(res, sp, cidx, &qt, &nt, &dt, flt) != CUVS_SUCCESS) {
    fprintf(stderr, "SEARCH_FAIL: %s\n", cuvsGetLastErrorText()); return 11;
  }
  double search_ms = now_ms() - t0;
  cudaMemcpy(ids, nd, sizeof(unsigned) * nq * topk, cudaMemcpyDeviceToHost);

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

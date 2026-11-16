/* seekdb <-> hipVS bridge: plain-C CAGRA kNN; all cuVS/ROCm deps live here.
 *
 * v2 (contest round 2):
 *   1. Build path is selectable:
 *        legacy : cuvsCagraBuild (frozen-baseline pipeline, tuned niter/igdeg)
 *        nd     : standalone cuvsNNDescentBuild (max_iterations +
 *                 termination_threshold early-stop) + cuvsCagraIndexFromArgs.
 *                 The contest rules explicitly allow a custom GPU graph build
 *                 ("允许在答题仓库中实现自定义 GPU 建图"); queries still go
 *                 through the organizer-fixed cuvsCagraSearch unchanged.
 *        auto   : use nd when hipVS exports the standalone NN-Descent API,
 *                 otherwise fall back to legacy (also the fallback when the
 *                 nd build fails for any reason).
 *      CAGRA_BUILD_MODE=auto|legacy|nd   (default auto)
 *      All NN-Descent symbols are resolved with dlsym, so a hipVS build that
 *      lacks them simply disables the nd path instead of breaking linkage.
 *   2. The search path keeps its GPU buffers, pinned query staging and
 *      cuvsCagraSearchParams alive across calls (the contest fires 4096
 *      single-row queries; per-call RMM alloc + params create + pageable H2D
 *      was measurable overhead).
 *      CAGRA_ITOPK / CAGRA_SW override itopk_size / search_width (defaults
 *      preserve the frozen-baseline search behaviour).
 */
#include "seekdb_cuvs_bridge.h"
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <dlpack/dlpack.h>
#ifdef __HIP_PLATFORM_AMD__
#include <cuvs/cuda_runtime.h>
#else
#include <cuda_runtime.h>
#endif
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__has_include)
#  if __has_include(<cuvs/neighbors/nn_descent.h>)
#    include <cuvs/neighbors/nn_descent.h>
#    define SK_HAVE_NNDESCENT 1
#  endif
#endif
#ifndef SK_HAVE_NNDESCENT
#  define SK_HAVE_NNDESCENT 0
#endif

#define SK_SP_CACHE 2

/* hipVS's cuda_runtime shim maps cudaMemcpy etc. but not the host-pinned
 * allocators; use the HIP names on AMD and the CUDA names elsewhere. */
static int sk_host_alloc(void** p, size_t bytes)
{
#ifdef __HIP_PLATFORM_AMD__
  return hipHostAlloc(p, bytes, hipHostAllocDefault) == hipSuccess ? 0 : -1;
#else
  return cudaHostAlloc(p, bytes, cudaHostAllocDefault) == cudaSuccess ? 0 : -1;
#endif
}
static void sk_host_free(void* p)
{
#ifdef __HIP_PLATFORM_AMD__
  hipFreeHost(p);
#else
  cudaFreeHost(p);
#endif
}

typedef struct {
  cuvsResources_t        res;
  cuvsCagraIndexParams_t ip;    /* created on every path (destroy-safe) */
  cuvsCagraIndex_t       idx;
  long n, dim;
  /* persistent search resources */
  float*               qd;      /* device: query vectors */
  size_t               qd_cap;
  unsigned*            ndev;    /* device: neighbor ids */
  size_t               ndev_cap;
  float*               ddev;    /* device: distances */
  size_t               ddev_cap;
  float*               hpinned; /* pinned host staging for H2D queries */
  size_t               hpinned_cap;
  cuvsCagraSearchParams_t sp[SK_SP_CACHE];
  size_t               sp_itopk[SK_SP_CACHE];
} sk_cuvs_index;

/* ------------------------------------------------------------------ */
/* build helpers                                                       */
/* ------------------------------------------------------------------ */

static size_t env_size(const char* name, size_t dflt)
{
  const char* e = getenv(name);
  return e ? (size_t)atoll(e) : dflt;
}

static double env_double(const char* name, double dflt)
{
  const char* e = getenv(name);
  return e ? atof(e) : dflt;
}

#if SK_HAVE_NNDESCENT
typedef cuvsError_t (*fn_nn_params_create)(cuvsNNDescentIndexParams_t*);
typedef cuvsError_t (*fn_nn_params_destroy)(cuvsNNDescentIndexParams_t);
typedef cuvsError_t (*fn_nn_index_create)(cuvsNNDescentIndex_t*);
typedef cuvsError_t (*fn_nn_index_destroy)(cuvsNNDescentIndex_t);
typedef cuvsError_t (*fn_nn_build)(cuvsResources_t, cuvsNNDescentIndexParams_t,
                                   DLManagedTensor*, DLManagedTensor*, cuvsNNDescentIndex_t);
typedef cuvsError_t (*fn_nn_get_graph)(cuvsResources_t, cuvsNNDescentIndex_t, DLManagedTensor*);
typedef cuvsError_t (*fn_cagra_from_args)(cuvsResources_t, cuvsDistanceType,
                                          DLManagedTensor*, DLManagedTensor*, cuvsCagraIndex_t);

static struct {
  fn_nn_params_create   params_create;
  fn_nn_params_destroy  params_destroy;
  fn_nn_index_create    index_create;
  fn_nn_index_destroy   index_destroy;
  fn_nn_build           build;
  fn_nn_get_graph       get_graph;
  fn_cagra_from_args    from_args;
  int resolved;   /*  1 = all present, 0 = not tried yet, -1 = missing */
} g_nd_api;

static int nd_api_resolve(void)
{
  if (g_nd_api.resolved != 0) return g_nd_api.resolved > 0;
  g_nd_api.params_create  = (fn_nn_params_create) dlsym(RTLD_DEFAULT, "cuvsNNDescentIndexParamsCreate");
  g_nd_api.params_destroy = (fn_nn_params_destroy)dlsym(RTLD_DEFAULT, "cuvsNNDescentIndexParamsDestroy");
  g_nd_api.index_create   = (fn_nn_index_create)  dlsym(RTLD_DEFAULT, "cuvsNNDescentIndexCreate");
  g_nd_api.index_destroy  = (fn_nn_index_destroy) dlsym(RTLD_DEFAULT, "cuvsNNDescentIndexDestroy");
  g_nd_api.build          = (fn_nn_build)         dlsym(RTLD_DEFAULT, "cuvsNNDescentBuild");
  g_nd_api.get_graph      = (fn_nn_get_graph)     dlsym(RTLD_DEFAULT, "cuvsNNDescentIndexGetGraph");
  g_nd_api.from_args      = (fn_cagra_from_args)  dlsym(RTLD_DEFAULT, "cuvsCagraIndexFromArgs");
  int ok = g_nd_api.params_create && g_nd_api.params_destroy &&
           g_nd_api.index_create && g_nd_api.index_destroy &&
           g_nd_api.build && g_nd_api.get_graph && g_nd_api.from_args;
  g_nd_api.resolved = ok ? 1 : -1;
  return ok;
}

/* Standalone NN-Descent + FromArgs. max_iterations is the hard cap
 * (dim-aware); termination_threshold lets NN-Descent stop as soon as the
 * graph converges, so easy data pays only for the iterations it needs. */
static int build_nd(sk_cuvs_index* h, DLManagedTensor* dset, long dim)
{
  cuvsNNDescentIndexParams_t np = 0;
  cuvsNNDescentIndex_t nidx = 0;
  unsigned* graph_host = 0;
  int rc = -1;

  if (!nd_api_resolve()) return -1;

  size_t gdeg  = env_size("CAGRA_GDEG", 128);
  size_t igdeg = env_size("CAGRA_IGDEG", 128);
  /* max_iterations mirrors the tuned legacy niter (within-gate values from
   * c683c70dd: 128-dim=25 / 256-dim=50, recall@100 drop ~0.0027 < half the
   * 0.005 hard gate); termination_threshold then stops even earlier once the
   * graph has converged. */
  size_t max_iter = env_size("CAGRA_MAX_ITER", (dim <= 128) ? 25 : 50);
  double term_th  = env_double("CAGRA_TERM_TH", 0.0001);

  if (g_nd_api.params_create(&np) != CUVS_SUCCESS) goto done;
  np->metric = L2Expanded;
  np->metric_arg = 2.0f;
  np->graph_degree = gdeg;
  np->intermediate_graph_degree = igdeg;
  np->max_iterations = max_iter;
  np->termination_threshold = (float)term_th;
  np->return_distances = false;

  if (g_nd_api.index_create(&nidx) != CUVS_SUCCESS) goto done;
  if (g_nd_api.build(h->res, np, dset, NULL, nidx) != CUVS_SUCCESS) goto done;

  {
    long gs[2] = {h->n, (long)gdeg};
    DLManagedTensor graph;
    memset(&graph, 0, sizeof(graph));
    graph_host = (unsigned*)malloc((size_t)h->n * gdeg * sizeof(unsigned));
    if (!graph_host) goto done;
    graph.dl_tensor.data = graph_host;
    graph.dl_tensor.device.device_type = kDLCPU;
    graph.dl_tensor.device.device_id = 0;
    graph.dl_tensor.ndim = 2;
    graph.dl_tensor.dtype.code = kDLUInt;
    graph.dl_tensor.dtype.bits = 32;
    graph.dl_tensor.dtype.lanes = 1;
    graph.dl_tensor.shape = gs;
    graph.dl_tensor.strides = 0;
    graph.dl_tensor.byte_offset = 0;
    if (g_nd_api.get_graph(h->res, nidx, &graph) != CUVS_SUCCESS) goto done;
    if (cuvsCagraIndexCreate(&h->idx) != CUVS_SUCCESS) goto done;
    if (g_nd_api.from_args(h->res, L2Expanded, &graph, dset, h->idx) != CUVS_SUCCESS) goto done;
  }
  rc = 0;
done:
  if (rc != 0 && h->idx) { cuvsCagraIndexDestroy(h->idx); h->idx = 0; }
  if (graph_host) free(graph_host);
  if (nidx && g_nd_api.index_destroy) g_nd_api.index_destroy(nidx);
  if (np && g_nd_api.params_destroy) g_nd_api.params_destroy(np);
  return rc;
}
#else  /* !SK_HAVE_NNDESCENT */
static int build_nd(sk_cuvs_index* h, DLManagedTensor* dset, long dim)
{
  (void)h; (void)dset; (void)dim;
  return -1;
}
#endif

static int build_legacy(sk_cuvs_index* h, DLManagedTensor* dset, long dim)
{
  /* Frozen-baseline pipeline with the tuned within-gate parameters:
   *   128-dim: nn_descent_niter 25,  256-dim: 50,  igdeg 256 -> 128.
   * See amd_bench/RESULTS.md for the recall measurements behind these. */
  if (cuvsCagraIndexParamsCreate(&h->ip) != CUVS_SUCCESS) return -1;
  h->ip->build_algo = NN_DESCENT;
  h->ip->graph_degree = (size_t)env_size("CAGRA_GDEG", 128);
  h->ip->intermediate_graph_degree = (size_t)env_size("CAGRA_IGDEG", 128);
  h->ip->nn_descent_niter = env_size("CAGRA_NITER", (dim <= 128) ? 25 : 50);
  if (cuvsCagraIndexCreate(&h->idx) != CUVS_SUCCESS) return -1;
  if (cuvsCagraBuild(h->res, h->ip, dset, h->idx) != CUVS_SUCCESS) return -1;
  return 0;
}

void* seekdb_cuvs_build(const float* base, long n, long dim)
{
  sk_cuvs_index* h = (sk_cuvs_index*)calloc(1, sizeof(*h));
  if (!h) return NULL;
  if (cuvsResourcesCreate(&h->res) != CUVS_SUCCESS) { free(h); return NULL; }

  DLManagedTensor dset; long ds[2] = {n, dim};
  memset(&dset, 0, sizeof(dset));
  dset.dl_tensor.data = (void*)base;
  dset.dl_tensor.device.device_type = kDLCPU; dset.dl_tensor.device.device_id = 0;
  dset.dl_tensor.ndim = 2;
  dset.dl_tensor.dtype.code = kDLFloat; dset.dl_tensor.dtype.bits = 32; dset.dl_tensor.dtype.lanes = 1;
  dset.dl_tensor.shape = ds; dset.dl_tensor.strides = 0; dset.dl_tensor.byte_offset = 0;
  h->n = n; h->dim = dim;

  {
    const char* mode = getenv("CAGRA_BUILD_MODE");
    if (mode == NULL || mode[0] == '\0') mode = "auto";
    int done = 0;
    if (strcmp(mode, "legacy") != 0) {
      /* auto and nd both try the standalone NN-Descent pipeline first */
      if (build_nd(h, &dset, dim) == 0) { done = 1; }
      else if (strcmp(mode, "nd") == 0) {
        fprintf(stderr, "[seekdb_cuvs] nd build failed: %s\n", cuvsGetLastErrorText());
        goto fail_res;
      }
    }
    if (!done) {
      if (build_legacy(h, &dset, dim) != 0) goto fail_res;
    }
  }
  h->n = n; h->dim = dim;
  return h;

fail_res:
  if (h->idx) cuvsCagraIndexDestroy(h->idx);
  if (h->ip) cuvsCagraIndexParamsDestroy(h->ip);
  cuvsResourcesDestroy(h->res); free(h);
  fprintf(stderr, "[seekdb_cuvs] build failed: %s\n", cuvsGetLastErrorText());
  return NULL;
}

/* ------------------------------------------------------------------ */
/* search: persistent buffers + cached params                          */
/* ------------------------------------------------------------------ */

static void sk_free_search_resources(sk_cuvs_index* h)
{
  if (h->sp[0]) { cuvsCagraSearchParamsDestroy(h->sp[0]); h->sp[0] = 0; }
  if (h->sp[1]) { cuvsCagraSearchParamsDestroy(h->sp[1]); h->sp[1] = 0; }
  if (h->ddev)  { cuvsRMMFree(h->res, h->ddev, h->ddev_cap); h->ddev = 0; }
  if (h->ndev)  { cuvsRMMFree(h->res, h->ndev, h->ndev_cap); h->ndev = 0; }
  if (h->qd)    { cuvsRMMFree(h->res, h->qd, h->qd_cap); h->qd = 0; }
  if (h->hpinned) { sk_host_free(h->hpinned); h->hpinned = 0; }
  h->qd_cap = h->ndev_cap = h->ddev_cap = h->hpinned_cap = 0;
  h->sp_itopk[0] = h->sp_itopk[1] = 0;
}

static int sk_search_ensure(sk_cuvs_index* h, long nq, long topk, size_t itopk, size_t sw)
{
  const size_t q_bytes = sizeof(float) * (size_t)nq * (size_t)h->dim;
  const size_t k_bytes = (size_t)nq * (size_t)topk;
  if (q_bytes > h->qd_cap) {
    if (h->qd) cuvsRMMFree(h->res, h->qd, h->qd_cap);
    h->qd = 0; h->qd_cap = 0;
    if (cuvsRMMAlloc(h->res, (void**)&h->qd, q_bytes) != CUVS_SUCCESS) return 10;
    h->qd_cap = q_bytes;
    if (h->hpinned) { sk_host_free(h->hpinned); h->hpinned = 0; h->hpinned_cap = 0; }
    if (sk_host_alloc((void**)&h->hpinned, q_bytes) != 0) {
      h->hpinned = 0; /* pageable fallback: memcpy still works from caller ptr */
    } else {
      h->hpinned_cap = q_bytes;
    }
  }
  if (k_bytes * sizeof(unsigned) > h->ndev_cap) {
    if (h->ndev) cuvsRMMFree(h->res, h->ndev, h->ndev_cap);
    h->ndev = 0; h->ndev_cap = 0;
    if (cuvsRMMAlloc(h->res, (void**)&h->ndev, k_bytes * sizeof(unsigned)) != CUVS_SUCCESS) return 11;
    h->ndev_cap = k_bytes * sizeof(unsigned);
  }
  if (k_bytes * sizeof(float) > h->ddev_cap) {
    if (h->ddev) cuvsRMMFree(h->res, h->ddev, h->ddev_cap);
    h->ddev = 0; h->ddev_cap = 0;
    if (cuvsRMMAlloc(h->res, (void**)&h->ddev, k_bytes * sizeof(float)) != CUVS_SUCCESS) return 12;
    h->ddev_cap = k_bytes * sizeof(float);
  }
  /* cached search params, one slot per distinct itopk */
  {
    int slot = -1;
    for (int i = 0; i < SK_SP_CACHE; i++) {
      if (h->sp[i] && h->sp_itopk[i] == itopk) { slot = i; break; }
    }
    if (slot < 0) {
      slot = (h->sp[0] == 0) ? 0 : 1;   /* evict slot 1 */
      if (h->sp[slot]) { cuvsCagraSearchParamsDestroy(h->sp[slot]); h->sp[slot] = 0; }
      if (cuvsCagraSearchParamsCreate(&h->sp[slot]) != CUVS_SUCCESS) return 14;
      h->sp[slot]->itopk_size = itopk;
      h->sp[slot]->search_width = sw;
      h->sp_itopk[slot] = itopk;
    }
  }
  return 0;
}

static cuvsCagraSearchParams_t sk_search_params(sk_cuvs_index* h, size_t itopk)
{
  for (int i = 0; i < SK_SP_CACHE; i++) {
    if (h->sp[i] && h->sp_itopk[i] == itopk) return h->sp[i];
  }
  return 0;
}

int seekdb_cuvs_search(void* handle, const float* query, long nq, long topk,
                       unsigned int* out_ids, float* out_dist)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h || nq <= 0 || topk <= 0) return 1;
  int rc = 0;

  size_t itopk = env_size("CAGRA_ITOPK", 0);
  if (itopk == 0) {
    itopk = (size_t)topk * 4;
    if (itopk < 512) itopk = 512;
  }
  size_t sw = env_size("CAGRA_SW", 4);

  if ((rc = sk_search_ensure(h, nq, topk, itopk, sw)) != 0) goto done;
  {
    const size_t q_bytes = sizeof(float) * (size_t)nq * (size_t)h->dim;
    if (h->hpinned) {
      memcpy(h->hpinned, query, q_bytes);
      if (cudaMemcpy(h->qd, h->hpinned, q_bytes, cudaMemcpyHostToDevice) != cudaSuccess) { rc = 13; goto done; }
    } else {
      if (cudaMemcpy(h->qd, query, q_bytes, cudaMemcpyHostToDevice) != cudaSuccess) { rc = 13; goto done; }
    }
    {
      DLManagedTensor qt, nt, dt;
      long qs[2] = {nq, h->dim}, nsh[2] = {nq, topk}, dsh[2] = {nq, topk};
      memset(&qt, 0, sizeof(qt)); memset(&nt, 0, sizeof(nt)); memset(&dt, 0, sizeof(dt));
      qt.dl_tensor.data = h->qd;
      qt.dl_tensor.device.device_type = kDLCUDA; qt.dl_tensor.device.device_id = 0;
      qt.dl_tensor.ndim = 2;
      qt.dl_tensor.dtype.code = kDLFloat; qt.dl_tensor.dtype.bits = 32; qt.dl_tensor.dtype.lanes = 1;
      qt.dl_tensor.shape = qs; qt.dl_tensor.strides = 0; qt.dl_tensor.byte_offset = 0;
      nt = qt; nt.dl_tensor.data = h->ndev; nt.dl_tensor.dtype.code = kDLUInt; nt.dl_tensor.shape = nsh;
      dt = qt; dt.dl_tensor.data = h->ddev; dt.dl_tensor.dtype.code = kDLFloat; dt.dl_tensor.shape = dsh;
      cuvsCagraSearchParams_t sp = sk_search_params(h, itopk);
      if (!sp) { rc = 14; goto done; }
      cuvsFilter f; f.type = NO_FILTER; f.addr = (uintptr_t)0;
      if (cuvsCagraSearch(h->res, sp, h->idx, &qt, &nt, &dt, f) != CUVS_SUCCESS) { rc = 15; goto done; }
      if (cudaMemcpy(out_ids, h->ndev, sizeof(unsigned) * (size_t)nq * topk, cudaMemcpyDeviceToHost) != cudaSuccess) { rc = 16; goto done; }
      if (out_dist) cudaMemcpy(out_dist, h->ddev, sizeof(float) * (size_t)nq * topk, cudaMemcpyDeviceToHost);
    }
  }
done:
  if (rc) fprintf(stderr, "[seekdb_cuvs] search rc=%d: %s\n", rc, cuvsGetLastErrorText());
  return rc;
}

void seekdb_cuvs_free(void* handle)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return;
  sk_free_search_resources(h);
  if (h->idx) cuvsCagraIndexDestroy(h->idx);
  if (h->ip) cuvsCagraIndexParamsDestroy(h->ip);
  cuvsResourcesDestroy(h->res);
  free(h);
}

int seekdb_cuvs_serialize(void* handle, const char* filename)
{
  sk_cuvs_index* index = (sk_cuvs_index*)handle;
  if (!index || !filename) return 1;
  if (cuvsCagraSerialize(index->res, filename, index->idx, true) != CUVS_SUCCESS) {
    fprintf(stderr, "[seekdb_cuvs] serialize failed: %s\n", cuvsGetLastErrorText());
    return 1;
  }
  return 0;
}

void* seekdb_cuvs_deserialize(const char* filename, long n, long dim)
{
  if (!filename || n <= 0 || dim <= 0) return NULL;
  sk_cuvs_index* index = (sk_cuvs_index*)calloc(1, sizeof(*index));
  if (!index) return NULL;
  if (cuvsResourcesCreate(&index->res) != CUVS_SUCCESS) goto fail;
  if (cuvsCagraIndexParamsCreate(&index->ip) != CUVS_SUCCESS) goto fail_res;
  if (cuvsCagraIndexCreate(&index->idx) != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraDeserialize(index->res, filename, index->idx) != CUVS_SUCCESS) goto fail_idx;
  {
    int64_t actual_n = 0;
    int64_t actual_dim = 0;
    if (cuvsCagraIndexGetSize(index->idx, &actual_n) != CUVS_SUCCESS ||
        cuvsCagraIndexGetDims(index->idx, &actual_dim) != CUVS_SUCCESS ||
        actual_n != n || actual_dim != dim) {
      fprintf(stderr, "[seekdb_cuvs] snapshot shape mismatch: %ld x %ld, expected %ld x %ld\n",
              (long)actual_n, (long)actual_dim, n, dim);
      goto fail_idx;
    }
  }
  index->n = n;
  index->dim = dim;
  return index;
fail_idx:
  cuvsCagraIndexDestroy(index->idx);
fail_ip:
  cuvsCagraIndexParamsDestroy(index->ip);
fail_res:
  cuvsResourcesDestroy(index->res);
fail:
  fprintf(stderr, "[seekdb_cuvs] deserialize failed: %s\n", cuvsGetLastErrorText());
  free(index);
  return NULL;
}

int seekdb_cuvs_cagra_knn(const float* base, long n, long dim,
                          const float* query, long nq, long topk,
                          unsigned int* out_ids)
{
  void* h = seekdb_cuvs_build(base, n, dim);
  if (!h) return 10;
  int rc = seekdb_cuvs_search(h, query, nq, topk, out_ids, NULL);
  seekdb_cuvs_free(h);
  return rc;
}

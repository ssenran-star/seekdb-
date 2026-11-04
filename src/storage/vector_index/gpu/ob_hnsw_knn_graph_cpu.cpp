/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
#include <utility>
#include <vector>

#include "ob_hnsw_knn_graph_internal.h"
#include "ob_hnsw_parallel.h"
#include "ob_hnsw_rng.h"
#include "ob_hnsw_topk.h"

namespace oceanbase
{
namespace hnsw_gpu
{

namespace
{

constexpr int64_t IVF_MAX_SAMPLE = 4096;
constexpr int64_t IVF_MAX_NLIST = 4096;
constexpr int64_t IVF_KMEANS_ITERS = 10;

// Cost between a raw vector and a raw centroid, same "smaller is better" convention.
inline float raw_cost(MetricType metric,
                      const float *lhs,
                      float lhs_norm,
                      const float *rhs,
                      float rhs_norm,
                      int64_t dim)
{
  float cost = 0.0f;
  if (MetricType::L2 == metric) {
    cost = l2_sqr_distance(lhs, rhs, dim);
  } else {
    const float dot = dot_product(lhs, rhs, dim);
    if (MetricType::INNER_PRODUCT == metric) {
      cost = -dot;
    } else {
      const float denom = std::sqrt(lhs_norm * rhs_norm);
      cost = denom > 0.0f ? -dot / denom : 0.0f;
    }
  }
  return cost;
}

inline void store_row(KnnGraph &graph,
                      int64_t row,
                      const std::vector<std::pair<float, int32_t>> &sorted)
{
  int32_t *ids = graph.row(row);
  float *costs = graph.dist_row(row);
  const int64_t keep = std::min<int64_t>(static_cast<int64_t>(sorted.size()), graph.k);
  for (int64_t t = 0; t < keep; ++t) {
    ids[t] = sorted[static_cast<size_t>(t)].second;
    costs[t] = sorted[static_cast<size_t>(t)].first;
  }
}

// Deterministic stratified sample used to train the IVF centroids.
void pick_sample_ids(int64_t n, int64_t sample_n, std::vector<int64_t> &ids)
{
  ids.clear();
  if (sample_n >= n) {
    ids.reserve(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) {
      ids.push_back(i);
    }
  } else {
    const int64_t stride = std::max<int64_t>(1, n / sample_n);
    ids.reserve(static_cast<size_t>(sample_n));
    for (int64_t i = 0; i < n && static_cast<int64_t>(ids.size()) < sample_n; i += stride) {
      ids.push_back(i);
    }
  }
}

// ---------------------------------------------------------------------------
// cpu-exact: full scan reference. Only practical for small n; it is the ground truth
// used by the correctness tests and by recall evaluation.
// ---------------------------------------------------------------------------
class CpuExactEngine : public IKnnGraphEngine
{
public:
  const char *name() const override { return "cpu-exact"; }

  StatusCode build(const BaseVectors &base,
                   int64_t k,
                   const BuildConfig &cfg,
                   KnnGraph &graph,
                   std::string &error) override
  {
    StatusCode status = StatusCode::OK;
    if (base.n <= 0 || base.dim <= 0 || k <= 0 || nullptr == base.data) {
      error = "cpu-exact: invalid base vectors or k";
      status = StatusCode::INVALID_ARGUMENT;
    } else {
      graph.reset(base.n, k);
      const int64_t threads = cfg.threads > 0 ? cfg.threads : default_thread_count();
      parallel_for(base.n, threads, [&](int64_t row) {
        TopK heap;
        heap.init(k);
        for (int64_t cand = 0; cand < base.n; ++cand) {
          if (cand == row) {
            continue;
          }
          heap.push(base.dist(row, cand), static_cast<int32_t>(cand));
        }
        std::vector<std::pair<float, int32_t>> sorted;
        heap.flush(sorted);
        store_row(graph, row, sorted);
      });
    }
    return status;
  }
};

// ---------------------------------------------------------------------------
// cpu-ivf: k-means partitioning + probe. Scales to millions of rows on a laptop, which
// makes it usable as the CPU baseline the GPU engine is compared against.
// ---------------------------------------------------------------------------
class CpuIvfEngine : public IKnnGraphEngine
{
public:
  const char *name() const override { return "cpu-ivf"; }

  StatusCode build(const BaseVectors &base,
                   int64_t k,
                   const BuildConfig &cfg,
                   KnnGraph &graph,
                   std::string &error) override
  {
    StatusCode status = StatusCode::OK;
    if (base.n <= 0 || base.dim <= 0 || k <= 0 || nullptr == base.data) {
      error = "cpu-ivf: invalid base vectors or k";
      status = StatusCode::INVALID_ARGUMENT;
    } else {
      const int64_t n = base.n;
      const int64_t dim = base.dim;
      const int64_t threads = cfg.threads > 0 ? cfg.threads : default_thread_count();
      int64_t nlist = cfg.ivf_nlist > 0
                          ? cfg.ivf_nlist
                          : static_cast<int64_t>(
                                std::lround(4.0 * std::sqrt(static_cast<double>(n))));
      nlist = std::max<int64_t>(1, std::min<int64_t>(nlist, std::min<int64_t>(n, IVF_MAX_NLIST)));
      int64_t nprobe = cfg.ivf_nprobe > 0 ? cfg.ivf_nprobe : 8;
      nprobe = std::max<int64_t>(1, std::min<int64_t>(nprobe, nlist));

      std::vector<float> centroids(static_cast<size_t>(nlist * dim), 0.0f);
      std::vector<float> centroid_norms(static_cast<size_t>(nlist), 0.0f);
      std::vector<int32_t> assign(static_cast<size_t>(n), 0);
      std::vector<std::vector<int32_t>> lists(static_cast<size_t>(nlist));

      status = train_centroids(base, nlist, cfg.seed, threads, centroids, centroid_norms, error);
      if (StatusCode::OK == status) {
        status = assign_rows(base, centroids, centroid_norms, threads, assign, lists, error);
      }
      if (StatusCode::OK == status) {
        graph.reset(n, k);
        parallel_for(n, threads, [&](int64_t row) {
          TopK centroid_heap;
          centroid_heap.init(nprobe);
          const float *query = base.at(row);
          const float query_norm = base.norm_of(row);
          for (int64_t c = 0; c < nlist; ++c) {
            centroid_heap.push(raw_cost(base.metric,
                                        query,
                                        query_norm,
                                        centroids.data() + c * dim,
                                        centroid_norms[static_cast<size_t>(c)],
                                        dim),
                               static_cast<int32_t>(c));
          }
          std::vector<std::pair<float, int32_t>> probed;
          centroid_heap.flush(probed);

          TopK heap;
          heap.init(k);
          for (size_t p = 0; p < probed.size(); ++p) {
            const std::vector<int32_t> &bucket = lists[static_cast<size_t>(probed[p].second)];
            for (size_t e = 0; e < bucket.size(); ++e) {
              const int64_t cand = bucket[e];
              if (cand == row) {
                continue;
              }
              heap.push(base.dist(row, cand), static_cast<int32_t>(cand));
            }
          }
          std::vector<std::pair<float, int32_t>> sorted;
          heap.flush(sorted);
          store_row(graph, row, sorted);
        });
      }
    }
    return status;
  }

private:
  static StatusCode train_centroids(const BaseVectors &base,
                                    int64_t nlist,
                                    int64_t seed,
                                    int64_t threads,
                                    std::vector<float> &centroids,
                                    std::vector<float> &centroid_norms,
                                    std::string &error)
  {
    StatusCode status = StatusCode::OK;
    const int64_t dim = base.dim;
    int64_t sample_n = std::min<int64_t>(base.n, std::max<int64_t>(2 * nlist, 1024));
    sample_n = std::min<int64_t>(sample_n, IVF_MAX_SAMPLE);
    std::vector<int64_t> sample_ids;
    pick_sample_ids(base.n, sample_n, sample_ids);
    const int64_t actual_sample = static_cast<int64_t>(sample_ids.size());
    if (0 == actual_sample) {
      error = "cpu-ivf: empty training sample";
      status = StatusCode::INTERNAL_ERROR;
    } else {
      // Deterministic seed selection: walk the sample with a fixed stride starting from a
      // pseudo random offset. k-means++ was avoided on purpose, its O(nlist * sample * dim)
      // sequential pass dominates the whole build for large nlist.
      const int64_t stride = std::max<int64_t>(1, actual_sample / nlist);
      int64_t cursor = static_cast<int64_t>(
                           uniform01(static_cast<uint64_t>(seed), 17) *
                           static_cast<double>(actual_sample)) %
                       actual_sample;
      for (int64_t c = 0; c < nlist; ++c) {
        const int64_t picked =
            sample_ids[static_cast<size_t>((cursor + c * stride) % actual_sample)];
        memcpy(centroids.data() + c * dim, base.at(picked), sizeof(float) * static_cast<size_t>(dim));
      }

      std::vector<int32_t> nearest(static_cast<size_t>(actual_sample), 0);
      std::vector<float> sums(static_cast<size_t>(nlist * dim), 0.0f);
      std::vector<int32_t> counts(static_cast<size_t>(nlist), 0);
      for (int64_t iter = 0; iter < IVF_KMEANS_ITERS; ++iter) {
        parallel_for(actual_sample, threads, [&](int64_t s) {
          const int64_t id = sample_ids[static_cast<size_t>(s)];
          const float *vec = base.at(id);
          const float vec_norm = base.norm_of(id);
          float best = 3.402823466e+38F;
          int32_t best_id = 0;
          for (int64_t c = 0; c < nlist; ++c) {
            const float cost = raw_cost(base.metric,
                                        vec,
                                        vec_norm,
                                        centroids.data() + c * dim,
                                        centroid_norms[static_cast<size_t>(c)],
                                        dim);
            if (cost < best) {
              best = cost;
              best_id = static_cast<int32_t>(c);
            }
          }
          nearest[static_cast<size_t>(s)] = best_id;
        });

        std::fill(sums.begin(), sums.end(), 0.0f);
        std::fill(counts.begin(), counts.end(), 0);
        for (int64_t s = 0; s < actual_sample; ++s) {
          const int32_t c = nearest[static_cast<size_t>(s)];
          const float *vec = base.at(sample_ids[static_cast<size_t>(s)]);
          float *acc = sums.data() + static_cast<int64_t>(c) * dim;
          for (int64_t d = 0; d < dim; ++d) {
            acc[d] += vec[d];
          }
          ++counts[static_cast<size_t>(c)];
        }
        for (int64_t c = 0; c < nlist; ++c) {
          const int32_t count = counts[static_cast<size_t>(c)];
          if (count > 0) {
            float *centroid = centroids.data() + c * dim;
            const float inv = 1.0f / static_cast<float>(count);
            for (int64_t d = 0; d < dim; ++d) {
              centroid[d] = sums[static_cast<size_t>(c * dim + d)] * inv;
            }
          }
        }
        for (int64_t c = 0; c < nlist; ++c) {
          centroid_norms[static_cast<size_t>(c)] =
              squared_norm(centroids.data() + c * dim, dim);
        }
      }
    }
    return status;
  }

  static StatusCode assign_rows(const BaseVectors &base,
                                const std::vector<float> &centroids,
                                const std::vector<float> &centroid_norms,
                                int64_t threads,
                                std::vector<int32_t> &assign,
                                std::vector<std::vector<int32_t>> &lists,
                                std::string &error)
  {
    StatusCode status = StatusCode::OK;
    const int64_t n = base.n;
    const int64_t dim = base.dim;
    const int64_t nlist = static_cast<int64_t>(centroids.size() / static_cast<size_t>(dim));
    if (nlist <= 0) {
      error = "cpu-ivf: zero centroids";
      status = StatusCode::INTERNAL_ERROR;
    } else {
      parallel_for(n, threads, [&](int64_t row) {
        const float *vec = base.at(row);
        const float vec_norm = base.norm_of(row);
        float best = 3.402823466e+38F;
        int32_t best_id = 0;
        for (int64_t c = 0; c < nlist; ++c) {
          const float cost = raw_cost(base.metric,
                                      vec,
                                      vec_norm,
                                      centroids.data() + c * dim,
                                      centroid_norms[static_cast<size_t>(c)],
                                      dim);
          if (cost < best) {
            best = cost;
            best_id = static_cast<int32_t>(c);
          }
        }
        assign[static_cast<size_t>(row)] = best_id;
      });

      std::vector<int64_t> offsets(static_cast<size_t>(nlist) + 1, 0);
      for (int64_t row = 0; row < n; ++row) {
        ++offsets[static_cast<size_t>(assign[static_cast<size_t>(row)] + 1)];
      }
      for (int64_t c = 0; c < nlist; ++c) {
        offsets[static_cast<size_t>(c + 1)] += offsets[static_cast<size_t>(c)];
      }
      std::vector<int32_t> cursor(offsets.begin(), offsets.end() - 1);
      std::vector<int32_t> flat(static_cast<size_t>(n), 0);
      for (int64_t row = 0; row < n; ++row) {
        const int32_t c = assign[static_cast<size_t>(row)];
        flat[static_cast<size_t>(cursor[static_cast<size_t>(c)]++)] = static_cast<int32_t>(row);
      }
      for (int64_t c = 0; c < nlist; ++c) {
        const int64_t begin = offsets[static_cast<size_t>(c)];
        const int64_t end = offsets[static_cast<size_t>(c + 1)];
        lists[static_cast<size_t>(c)].assign(flat.begin() + begin, flat.begin() + end);
      }
    }
    return status;
  }
};

} // namespace

IKnnGraphEngine *create_cpu_exact_engine() { return new (std::nothrow) CpuExactEngine(); }

IKnnGraphEngine *create_cpu_ivf_engine() { return new (std::nothrow) CpuIvfEngine(); }

} // namespace hnsw_gpu
} // namespace oceanbase

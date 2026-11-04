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

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ob_hnsw_distance.h"
#include "ob_hnsw_gpu_types.h"
#include "ob_hnsw_knn_graph.h"

namespace oceanbase
{
namespace hnsw_gpu
{

// Multi layer proximity graph produced by the builder. Layout:
//   level 0  : dense, n * m0 slots, -1 padded
//   level l>0: only nodes whose level >= l take part, m slots each, stored per node in
//              `upper[node]` at [(l-1)*m, l*m)
// This is the same topology vsag / hnswlib maintain; only the construction order
// differs -- links come from a batch k-NN graph instead of n online searches.
struct HnswGraph
{
  int64_t n = 0;
  int64_t dim = 0;
  int32_t m = 16;
  int32_t m0 = 32;
  int32_t max_level = 0;
  MetricType metric = MetricType::L2;
  int32_t entry_point = 0;
  std::vector<int32_t> levels;
  std::vector<int32_t> level0;
  std::vector<std::vector<int32_t>> upper;

  void init(int64_t row_count,
            int64_t dimension,
            int32_t degree,
            int32_t degree0,
            MetricType metric_type)
  {
    n = row_count;
    dim = dimension;
    m = degree;
    m0 = degree0;
    max_level = 0;
    metric = metric_type;
    entry_point = 0;
    levels.assign(static_cast<size_t>(n), 0);
    level0.assign(static_cast<size_t>(n * m0), -1);
    upper.assign(static_cast<size_t>(n), std::vector<int32_t>());
  }

  int32_t *level0_row(int64_t id) { return level0.data() + id * m0; }
  const int32_t *level0_row(int64_t id) const { return level0.data() + id * m0; }

  int32_t *upper_row(int64_t id, int32_t level)
  {
    return upper[static_cast<size_t>(id)].data() + static_cast<int64_t>(level - 1) * m;
  }
  const int32_t *upper_row(int64_t id, int32_t level) const
  {
    return upper[static_cast<size_t>(id)].data() + static_cast<int64_t>(level - 1) * m;
  }

  int32_t *row(int64_t id, int32_t level)
  {
    return 0 == level ? level0_row(id) : upper_row(id, level);
  }
  const int32_t *row(int64_t id, int32_t level) const
  {
    return 0 == level ? level0_row(id) : upper_row(id, level);
  }

  int64_t slot_count(int32_t level) const { return 0 == level ? m0 : m; }

  int64_t degree(int64_t id, int32_t level) const
  {
    int64_t count = 0;
    const int32_t *links = row(id, level);
    const int64_t cap = slot_count(level);
    while (count < cap && links[count] >= 0) {
      ++count;
    }
    return count;
  }

  int64_t edge_count() const
  {
    int64_t edges = 0;
    for (int64_t id = 0; id < n; ++id) {
      edges += degree(id, 0);
      for (int32_t level = 1; level <= levels[static_cast<size_t>(id)]; ++level) {
        edges += degree(id, level);
      }
    }
    return edges;
  }
};

// Turns the candidate graph into the layered graph. Deterministic for a given
// (knn graph, config): level assignment is splitmix64 over (seed, node) so the same
// input always produces a byte identical graph, even across thread counts.
StatusCode build_layered_graph(const BaseVectors &base,
                               const KnnGraph &knn,
                               const BuildConfig &cfg,
                               HnswGraph &graph,
                               std::string &error);

struct SearchConfig
{
  int64_t ef_search = 64;
  int64_t topk = 10;
};

void search_graph(const BaseVectors &base,
                  const HnswGraph &graph,
                  const float *query,
                  float query_norm,
                  const SearchConfig &cfg,
                  std::vector<int32_t> &ids,
                  std::vector<float> &costs);

// Mean |result ∩ truth| / topk over the supplied queries.
double recall_at_k(const std::vector<std::vector<int32_t>> &results,
                   const std::vector<std::vector<int32_t>> &truth);

} // namespace hnsw_gpu
} // namespace oceanbase

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

#include <string>
#include <vector>

#include "ob_hnsw_distance.h"
#include "ob_hnsw_gpu_types.h"

namespace oceanbase
{
namespace hnsw_gpu
{

// Row major k-NN graph: n rows of k entries ordered by increasing cost.
// Unused slots stay -1 / FLT_MAX so consumers can simply iterate.
struct KnnGraph
{
  int64_t n = 0;
  int64_t k = 0;
  std::vector<int32_t> neighbors;
  std::vector<float> distances;

  void reset(int64_t row_count, int64_t width)
  {
    n = row_count;
    k = width;
    neighbors.assign(static_cast<size_t>(n * k), -1);
    distances.assign(static_cast<size_t>(n * k), 3.402823466e+38F);
  }

  int32_t *row(int64_t i) { return neighbors.data() + i * k; }
  const int32_t *row(int64_t i) const { return neighbors.data() + i * k; }
  float *dist_row(int64_t i) { return distances.data() + i * k; }
  const float *dist_row(int64_t i) const { return distances.data() + i * k; }

  int64_t valid_count(int64_t i) const
  {
    int64_t count = 0;
    const int32_t *ids = row(i);
    while (count < k && ids[count] >= 0) {
      ++count;
    }
    return count;
  }
};

// The k-NN stage is the only O(n^2)-ish part of the build and the only one a GPU
// accelerates. Everything downstream (layer assembly, pruning) is O(n * m0^2) and stays
// on the host even when the GPU engine is selected.
class IKnnGraphEngine
{
public:
  virtual ~IKnnGraphEngine() = default;
  virtual const char *name() const = 0;
  virtual StatusCode build(const BaseVectors &base,
                           int64_t k,
                           const BuildConfig &cfg,
                           KnnGraph &graph,
                           std::string &error) = 0;
  virtual bool available(std::string &reason) const
  {
    reason.clear();
    return true;
  }
};

// Returns nullptr for an unknown engine name.
IKnnGraphEngine *create_knn_engine(const char *name);
// "cpu-exact,cpu-ivf,hipvs"
const char *knn_engine_names();
bool hipvs_compiled_in();

// Exact top-k of one external query vector, used for recall evaluation.
void exact_search(const BaseVectors &base,
                  const float *query,
                  float query_norm,
                  int64_t topk,
                  std::vector<int32_t> &ids,
                  std::vector<float> &costs);

} // namespace hnsw_gpu
} // namespace oceanbase

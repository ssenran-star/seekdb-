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

#include "ob_hnsw_knn_graph.h"

#include <chrono>
#include <cstring>

#include "ob_hnsw_knn_graph_internal.h"
#include "ob_hnsw_topk.h"

namespace oceanbase
{
namespace hnsw_gpu
{

int64_t now_us()
{
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::microseconds>(now).count();
}

IKnnGraphEngine *create_knn_engine(const char *name)
{
  IKnnGraphEngine *engine = nullptr;
  if (nullptr != name) {
    if (0 == strcmp(name, "cpu-exact")) {
      engine = create_cpu_exact_engine();
    } else if (0 == strcmp(name, "cpu-ivf")) {
      engine = create_cpu_ivf_engine();
    } else if (0 == strcmp(name, "hipvs")) {
      engine = create_hipvs_engine();
    }
  }
  return engine;
}

const char *knn_engine_names() { return "cpu-exact,cpu-ivf,hipvs"; }

bool hipvs_compiled_in()
{
  bool compiled = false;
#ifdef SEEKDB_ENABLE_HIPVS
  compiled = true;
#endif
  return compiled;
}

void exact_search(const BaseVectors &base,
                  const float *query,
                  float query_norm,
                  int64_t topk,
                  std::vector<int32_t> &ids,
                  std::vector<float> &costs)
{
  ids.clear();
  costs.clear();
  if (nullptr == query || base.n <= 0 || topk <= 0) {
    return;
  }
  TopK heap;
  heap.init(topk);
  for (int64_t i = 0; i < base.n; ++i) {
    heap.push(base.dist_query(query, query_norm, i), static_cast<int32_t>(i));
  }
  std::vector<std::pair<float, int32_t>> sorted;
  heap.flush(sorted);
  ids.reserve(sorted.size());
  costs.reserve(sorted.size());
  for (size_t i = 0; i < sorted.size(); ++i) {
    costs.push_back(sorted[i].first);
    ids.push_back(sorted[i].second);
  }
}

} // namespace hnsw_gpu
} // namespace oceanbase

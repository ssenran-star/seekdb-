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

#include "ob_hnsw_gpu_types.h"
#include "ob_hnsw_knn_graph.h"
#include "ob_hnsw_layered_graph.h"

namespace oceanbase
{
namespace hnsw_gpu
{

// Builds a full HNSW graph from a dense float32 matrix.
//
// The work is split in two stages on purpose:
//   stage 1 (engine)  : k-NN candidate graph, O(n^2)-ish, GPU friendly
//   stage 2 (host)    : layer assembly + heuristic pruning, O(n * m0^2), cheap
// Only stage 1 is replaced by hipVS, which keeps the graph topology byte identical to
// the CPU path for the same candidate graph (and therefore keeps recall comparable).
StatusCode build_index(const float *vectors,
                       int64_t n,
                       int64_t dim,
                       MetricType metric,
                       const BuildConfig &cfg,
                       IKnnGraphEngine *engine,
                       HnswGraph &graph,
                       BuildStats &stats,
                       std::string &error);

// Same as above, resolving the engine from its name ("cpu-exact" / "cpu-ivf" /
// "hipvs"). When the requested engine is not usable the call fails instead of
// silently falling back, so that a benchmark can never report GPU numbers it did not
// actually produce.
StatusCode build_index(const float *vectors,
                       int64_t n,
                       int64_t dim,
                       MetricType metric,
                       const BuildConfig &cfg,
                       const char *engine_name,
                       HnswGraph &graph,
                       BuildStats &stats,
                       std::string &error);

} // namespace hnsw_gpu
} // namespace oceanbase

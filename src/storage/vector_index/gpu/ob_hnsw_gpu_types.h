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

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

namespace oceanbase
{
namespace hnsw_gpu
{

// Distance semantics used across the module: for every metric the stored value is a
// "cost" -- the smaller the value, the closer the two vectors are.
//   L2            -> squared euclidean distance
//   INNER_PRODUCT -> negative dot product
//   COSINE        -> negative cosine similarity
enum class MetricType : int32_t
{
  L2 = 0,
  INNER_PRODUCT = 1,
  COSINE = 2,
  MAX_METRIC_TYPE
};

enum class StatusCode : int32_t
{
  OK = 0,
  INVALID_ARGUMENT = -1,
  OUT_OF_MEMORY = -2,
  GPU_ERROR = -3,
  IO_ERROR = -4,
  NOT_SUPPORTED = -5,
  INTERNAL_ERROR = -6
};

inline const char *status_name(StatusCode code)
{
  switch (code) {
    case StatusCode::OK: return "OK";
    case StatusCode::INVALID_ARGUMENT: return "INVALID_ARGUMENT";
    case StatusCode::OUT_OF_MEMORY: return "OUT_OF_MEMORY";
    case StatusCode::GPU_ERROR: return "GPU_ERROR";
    case StatusCode::IO_ERROR: return "IO_ERROR";
    case StatusCode::NOT_SUPPORTED: return "NOT_SUPPORTED";
    case StatusCode::INTERNAL_ERROR: return "INTERNAL_ERROR";
    default: return "UNKNOWN";
  }
}

inline const char *metric_name(MetricType metric)
{
  switch (metric) {
    case MetricType::L2: return "l2";
    case MetricType::INNER_PRODUCT: return "ip";
    case MetricType::COSINE: return "cosine";
    default: return "unknown";
  }
}

inline bool parse_metric(const char *text, MetricType &metric)
{
  bool ok = true;
  if (nullptr == text) {
    ok = false;
  } else if (0 == strcmp(text, "l2") || 0 == strcmp(text, "L2")) {
    metric = MetricType::L2;
  } else if (0 == strcmp(text, "ip") || 0 == strcmp(text, "IP")) {
    metric = MetricType::INNER_PRODUCT;
  } else if (0 == strcmp(text, "cosine") || 0 == strcmp(text, "COSINE")) {
    metric = MetricType::COSINE;
  } else {
    ok = false;
  }
  return ok;
}

// Parameters that describe one HNSW index. A plain struct on purpose: the module can
// be compiled and exercised without the seekdb runtime or its config system.
struct BuildConfig
{
  int64_t dim = 0;
  int64_t m = 16;                    // max out-degree for level > 0
  int64_t ef_construction = 100;     // kept for parity with vsag / ann-benchmarks
  MetricType metric = MetricType::L2;
  int64_t knn_k = 0;                 // 0 => derived from m
  int64_t seed = 42;                 // level assignment and sampling
  int64_t threads = 0;               // 0 => hardware_concurrency
  int64_t gpu_block_rows = 1 << 20;  // rows per host<->device wave, bounds peak memory
  int64_t ivf_nlist = 0;             // 0 => derived, cpu-ivf only
  int64_t ivf_nprobe = 8;            // cpu-ivf only

  int64_t m0() const { return 2 * m; }

  // The candidate list must be a superset of the out-degree, otherwise the neighbour
  // selection heuristic has nothing to prune from.
  int64_t effective_knn_k() const
  {
    int64_t k = knn_k > 0 ? knn_k : 2 * m0();
    if (k < m0()) {
      k = m0();
    }
    return k;
  }
};

struct BuildStats
{
  int64_t n = 0;
  int64_t dim = 0;
  int64_t knn_k = 0;
  int64_t knn_stage_us = 0;
  int64_t layer_stage_us = 0;
  int64_t total_us = 0;
  int64_t max_level = 0;
  int64_t edge_count = 0;
  const char *engine = "";
};

// Monotonic clock in microseconds. Declared here, defined in ob_hnsw_knn_graph.cpp so
// that the headers stay free of <chrono>.
int64_t now_us();

} // namespace hnsw_gpu
} // namespace oceanbase

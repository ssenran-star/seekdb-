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

#include "ob_hnsw_gpu_builder.h"

#include <memory>

namespace oceanbase
{
namespace hnsw_gpu
{

StatusCode build_index(const float *vectors,
                       int64_t n,
                       int64_t dim,
                       MetricType metric,
                       const BuildConfig &cfg,
                       IKnnGraphEngine *engine,
                       HnswGraph &graph,
                       BuildStats &stats,
                       std::string &error)
{
  StatusCode status = StatusCode::OK;
  if (nullptr == vectors || n <= 0 || dim <= 0) {
    error = "build_index: invalid input matrix";
    status = StatusCode::INVALID_ARGUMENT;
  } else if (nullptr == engine) {
    error = "build_index: no k-NN engine";
    status = StatusCode::INVALID_ARGUMENT;
  } else if (cfg.m <= 0) {
    error = "build_index: m must be positive";
    status = StatusCode::INVALID_ARGUMENT;
  } else {
    BuildConfig effective = cfg;
    effective.dim = dim;
    const int64_t knn_k = effective.effective_knn_k();
    if (knn_k >= n) {
      error = "build_index: k-NN width must be smaller than the row count";
      status = StatusCode::INVALID_ARGUMENT;
    } else {
      BaseVectors base;
      base.bind(vectors, n, dim, metric);

      KnnGraph knn;
      const int64_t begin_us = now_us();
      status = engine->build(base, knn_k, effective, knn, error);
      const int64_t knn_us = now_us() - begin_us;

      if (StatusCode::OK == status) {
        const int64_t layer_begin_us = now_us();
        status = build_layered_graph(base, knn, effective, graph, error);
        const int64_t layer_us = now_us() - layer_begin_us;

        stats.n = n;
        stats.dim = dim;
        stats.knn_k = knn_k;
        stats.knn_stage_us = knn_us;
        stats.layer_stage_us = layer_us;
        stats.total_us = knn_us + layer_us;
        stats.max_level = graph.max_level;
        stats.edge_count = graph.edge_count();
        stats.engine = engine->name();
      }
    }
  }
  return status;
}

StatusCode build_index(const float *vectors,
                       int64_t n,
                       int64_t dim,
                       MetricType metric,
                       const BuildConfig &cfg,
                       const char *engine_name,
                       HnswGraph &graph,
                       BuildStats &stats,
                       std::string &error)
{
  StatusCode status = StatusCode::OK;
  std::unique_ptr<IKnnGraphEngine> engine(create_knn_engine(engine_name));
  if (nullptr == engine.get()) {
    error = std::string("build_index: unknown engine '") +
            (nullptr == engine_name ? "" : engine_name) + "', expected one of " +
            knn_engine_names();
    status = StatusCode::INVALID_ARGUMENT;
  } else {
    std::string reason;
    if (!engine->available(reason)) {
      error = reason;
      status = StatusCode::NOT_SUPPORTED;
    } else {
      status = build_index(vectors, n, dim, metric, cfg, engine.get(), graph, stats, error);
    }
  }
  return status;
}

} // namespace hnsw_gpu
} // namespace oceanbase

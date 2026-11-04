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

// hipVS backend for the k-NN stage.
//
// hipVS is a HIP port of RAPIDS cuVS and keeps the cuVS C++ API, the `cuvs` namespace
// and the CMake target names. The code below is therefore written against the cuVS API
// and builds unchanged against either library; only this translation unit needs ROCm.
//
// Build requirements (see docs/competition/amd_hipvs_hnsw.md):
//   * Linux + ROCm >= 6.4 and hipVS / hipRAFT installed
//   * -DSEEKDB_ENABLE_HIPVS=ON -DHIPVS_ROOT=/opt/rocm

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "ob_hnsw_knn_graph_internal.h"

#if defined(SEEKDB_ENABLE_HIPVS)

#include <raft/core/device_mdarray.hpp>
#include <raft/core/device_resources.hpp>
#include <raft/core/host_mdarray.hpp>
#include <raft/core/resource/cuda_stream.hpp>

#include <cuvs/distance/distance.hpp>
#include <cuvs/neighbors/cagra.hpp>

namespace oceanbase
{
namespace hnsw_gpu
{

namespace
{

constexpr int64_t MAX_GRAPH_DEGREE = 512;

cuvs::distance::DistanceType to_cuvs_metric(MetricType metric)
{
  // COSINE is folded into InnerProduct over L2 normalised vectors, done on the host
  // before upload so the graph ordering matches the CPU engines exactly.
  return MetricType::INNER_PRODUCT == metric || MetricType::COSINE == metric
             ? cuvs::distance::DistanceType::InnerProduct
             : cuvs::distance::DistanceType::L2Expanded;
}

// Builds a CAGRA graph on the GPU and returns, for every base vector, its k nearest
// neighbours. This is the stage that dominates HNSW build time.
class HipVsCagraEngine : public IKnnGraphEngine
{
public:
  const char *name() const override { return "hipvs"; }

  bool available(std::string &reason) const override
  {
    bool ok = true;
    try {
      raft::device_resources probe;
      const int device_count = static_cast<int>(raft::resource::get_device_count(probe));
      if (device_count <= 0) {
        ok = false;
        reason = "hipvs: no HIP device visible";
      }
    } catch (const std::exception &e) {
      ok = false;
      reason = std::string("hipvs: HIP runtime unavailable: ") + e.what();
    }
    return ok;
  }

  StatusCode build(const BaseVectors &base,
                   int64_t k,
                   const BuildConfig &cfg,
                   KnnGraph &graph,
                   std::string &error) override
  {
    StatusCode status = StatusCode::OK;
    if (base.n <= 0 || base.dim <= 0 || k <= 0 || nullptr == base.data) {
      error = "hipvs: invalid base vectors or k";
      status = StatusCode::INVALID_ARGUMENT;
    } else if (k > MAX_GRAPH_DEGREE) {
      error = "hipvs: k exceeds the supported CAGRA graph degree";
      status = StatusCode::INVALID_ARGUMENT;
    } else {
      std::vector<float> normalised;
      const float *source = base.data;
      status = prepare_input(base, normalised, source, error);
      if (StatusCode::OK == status) {
        status = run_cagra(base, source, k, cfg, graph, error);
      }
    }
    return status;
  }

private:
  // COSINE needs unit length vectors so that the search reduces to inner product; the
  // normalisation happens once here instead of per query on the device.
  StatusCode prepare_input(const BaseVectors &base,
                           std::vector<float> &normalised,
                           const float *&source,
                           std::string &error)
  {
    StatusCode status = StatusCode::OK;
    source = base.data;
    if (MetricType::COSINE == base.metric) {
      try {
        normalised.resize(static_cast<size_t>(base.n * base.dim));
        for (int64_t i = 0; i < base.n; ++i) {
          const float *src = base.data + i * base.dim;
          float *dst = normalised.data() + i * base.dim;
          const float norm = std::sqrt(std::max(0.0f, squared_norm(src, base.dim)));
          const float scale = norm > 0.0f ? 1.0f / norm : 0.0f;
          for (int64_t d = 0; d < base.dim; ++d) {
            dst[d] = src[d] * scale;
          }
        }
        source = normalised.data();
      } catch (const std::bad_alloc &) {
        error = "hipvs: out of host memory while normalising";
        status = StatusCode::OUT_OF_MEMORY;
      }
    }
    return status;
  }

  StatusCode run_cagra(const BaseVectors &base,
                       const float *source,
                       int64_t k,
                       const BuildConfig &cfg,
                       KnnGraph &graph,
                       std::string &error)
  {
    StatusCode status = StatusCode::OK;
    const int64_t n = base.n;
    const int64_t dim = base.dim;
    try {
      raft::device_resources res;
      const cudaStream_t stream = raft::resource::get_cuda_stream(res);

      // The dataset must stay resident for the whole build: n * dim * 4 bytes plus the
      // CAGRA graph itself (n * degree * 4 bytes).
      auto dataset = raft::make_device_matrix<float, int64_t>(res, n, dim);
      raft::copy(dataset.data_handle(), source, n * dim, stream);

      cuvs::neighbors::cagra::index_params index_params;
      index_params.metric = to_cuvs_metric(base.metric);
      index_params.graph_degree = static_cast<uint32_t>(k);
      index_params.intermediate_graph_degree = static_cast<uint32_t>(
          std::min<int64_t>(std::max<int64_t>(k * 3 / 2, k + 1), MAX_GRAPH_DEGREE));
      index_params.attach_dataset_on_build = true;

      auto index = cuvs::neighbors::cagra::build(
          res, index_params, raft::make_const_mdspan(dataset.view()));

      cuvs::neighbors::cagra::search_params search_params;
      search_params.itopk_size = static_cast<uint32_t>(std::max<int64_t>(64, k * 4));
      search_params.search_width = static_cast<uint32_t>(std::max<int64_t>(1, k));

      // Base vectors double as queries: one GPU k-NN scan replaces n CPU traversals.
      // Results are drained in waves so peak host memory stays bounded by
      // gpu_block_rows * k.
      int64_t block_rows = cfg.gpu_block_rows > 0 ? cfg.gpu_block_rows : n;
      block_rows = std::min<int64_t>(block_rows, n);
      graph.reset(n, k);

      auto neighbors = raft::make_device_matrix<uint32_t, int64_t>(res, block_rows, k);
      auto distances = raft::make_device_matrix<float, int64_t>(res, block_rows, k);

      std::vector<uint32_t> host_ids(static_cast<size_t>(block_rows * k), 0);
      std::vector<float> host_costs(static_cast<size_t>(block_rows * k), 0.0f);

      for (int64_t begin = 0; begin < n; begin += block_rows) {
        const int64_t rows = std::min<int64_t>(block_rows, n - begin);
        auto queries = raft::make_device_matrix_view<const float, int64_t>(
            dataset.data_handle() + begin * dim, rows, dim);
        auto neighbors_view =
            raft::make_device_matrix_view<uint32_t, int64_t>(neighbors.data_handle(), rows, k);
        auto distances_view =
            raft::make_device_matrix_view<float, int64_t>(distances.data_handle(), rows, k);

        cuvs::neighbors::cagra::search(
            res, search_params, index, queries, neighbors_view, distances_view);
        raft::resource::sync_stream(res, stream);

        raft::copy(host_ids.data(), neighbors.data_handle(), rows * k, stream);
        raft::copy(host_costs.data(), distances.data_handle(), rows * k, stream);
        raft::resource::sync_stream(res, stream);

        for (int64_t r = 0; r < rows; ++r) {
          const int64_t global_row = begin + r;
          int32_t *ids = graph.row(global_row);
          float *costs = graph.dist_row(global_row);
          const uint32_t *src_ids = host_ids.data() + r * k;
          const float *src_costs = host_costs.data() + r * k;
          int64_t written = 0;
          for (int64_t c = 0; c < k; ++c) {
            const int64_t candidate = static_cast<int64_t>(src_ids[c]);
            if (candidate == global_row || candidate >= n) {
              continue;
            }
            ids[written] = static_cast<int32_t>(candidate);
            costs[written] = src_costs[c];
            ++written;
            if (written == k) {
              break;
            }
          }
        }
      }
    } catch (const std::bad_alloc &) {
      error = "hipvs: out of device/host memory while building the CAGRA graph";
      status = StatusCode::OUT_OF_MEMORY;
    } catch (const std::exception &e) {
      error = std::string("hipvs: ") + e.what();
      status = StatusCode::GPU_ERROR;
    }
    return status;
  }
};

} // namespace

IKnnGraphEngine *create_hipvs_engine() { return new (std::nothrow) HipVsCagraEngine(); }

} // namespace hnsw_gpu
} // namespace oceanbase

#else // SEEKDB_ENABLE_HIPVS

namespace oceanbase
{
namespace hnsw_gpu
{

namespace
{

// Stub used when the module is compiled without ROCm. It keeps the engine name
// resolvable so callers report a precise reason instead of crashing or silently
// falling back to the CPU.
class DisabledHipVsEngine : public IKnnGraphEngine
{
public:
  const char *name() const override { return "hipvs"; }

  bool available(std::string &reason) const override
  {
    reason = "hipvs: seekdb was built without SEEKDB_ENABLE_HIPVS";
    return false;
  }

  StatusCode build(const BaseVectors &,
                   int64_t,
                   const BuildConfig &,
                   KnnGraph &,
                   std::string &error) override
  {
    error = "hipvs: seekdb was built without SEEKDB_ENABLE_HIPVS";
    return StatusCode::NOT_SUPPORTED;
  }
};

} // namespace

IKnnGraphEngine *create_hipvs_engine() { return new (std::nothrow) DisabledHipVsEngine(); }

} // namespace hnsw_gpu
} // namespace oceanbase

#endif // SEEKDB_ENABLE_HIPVS

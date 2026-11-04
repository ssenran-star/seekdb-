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
#include <vector>

#include "ob_hnsw_gpu_types.h"

namespace oceanbase
{
namespace hnsw_gpu
{

// Squared euclidean distance, 4-way unrolled. The compiler auto-vectorizes this on
// AVX2 / NEON, which is what makes the CPU reference engines usable as a baseline.
inline float l2_sqr_distance(const float *lhs, const float *rhs, int64_t dim)
{
  float sum0 = 0.0f;
  float sum1 = 0.0f;
  float sum2 = 0.0f;
  float sum3 = 0.0f;
  int64_t i = 0;
  for (; i + 4 <= dim; i += 4) {
    const float d0 = lhs[i] - rhs[i];
    const float d1 = lhs[i + 1] - rhs[i + 1];
    const float d2 = lhs[i + 2] - rhs[i + 2];
    const float d3 = lhs[i + 3] - rhs[i + 3];
    sum0 += d0 * d0;
    sum1 += d1 * d1;
    sum2 += d2 * d2;
    sum3 += d3 * d3;
  }
  for (; i < dim; ++i) {
    const float d = lhs[i] - rhs[i];
    sum0 += d * d;
  }
  return (sum0 + sum1) + (sum2 + sum3);
}

inline float dot_product(const float *lhs, const float *rhs, int64_t dim)
{
  float sum0 = 0.0f;
  float sum1 = 0.0f;
  float sum2 = 0.0f;
  float sum3 = 0.0f;
  int64_t i = 0;
  for (; i + 4 <= dim; i += 4) {
    sum0 += lhs[i] * rhs[i];
    sum1 += lhs[i + 1] * rhs[i + 1];
    sum2 += lhs[i + 2] * rhs[i + 2];
    sum3 += lhs[i + 3] * rhs[i + 3];
  }
  for (; i < dim; ++i) {
    sum0 += lhs[i] * rhs[i];
  }
  return (sum0 + sum1) + (sum2 + sum3);
}

inline float squared_norm(const float *vec, int64_t dim) { return dot_product(vec, vec, dim); }

// Converts a raw similarity/distance into the module wide "smaller is better" cost.
inline float to_cost(MetricType metric, float l2_sqr, float dot, float lhs_norm, float rhs_norm)
{
  float cost = l2_sqr;
  if (MetricType::INNER_PRODUCT == metric) {
    cost = -dot;
  } else if (MetricType::COSINE == metric) {
    const float denom = std::sqrt(lhs_norm * rhs_norm);
    cost = denom > 0.0f ? -dot / denom : 0.0f;
  }
  return cost;
}

// Base matrix plus the per vector squared norms that COSINE needs, so that a scan only
// pays for one dot product per candidate.
struct BaseVectors
{
  const float *data = nullptr;
  int64_t n = 0;
  int64_t dim = 0;
  MetricType metric = MetricType::L2;
  std::vector<float> sq_norms;

  void bind(const float *base, int64_t count, int64_t dimension, MetricType metric_type)
  {
    data = base;
    n = count;
    dim = dimension;
    metric = metric_type;
    if (MetricType::COSINE == metric) {
      sq_norms.resize(static_cast<size_t>(n));
      for (int64_t i = 0; i < n; ++i) {
        sq_norms[static_cast<size_t>(i)] = squared_norm(data + i * dim, dim);
      }
    } else {
      sq_norms.clear();
    }
  }

  const float *at(int64_t id) const { return data + id * dim; }

  float norm_of(int64_t id) const
  {
    return MetricType::COSINE == metric ? sq_norms[static_cast<size_t>(id)] : 0.0f;
  }

  // Cost between two base vectors.
  float dist(int64_t lhs, int64_t rhs) const
  {
    const float *a = at(lhs);
    const float *b = at(rhs);
    if (MetricType::L2 == metric) {
      return l2_sqr_distance(a, b, dim);
    }
    const float dot = dot_product(a, b, dim);
    return to_cost(metric, 0.0f, dot, norm_of(lhs), norm_of(rhs));
  }

  // Cost between an external query vector and a base vector.
  float dist_query(const float *query, float query_norm, int64_t rhs) const
  {
    const float *b = at(rhs);
    if (MetricType::L2 == metric) {
      return l2_sqr_distance(query, b, dim);
    }
    const float dot = dot_product(query, b, dim);
    return to_cost(metric, 0.0f, dot, query_norm, norm_of(rhs));
  }
};

} // namespace hnsw_gpu
} // namespace oceanbase

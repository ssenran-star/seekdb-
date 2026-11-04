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

namespace oceanbase
{
namespace hnsw_gpu
{

// Deterministic, allocation free PRNG. Level assignment must not depend on thread
// scheduling or traversal order, otherwise the same input produces a different graph on
// every run and the index stops being reproducible.
inline uint64_t splitmix64(uint64_t x)
{
  x += 0x9E3779B97F4A7C15ULL;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
  return x ^ (x >> 31);
}

// Uniform in (0, 1), never returning exactly 0 so that log() stays finite.
inline double uniform01(uint64_t seed, uint64_t salt)
{
  const uint64_t bits = splitmix64(seed * 0x9E3779B97F4A7C15ULL + salt);
  const double unit = static_cast<double>(bits >> 11) * (1.0 / 9007199254740992.0);
  return unit <= 0.0 ? 1e-12 : unit;
}

// HNSW level assignment: level = floor(-ln(U) * (1 / ln(M))).
inline int32_t random_level(uint64_t seed, int64_t node_id, int64_t m, int32_t level_cap)
{
  const double ml = 1.0 / std::log(static_cast<double>(m > 1 ? m : 2));
  const double draw = uniform01(seed, static_cast<uint64_t>(node_id) + 1);
  double scaled = -std::log(draw) * ml;
  if (scaled < 0.0) {
    scaled = 0.0;
  }
  int32_t level = static_cast<int32_t>(scaled);
  if (level > level_cap) {
    level = level_cap;
  }
  return level;
}

} // namespace hnsw_gpu
} // namespace oceanbase

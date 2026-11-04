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

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace oceanbase
{
namespace hnsw_gpu
{

inline int64_t default_thread_count()
{
  const unsigned hw = std::thread::hardware_concurrency();
  return hw == 0 ? 1 : static_cast<int64_t>(hw);
}

// Minimal fork/join helper: `body(i)` runs for every index in [0, count).
// The build path only needs "spread rows over cores", so this stays tiny.
template <typename Body>
void parallel_for(int64_t count, int64_t threads, Body body)
{
  if (count <= 0) {
    return;
  }
  int64_t worker_count = threads > 0 ? threads : default_thread_count();
  if (worker_count > count) {
    worker_count = count;
  }
  if (worker_count <= 1) {
    for (int64_t i = 0; i < count; ++i) {
      body(i);
    }
    return;
  }
  std::atomic<int64_t> cursor(0);
  std::vector<std::thread> workers;
  workers.reserve(static_cast<size_t>(worker_count));
  for (int64_t t = 0; t < worker_count; ++t) {
    workers.emplace_back([&cursor, count, &body]() {
      for (;;) {
        const int64_t i = cursor.fetch_add(1, std::memory_order_relaxed);
        if (i >= count) {
          break;
        }
        body(i);
      }
    });
  }
  for (size_t t = 0; t < workers.size(); ++t) {
    workers[t].join();
  }
}

} // namespace hnsw_gpu
} // namespace oceanbase

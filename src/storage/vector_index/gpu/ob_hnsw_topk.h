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

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace oceanbase
{
namespace hnsw_gpu
{

// Bounded max-heap keeping the k cheapest (cost, id) pairs seen so far, where `cost`
// follows the module wide convention: smaller is closer.
class TopK
{
public:
  void init(int64_t capacity)
  {
    capacity_ = capacity;
    heap_.clear();
    heap_.reserve(static_cast<size_t>(capacity > 0 ? capacity : 0));
  }

  void clear() { heap_.clear(); }

  bool is_full() const { return static_cast<int64_t>(heap_.size()) >= capacity_; }
  int64_t size() const { return static_cast<int64_t>(heap_.size()); }
  float worst() const { return heap_.empty() ? 3.402823466e+38F : heap_.front().first; }

  inline void push(float cost, int32_t id)
  {
    if (static_cast<int64_t>(heap_.size()) < capacity_) {
      heap_.emplace_back(cost, id);
      std::push_heap(heap_.begin(), heap_.end(), comparator_);
    } else if (capacity_ > 0 && cost < heap_.front().first) {
      std::pop_heap(heap_.begin(), heap_.end(), comparator_);
      heap_.back() = std::make_pair(cost, id);
      std::push_heap(heap_.begin(), heap_.end(), comparator_);
    }
  }

  // Writes the retained pairs in ascending cost order, returns how many were kept.
  int64_t flush(std::vector<std::pair<float, int32_t>> &sorted) const
  {
    sorted.assign(heap_.begin(), heap_.end());
    std::sort(sorted.begin(), sorted.end(), comparator_);
    return static_cast<int64_t>(sorted.size());
  }

private:
  struct ByCost
  {
    bool operator()(const std::pair<float, int32_t> &lhs,
                    const std::pair<float, int32_t> &rhs) const
    {
      return lhs.first < rhs.first;
    }
  };

  int64_t capacity_ = 0;
  std::vector<std::pair<float, int32_t>> heap_;
  ByCost comparator_;
};

} // namespace hnsw_gpu
} // namespace oceanbase

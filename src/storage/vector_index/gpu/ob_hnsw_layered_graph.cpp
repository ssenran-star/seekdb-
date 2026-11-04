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

#include "ob_hnsw_layered_graph.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <queue>
#include <utility>
#include <vector>

#include "ob_hnsw_parallel.h"
#include "ob_hnsw_rng.h"

namespace oceanbase
{
namespace hnsw_gpu
{

namespace
{

constexpr int32_t LEVEL_CAP = 32;
// Upper layers hold n / m^level nodes; a bounded stride sample of that set is enough
// to seed the neighbour selection without turning the top layers into a hot spot.
constexpr int64_t UPPER_SAMPLE_SIZE = 128;

inline bool by_cost(const std::pair<float, int32_t> &lhs, const std::pair<float, int32_t> &rhs)
{
  return lhs.first < rhs.first;
}

// Sorts by id to drop duplicates (keeping the cheapest occurrence), then restores the
// cost order the selection heuristic expects.
void sort_unique_by_cost(std::vector<std::pair<float, int32_t>> &candidates)
{
  std::sort(candidates.begin(),
            candidates.end(),
            [](const std::pair<float, int32_t> &lhs, const std::pair<float, int32_t> &rhs) {
              if (lhs.second != rhs.second) {
                return lhs.second < rhs.second;
              }
              return lhs.first < rhs.first;
            });
  candidates.erase(std::unique(candidates.begin(),
                               candidates.end(),
                               [](const std::pair<float, int32_t> &lhs,
                                  const std::pair<float, int32_t> &rhs) {
                                 return lhs.second == rhs.second;
                               }),
                   candidates.end());
  std::sort(candidates.begin(), candidates.end(), by_cost);
}

// Neighbour selection heuristic from the HNSW paper (Algorithm 4, keepPruned = true):
// walk the candidates in increasing distance and keep one only when it is closer to the
// query than to every neighbour kept so far. This is what produces the long range links;
// without it the graph degenerates into a k-NN graph and recall collapses.
//
// The pruned tail is appended back when the diversity rule leaves the list short: on
// tightly packed clusters almost every candidate is rejected by the rule above, the
// intra-cluster links disappear and queries cannot walk inside a cluster any more
// (recall then stops improving no matter how wide ef_search gets).
void select_neighbors(const BaseVectors &base,
                      const std::vector<std::pair<float, int32_t>> &candidates,
                      int64_t max_degree,
                      std::vector<int32_t> &selected)
{
  selected.clear();
  if (max_degree > 0) {
    selected.reserve(static_cast<size_t>(max_degree));
    std::vector<int32_t> pruned;
    pruned.reserve(static_cast<size_t>(max_degree));
    for (size_t i = 0; i < candidates.size(); ++i) {
      const int32_t candidate = candidates[i].second;
      const float candidate_cost = candidates[i].first;
      bool keep = true;
      for (size_t s = 0; s < selected.size(); ++s) {
        if (base.dist(candidate, selected[s]) < candidate_cost) {
          keep = false;
          break;
        }
      }
      if (keep) {
        if (static_cast<int64_t>(selected.size()) < max_degree) {
          selected.push_back(candidate);
        } else {
          pruned.push_back(candidate);
        }
      } else {
        pruned.push_back(candidate);
      }
    }
    for (size_t i = 0; i < pruned.size(); ++i) {
      if (static_cast<int64_t>(selected.size()) >= max_degree) {
        break;
      }
      selected.push_back(pruned[i]);
    }
  }
}

// Reverse edge table (CSR) over a `count x degree` block of forward links.
// Links are expressed in the same index space as the rows: level 0 uses node ids
// directly, upper levels use positions inside the level's participant list.
void build_reverse_table(const std::vector<int32_t> &out_links,
                         int64_t count,
                         int64_t degree,
                         std::vector<int64_t> &offsets,
                         std::vector<int32_t> &reverse_edges)
{
  offsets.assign(static_cast<size_t>(count) + 1, 0);
  for (int64_t row = 0; row < count; ++row) {
    const int32_t *links = out_links.data() + row * degree;
    for (int64_t i = 0; i < degree; ++i) {
      if (links[i] >= 0) {
        ++offsets[static_cast<size_t>(links[i] + 1)];
      }
    }
  }
  for (int64_t i = 0; i < count; ++i) {
    offsets[static_cast<size_t>(i + 1)] += offsets[static_cast<size_t>(i)];
  }
  std::vector<int32_t> cursor(offsets.begin(), offsets.end() - 1);
  reverse_edges.assign(static_cast<size_t>(offsets[static_cast<size_t>(count)]), -1);
  for (int64_t row = 0; row < count; ++row) {
    const int32_t *links = out_links.data() + row * degree;
    for (int64_t i = 0; i < degree; ++i) {
      if (links[i] >= 0) {
        const int32_t target = links[i];
        reverse_edges[static_cast<size_t>(cursor[static_cast<size_t>(target)]++)] =
            static_cast<int32_t>(row);
      }
    }
  }
}

} // namespace

StatusCode build_layered_graph(const BaseVectors &base,
                               const KnnGraph &knn,
                               const BuildConfig &cfg,
                               HnswGraph &graph,
                               std::string &error)
{
  StatusCode status = StatusCode::OK;
  if (base.n <= 0 || base.dim <= 0 || knn.n != base.n || knn.k <= 0) {
    error = "layered graph: candidate graph does not match the base vectors";
    status = StatusCode::INVALID_ARGUMENT;
  } else if (cfg.m <= 0) {
    error = "layered graph: m must be positive";
    status = StatusCode::INVALID_ARGUMENT;
  } else {
    const int64_t n = base.n;
    const int64_t threads = cfg.threads > 0 ? cfg.threads : default_thread_count();
    const int32_t m = static_cast<int32_t>(cfg.m);
    const int32_t m0 = static_cast<int32_t>(cfg.m0());
    const uint64_t seed = static_cast<uint64_t>(cfg.seed);

    graph.init(n, base.dim, m, m0, base.metric);

    // ---- 1. deterministic level assignment -------------------------------------
    int32_t max_level = 0;
    for (int64_t i = 0; i < n; ++i) {
      const int32_t level = random_level(seed, i, cfg.m, LEVEL_CAP);
      graph.levels[static_cast<size_t>(i)] = level;
      max_level = std::max(max_level, level);
    }
    graph.max_level = max_level;
    for (int64_t i = 0; i < n; ++i) {
      const int32_t level = graph.levels[static_cast<size_t>(i)];
      if (level > 0) {
        graph.upper[static_cast<size_t>(i)].assign(static_cast<size_t>(level) * m, -1);
      }
    }

    // ---- 2. level 0: k-NN candidates -> heuristic prune -> reverse edge repair --
    {
      std::vector<int32_t> out_links(static_cast<size_t>(n * m0), -1);
      parallel_for(n, threads, [&](int64_t row) {
        std::vector<std::pair<float, int32_t>> candidates;
        const int32_t *ids = knn.row(row);
        const float *costs = knn.dist_row(row);
        candidates.reserve(static_cast<size_t>(knn.k));
        for (int64_t i = 0; i < knn.k; ++i) {
          if (ids[i] >= 0 && ids[i] != row) {
            candidates.emplace_back(costs[i], ids[i]);
          }
        }
        sort_unique_by_cost(candidates);
        std::vector<int32_t> selected;
        select_neighbors(base, candidates, m0, selected);
        int32_t *dst = out_links.data() + row * m0;
        for (size_t s = 0; s < selected.size(); ++s) {
          dst[s] = selected[s];
        }
      });

      std::vector<int64_t> offsets;
      std::vector<int32_t> reverse_edges;
      build_reverse_table(out_links, n, m0, offsets, reverse_edges);

      parallel_for(n, threads, [&](int64_t row) {
        std::vector<std::pair<float, int32_t>> candidates;
        candidates.reserve(static_cast<size_t>(m0) +
                           static_cast<size_t>(offsets[static_cast<size_t>(row + 1)] -
                                               offsets[static_cast<size_t>(row)]));
        const int32_t *forward = out_links.data() + row * m0;
        for (int64_t i = 0; i < m0; ++i) {
          if (forward[i] >= 0 && forward[i] != row) {
            candidates.emplace_back(base.dist(row, forward[i]), forward[i]);
          }
        }
        const int32_t *reverse = reverse_edges.data() + offsets[static_cast<size_t>(row)];
        const int64_t reverse_count =
            offsets[static_cast<size_t>(row + 1)] - offsets[static_cast<size_t>(row)];
        for (int64_t i = 0; i < reverse_count; ++i) {
          if (reverse[i] >= 0 && reverse[i] != row) {
            candidates.emplace_back(base.dist(row, reverse[i]), reverse[i]);
          }
        }
        sort_unique_by_cost(candidates);
        std::vector<int32_t> merged;
        select_neighbors(base, candidates, m0, merged);
        int32_t *dst = graph.level0_row(row);
        for (size_t s = 0; s < merged.size(); ++s) {
          dst[s] = merged[s];
        }
      });
    }

    // ---- 3. upper levels ------------------------------------------------------
    // Only the nodes reaching a level take part in it. Candidates are seeded from the
    // level below plus a bounded stride sample of the participants, which keeps the
    // top layers cheap while still giving the entry point usable links.
    std::vector<int32_t> position(static_cast<size_t>(n), -1);
    for (int32_t level = 1; level <= max_level; ++level) {
      std::vector<int32_t> present;
      present.reserve(static_cast<size_t>(n));
      for (int64_t i = 0; i < n; ++i) {
        if (graph.levels[static_cast<size_t>(i)] >= level) {
          position[static_cast<size_t>(i)] = static_cast<int32_t>(present.size());
          present.push_back(static_cast<int32_t>(i));
        }
      }
      const int64_t present_count = static_cast<int64_t>(present.size());
      if (present_count <= 1) {
        for (int64_t s = 0; s < present_count; ++s) {
          position[static_cast<size_t>(present[static_cast<size_t>(s)])] = -1;
        }
        continue;
      }

      std::vector<int32_t> out_links(static_cast<size_t>(present_count * m), -1);
      const int64_t stride = std::max<int64_t>(1, present_count / UPPER_SAMPLE_SIZE);
      parallel_for(present_count, threads, [&](int64_t slot) {
        const int64_t node = present[static_cast<size_t>(slot)];
        std::vector<std::pair<float, int32_t>> candidates;
        const int64_t below_degree = graph.degree(node, level - 1);
        const int32_t *below = graph.row(node, level - 1);
        for (int64_t i = 0; i < below_degree; ++i) {
          const int32_t candidate = below[i];
          if (candidate >= 0 && position[static_cast<size_t>(candidate)] >= 0) {
            candidates.emplace_back(base.dist(node, candidate), candidate);
          }
        }
        for (int64_t s = 0; s < present_count; s += stride) {
          const int32_t candidate = present[static_cast<size_t>(s)];
          if (candidate != node) {
            candidates.emplace_back(base.dist(node, candidate), candidate);
          }
        }
        sort_unique_by_cost(candidates);
        std::vector<int32_t> selected;
        select_neighbors(base, candidates, m, selected);
        int32_t *dst = out_links.data() + slot * m;
        for (size_t s = 0; s < selected.size(); ++s) {
          dst[s] = position[static_cast<size_t>(selected[s])];
        }
      });

      std::vector<int64_t> offsets;
      std::vector<int32_t> reverse_edges;
      build_reverse_table(out_links, present_count, m, offsets, reverse_edges);

      parallel_for(present_count, threads, [&](int64_t slot) {
        std::vector<std::pair<float, int32_t>> candidates;
        const int32_t *forward = out_links.data() + slot * m;
        for (int64_t i = 0; i < m; ++i) {
          if (forward[i] >= 0) {
            candidates.emplace_back(0.0f, present[static_cast<size_t>(forward[i])]);
          }
        }
        const int32_t *reverse = reverse_edges.data() + offsets[static_cast<size_t>(slot)];
        const int64_t reverse_count =
            offsets[static_cast<size_t>(slot + 1)] - offsets[static_cast<size_t>(slot)];
        for (int64_t i = 0; i < reverse_count; ++i) {
          if (reverse[i] >= 0) {
            candidates.emplace_back(0.0f, present[static_cast<size_t>(reverse[i])]);
          }
        }
        const int64_t self = present[static_cast<size_t>(slot)];
        for (size_t i = 0; i < candidates.size(); ++i) {
          candidates[i].first = base.dist(self, candidates[i].second);
        }
        sort_unique_by_cost(candidates);
        std::vector<int32_t> merged;
        select_neighbors(base, candidates, m, merged);
        int32_t *dst = graph.upper_row(self, level);
        for (size_t s = 0; s < merged.size(); ++s) {
          dst[s] = merged[s];
        }
      });

      for (int64_t s = 0; s < present_count; ++s) {
        position[static_cast<size_t>(present[static_cast<size_t>(s)])] = -1;
      }
    }

    // ---- 4. entry point: highest level, lowest id on ties ----------------------
    int32_t entry = 0;
    int32_t entry_level = -1;
    for (int64_t i = 0; i < n; ++i) {
      const int32_t level = graph.levels[static_cast<size_t>(i)];
      if (level > entry_level) {
        entry_level = level;
        entry = static_cast<int32_t>(i);
      }
    }
    graph.entry_point = entry;
  }
  return status;
}

void search_graph(const BaseVectors &base,
                  const HnswGraph &graph,
                  const float *query,
                  float query_norm,
                  const SearchConfig &cfg,
                  std::vector<int32_t> &ids,
                  std::vector<float> &costs)
{
  ids.clear();
  costs.clear();
  if (nullptr == query || graph.n <= 0 || cfg.topk <= 0) {
    return;
  }
  const int64_t ef = std::max<int64_t>(cfg.ef_search, cfg.topk);
  std::vector<uint8_t> visited(static_cast<size_t>(graph.n), 0);

  int32_t current = graph.entry_point;
  float current_cost = base.dist_query(query, query_norm, current);

  // greedy descent through the upper layers
  for (int32_t level = graph.max_level; level >= 1; --level) {
    bool improved = true;
    while (improved) {
      improved = false;
      const int64_t degree = graph.degree(current, level);
      const int32_t *links = graph.row(current, level);
      for (int64_t i = 0; i < degree; ++i) {
        const int32_t candidate = links[i];
        if (candidate < 0) {
          continue;
        }
        const float cost = base.dist_query(query, query_norm, candidate);
        if (cost < current_cost) {
          current_cost = cost;
          current = candidate;
          improved = true;
        }
      }
    }
  }

  typedef std::pair<float, int32_t> Candidate;
  std::priority_queue<Candidate, std::vector<Candidate>, std::greater<Candidate>> frontier;
  std::priority_queue<Candidate> best;
  frontier.push(std::make_pair(current_cost, current));
  best.push(std::make_pair(current_cost, current));
  visited[static_cast<size_t>(current)] = 1;

  while (!frontier.empty()) {
    const Candidate top = frontier.top();
    frontier.pop();
    if (static_cast<int64_t>(best.size()) >= ef && top.first > best.top().first) {
      break;
    }
    const int64_t degree = graph.degree(top.second, 0);
    const int32_t *links = graph.level0_row(top.second);
    for (int64_t i = 0; i < degree; ++i) {
      const int32_t candidate = links[i];
      if (candidate < 0 || visited[static_cast<size_t>(candidate)]) {
        continue;
      }
      visited[static_cast<size_t>(candidate)] = 1;
      const float cost = base.dist_query(query, query_norm, candidate);
      if (static_cast<int64_t>(best.size()) < ef || cost < best.top().first) {
        frontier.push(std::make_pair(cost, candidate));
        best.push(std::make_pair(cost, candidate));
        if (static_cast<int64_t>(best.size()) > ef) {
          best.pop();
        }
      }
    }
  }

  std::vector<Candidate> sorted;
  sorted.reserve(best.size());
  while (!best.empty()) {
    sorted.push_back(best.top());
    best.pop();
  }
  std::sort(sorted.begin(), sorted.end(), by_cost);
  const int64_t keep = std::min<int64_t>(cfg.topk, static_cast<int64_t>(sorted.size()));
  ids.reserve(static_cast<size_t>(keep));
  costs.reserve(static_cast<size_t>(keep));
  for (int64_t i = 0; i < keep; ++i) {
    costs.push_back(sorted[static_cast<size_t>(i)].first);
    ids.push_back(sorted[static_cast<size_t>(i)].second);
  }
}

double recall_at_k(const std::vector<std::vector<int32_t>> &results,
                   const std::vector<std::vector<int32_t>> &truth)
{
  double total = 0.0;
  int64_t queries = 0;
  const size_t count = std::min(results.size(), truth.size());
  for (size_t q = 0; q < count; ++q) {
    if (truth[q].empty()) {
      continue;
    }
    int64_t hits = 0;
    for (size_t i = 0; i < results[q].size(); ++i) {
      if (std::find(truth[q].begin(), truth[q].end(), results[q][i]) != truth[q].end()) {
        ++hits;
      }
    }
    total += static_cast<double>(hits) / static_cast<double>(truth[q].size());
    ++queries;
  }
  return queries == 0 ? 0.0 : total / static_cast<double>(queries);
}

} // namespace hnsw_gpu
} // namespace oceanbase

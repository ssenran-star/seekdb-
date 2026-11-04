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

// Unit tests for the hnsw_gpu build path. They only need a CPU: the correctness of the
// graph builder, the search and the file format is independent of which k-NN engine
// produced the candidate lists, so the same assertions cover the hipVS backend once a
// ROCm host runs them with --engine hipvs.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ob_hnsw_gpu_builder.h"
#include "ob_hnsw_graph_file.h"
#include "ob_hnsw_parallel.h"
#include "ob_hnsw_rng.h"

using namespace oceanbase::hnsw_gpu;

namespace
{

int g_failures = 0;

void check(bool condition, const char *what)
{
  if (condition) {
    printf("  PASS  %s\n", what);
  } else {
    printf("  FAIL  %s\n", what);
    ++g_failures;
  }
}

double gaussian(uint64_t seed, uint64_t salt)
{
  const double u1 = uniform01(seed, salt * 2 + 1);
  const double u2 = uniform01(seed, salt * 2 + 2);
  return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
}

void generate(int64_t n, int64_t dim, int64_t clusters, uint64_t seed, std::vector<float> &out)
{
  if (clusters < 1) {
    clusters = 1;
  }
  std::vector<float> centers(static_cast<size_t>(clusters * dim), 0.0f);
  for (size_t i = 0; i < centers.size(); ++i) {
    centers[i] = static_cast<float>(gaussian(seed, i + 1000));
  }
  out.assign(static_cast<size_t>(n * dim), 0.0f);
  for (int64_t row = 0; row < n; ++row) {
    const int64_t cluster =
        static_cast<int64_t>(uniform01(seed, row + 7) * clusters) % clusters;
    const float *center = centers.data() + cluster * dim;
    float *dst = out.data() + row * dim;
    for (int64_t d = 0; d < dim; ++d) {
      dst[d] = center[d] + static_cast<float>(gaussian(seed, (row * dim + d) + 31)) * 0.35f;
    }
  }
}

struct RecallReport
{
  double recall = 0.0;
  int64_t self_loops = 0;
  int64_t degree_violations = 0;
};

RecallReport evaluate(const BaseVectors &view,
                      const HnswGraph &graph,
                      int64_t topk,
                      int64_t ef_search,
                      int64_t queries)
{
  RecallReport report;
  std::vector<std::vector<int32_t>> results;
  std::vector<std::vector<int32_t>> truth;
  std::vector<float> query(static_cast<size_t>(view.dim), 0.0f);
  SearchConfig scfg;
  scfg.ef_search = ef_search;
  scfg.topk = topk;
  for (int64_t q = 0; q < queries; ++q) {
    const int64_t anchor = (q * 7919) % view.n;
    const float *src = view.at(anchor);
    for (int64_t d = 0; d < view.dim; ++d) {
      query[static_cast<size_t>(d)] =
          src[d] + static_cast<float>(gaussian(97, static_cast<uint64_t>(q * view.dim + d) + 3)) *
                       0.05f;
    }
    const float query_norm = squared_norm(query.data(), view.dim);
    std::vector<int32_t> truth_ids;
    std::vector<float> truth_costs;
    exact_search(view, query.data(), query_norm, topk, truth_ids, truth_costs);
    truth.push_back(truth_ids);
    std::vector<int32_t> ids;
    std::vector<float> costs;
    search_graph(view, graph, query.data(), query_norm, scfg, ids, costs);
    results.push_back(ids);
  }
  report.recall = recall_at_k(results, truth);

  for (int64_t id = 0; id < graph.n; ++id) {
    for (int32_t level = 0; level <= graph.levels[static_cast<size_t>(id)]; ++level) {
      const int64_t cap = graph.slot_count(level);
      const int32_t *links = graph.row(id, level);
      for (int64_t i = 0; i < cap; ++i) {
        if (links[i] < 0) {
          break;
        }
        if (links[i] == id) {
          ++report.self_loops;
        }
        if (links[i] >= graph.n) {
          ++report.degree_violations;
        }
      }
      if (graph.degree(id, level) > cap) {
        ++report.degree_violations;
      }
    }
  }
  return report;
}

} // namespace

int main(int argc, char **argv)
{
  const char *engine = "cpu-exact";
  for (int i = 1; i < argc; ++i) {
    if (0 == strcmp(argv[i], "--engine") && i + 1 < argc) {
      engine = argv[++i];
    }
  }

  const int64_t n = 3000;
  const int64_t dim = 48;
  const int64_t clusters = 24;
  std::vector<float> data;
  generate(n, dim, clusters, 42, data);

  BuildConfig cfg;
  cfg.dim = dim;
  cfg.m = 16;
  cfg.metric = MetricType::L2;
  cfg.threads = 4;

  printf("== hnsw_gpu unit tests (k-NN engine: %s) ==\n", engine);

  HnswGraph graph;
  BuildStats stats;
  std::string error;
  StatusCode status =
      build_index(data.data(), n, dim, cfg.metric, cfg, engine, graph, stats, error);
  check(StatusCode::OK == status, "build succeeds");
  if (StatusCode::OK != status) {
    printf("  build error: %s\n", error.c_str());
    return 1;
  }
  check(graph.n == n && graph.dim == dim, "graph dimensions match the input");
  check(graph.entry_point >= 0 && graph.entry_point < n, "entry point in range");
  check(stats.edge_count > 0 && stats.total_us > 0, "build stats populated");
  printf("  info  build %lld us (knn %lld, layer %lld), edges %lld, max_level %lld\n",
         (long long)stats.total_us,
         (long long)stats.knn_stage_us,
         (long long)stats.layer_stage_us,
         (long long)stats.edge_count,
         (long long)stats.max_level);

  BaseVectors view;
  view.bind(data.data(), n, dim, cfg.metric);
  const RecallReport report = evaluate(view, graph, 10, 64, 100);
  check(0 == report.self_loops, "no self loops");
  check(0 == report.degree_violations, "degrees stay within m0/m and ids in range");
  printf("  info  recall@10 = %.4f\n", report.recall);
  check(report.recall >= 0.90, "recall@10 >= 0.90 with m=16, ef=64");

  // ---- determinism --------------------------------------------------------
  HnswGraph again;
  BuildStats stats_again;
  status = build_index(data.data(), n, dim, cfg.metric, cfg, engine, again, stats_again, error);
  check(StatusCode::OK == status, "rebuild succeeds");
  check(again.level0 == graph.level0, "level 0 links are deterministic");
  check(again.levels == graph.levels, "level assignment is deterministic");
  check(again.entry_point == graph.entry_point, "entry point is deterministic");

  // ---- file format --------------------------------------------------------
  const char *path = "test_hnsw_gpu_graph.bin";
  status = save_graph(graph, path, error);
  check(StatusCode::OK == status, "save graph");
  HnswGraph loaded;
  status = load_graph(path, loaded, error);
  check(StatusCode::OK == status, "load graph");
  check(loaded.n == graph.n && loaded.dim == graph.dim && loaded.m == graph.m &&
            loaded.m0 == graph.m0,
        "header round trips");
  check(loaded.level0 == graph.level0 && loaded.levels == graph.levels &&
            loaded.entry_point == graph.entry_point,
        "links round trip");
  remove(path);

  // ---- other metrics ------------------------------------------------------
  const MetricType metrics[2] = {MetricType::INNER_PRODUCT, MetricType::COSINE};
  const char *names[2] = {"ip", "cosine"};
  for (int m = 0; m < 2; ++m) {
    BuildConfig mcfg = cfg;
    mcfg.metric = metrics[m];
    HnswGraph mgraph;
    BuildStats mstats;
    status = build_index(data.data(), n, dim, mcfg.metric, mcfg, engine, mgraph, mstats, error);
    const bool built = StatusCode::OK == status;
    check(built, names[m]);
    if (built) {
      BaseVectors mview;
      mview.bind(data.data(), n, dim, mcfg.metric);
      const RecallReport mreport = evaluate(mview, mgraph, 10, 64, 100);
      printf("  info  %s recall@10 = %.4f\n", names[m], mreport.recall);
      check(mreport.recall >= 0.85, "recall@10 >= 0.85 for this metric");
    }
  }

  // ---- invalid input ------------------------------------------------------
  HnswGraph dummy;
  BuildStats dummy_stats;
  status = build_index(nullptr, n, dim, cfg.metric, cfg, engine, dummy, dummy_stats, error);
  check(StatusCode::INVALID_ARGUMENT == status, "null input is rejected");
  status = build_index(data.data(), n, dim, cfg.metric, cfg, "nope", dummy, dummy_stats, error);
  check(StatusCode::INVALID_ARGUMENT == status, "unknown engine is rejected");

  printf("== %s (%d failure(s)) ==\n", 0 == g_failures ? "PASSED" : "FAILED", g_failures);
  return 0 == g_failures ? 0 : 1;
}

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

// Standalone harness for the hnsw_gpu build path.
//
// It exists so that the build stage can be measured without a seekdb server:
//   * same source files the engine compiles, no framework dependency
//   * exact ground truth, therefore a real recall number next to the timing
//   * --engine hipvs gives the GPU number, --engine cpu-ivf the CPU baseline
//
// Example (CPU baseline):
//   hnsw_gpu_bench --n 100000 --dim 128 --engine cpu-ivf --m 16 --topk 10
// Example (GPU, on a ROCm host):
//   hnsw_gpu_bench --n 1000000 --dim 768 --engine hipvs --m 16 --topk 10 --json out.json

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "ob_hnsw_gpu_builder.h"
#include "ob_hnsw_graph_file.h"
#include "ob_hnsw_rng.h"

using namespace oceanbase::hnsw_gpu;

namespace
{

struct Options
{
  int64_t n = 100000;
  int64_t dim = 128;
  int64_t clusters = 64;
  int64_t topk = 10;
  int64_t ef_search = 64;
  int64_t queries = 200;
  int64_t thread_count = 0;
  int64_t ivf_nlist = 0;
  int64_t ivf_nprobe = 8;
  int64_t gpu_block_rows = 1 << 20;
  BuildConfig build;
  std::string engine = "cpu-ivf";
  std::string save_path;
  std::string json_path;
  bool verify_io = false;
};

void usage()
{
  printf(
      "usage: hnsw_gpu_bench [options]\n"
      "  --n <rows>              base vector count        (default 100000)\n"
      "  --dim <dim>             dimensionality           (default 128)\n"
      "  --clusters <count>      synthetic cluster count  (default 64)\n"
      "  --m <m>                 HNSW out-degree          (default 16)\n"
      "  --ef-construction <ef>  kept for parity, unused  (default 100)\n"
      "  --knn-k <k>             candidate list width     (default 2*2*m)\n"
      "  --metric <l2|ip|cosine> distance                 (default l2)\n"
      "  --engine <name>         %s\n"
      "  --threads <n>           host threads             (default hw)\n"
      "  --ivf-nlist <n>         cpu-ivf partitions       (default 4*sqrt(n))\n"
      "  --ivf-nprobe <n>        cpu-ivf probes           (default 8)\n"
      "  --gpu-block-rows <n>    rows per GPU wave         (default 1048576)\n"
      "  --topk <k>              recall@k                 (default 10)\n"
      "  --ef-search <ef>        search beam              (default 64)\n"
      "  --queries <n>           recall measurement size  (default 200)\n"
      "  --seed <n>              deterministic seed       (default 42)\n"
      "  --save <path>           dump the built graph\n"
      "  --verify-io             save+load and compare\n"
      "  --json <path>           write machine readable result\n",
      knn_engine_names());
}

bool parse_int(const char *text, int64_t &value)
{
  bool ok = false;
  if (nullptr != text) {
    char *end = nullptr;
    const long long parsed = strtoll(text, &end, 10);
    if (nullptr != end && '\0' == *end) {
      value = static_cast<int64_t>(parsed);
      ok = true;
    }
  }
  return ok;
}

bool parse_args(int argc, char **argv, Options &opt, std::string &error)
{
  bool ok = true;
  for (int i = 1; i < argc && ok; ++i) {
    const char *flag = argv[i];
    const char *value = (i + 1 < argc) ? argv[i + 1] : nullptr;
    if (0 == strcmp(flag, "-h") || 0 == strcmp(flag, "--help")) {
      usage();
      exit(0);
    } else if (0 == strcmp(flag, "--verify-io")) {
      opt.verify_io = true;
      continue;
    } else if (nullptr == value) {
      error = std::string("missing value for ") + flag;
      ok = false;
      continue;
    } else if (0 == strcmp(flag, "--n")) {
      ok = parse_int(value, opt.n);
    } else if (0 == strcmp(flag, "--dim")) {
      ok = parse_int(value, opt.dim);
    } else if (0 == strcmp(flag, "--clusters")) {
      ok = parse_int(value, opt.clusters);
    } else if (0 == strcmp(flag, "--m")) {
      ok = parse_int(value, opt.build.m);
    } else if (0 == strcmp(flag, "--ef-construction")) {
      ok = parse_int(value, opt.build.ef_construction);
    } else if (0 == strcmp(flag, "--knn-k")) {
      ok = parse_int(value, opt.build.knn_k);
    } else if (0 == strcmp(flag, "--threads")) {
      ok = parse_int(value, opt.thread_count);
    } else if (0 == strcmp(flag, "--ivf-nlist")) {
      ok = parse_int(value, opt.ivf_nlist);
    } else if (0 == strcmp(flag, "--ivf-nprobe")) {
      ok = parse_int(value, opt.ivf_nprobe);
    } else if (0 == strcmp(flag, "--gpu-block-rows")) {
      ok = parse_int(value, opt.gpu_block_rows);
    } else if (0 == strcmp(flag, "--topk")) {
      ok = parse_int(value, opt.topk);
    } else if (0 == strcmp(flag, "--ef-search")) {
      ok = parse_int(value, opt.ef_search);
    } else if (0 == strcmp(flag, "--queries")) {
      ok = parse_int(value, opt.queries);
    } else if (0 == strcmp(flag, "--seed")) {
      ok = parse_int(value, opt.build.seed);
    } else if (0 == strcmp(flag, "--engine")) {
      opt.engine = value;
    } else if (0 == strcmp(flag, "--metric")) {
      ok = parse_metric(value, opt.build.metric);
    } else if (0 == strcmp(flag, "--save")) {
      opt.save_path = value;
    } else if (0 == strcmp(flag, "--json")) {
      opt.json_path = value;
    } else {
      error = std::string("unknown option ") + flag;
      ok = false;
    }
    ++i;
  }
  if (!ok && error.empty()) {
    error = "invalid numeric argument";
  }
  return ok;
}

// Box-Muller, fed by the deterministic splitmix64 stream.
double gaussian(uint64_t seed, uint64_t salt)
{
  const double u1 = uniform01(seed, salt * 2 + 1);
  const double u2 = uniform01(seed, salt * 2 + 2);
  return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
}

// Clustered synthetic data: nearest neighbour structure survives sampling, so recall
// numbers are meaningful without shipping a dataset.
void generate_clustered(int64_t n,
                        int64_t dim,
                        int64_t clusters,
                        uint64_t seed,
                        float spread,
                        std::vector<float> &out)
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
    const int64_t cluster = static_cast<int64_t>(uniform01(seed, row + 7) * clusters) % clusters;
    const float *center = centers.data() + cluster * dim;
    float *dst = out.data() + row * dim;
    for (int64_t d = 0; d < dim; ++d) {
      dst[d] = center[d] + static_cast<float>(gaussian(seed, (row * dim + d) + 31)) * spread;
    }
  }
}

double to_ms(int64_t us) { return static_cast<double>(us) / 1000.0; }

} // namespace

int main(int argc, char **argv)
{
  Options opt;
  std::string error;
  if (!parse_args(argc, argv, opt, error)) {
    printf("argument error: %s\n", error.c_str());
    usage();
    return 2;
  }
  if (opt.n <= 1 || opt.dim <= 0 || opt.topk <= 0 || opt.queries <= 0) {
    printf("invalid scale: n, dim, topk and queries must be positive\n");
    return 2;
  }

  std::string reason;
  {
    std::unique_ptr<IKnnGraphEngine> probe(create_knn_engine(opt.engine.c_str()));
    if (nullptr == probe.get()) {
      printf("unknown engine '%s', expected one of %s\n", opt.engine.c_str(), knn_engine_names());
      return 2;
    }
    if (!probe->available(reason)) {
      printf("engine '%s' is not usable: %s\n", opt.engine.c_str(), reason.c_str());
      return 3;
    }
  }

  BuildConfig cfg = opt.build;
  cfg.dim = opt.dim;
  cfg.threads = opt.thread_count;
  cfg.ivf_nlist = opt.ivf_nlist;
  cfg.ivf_nprobe = opt.ivf_nprobe;
  cfg.gpu_block_rows = opt.gpu_block_rows;

  printf("== hnsw_gpu build benchmark ==\n");
  printf("engine       : %s%s\n", opt.engine.c_str(), hipvs_compiled_in() ? "" : " (hipvs off)");
  printf("rows x dim   : %lld x %lld\n", (long long)opt.n, (long long)opt.dim);
  printf("m / m0       : %lld / %lld\n", (long long)cfg.m, (long long)cfg.m0());
  printf("knn_k        : %lld\n", (long long)cfg.effective_knn_k());
  printf("metric       : %s\n", metric_name(cfg.metric));

  std::vector<float> base;
  const int64_t t_gen = now_us();
  generate_clustered(opt.n, opt.dim, opt.clusters, static_cast<uint64_t>(cfg.seed), 0.35f, base);
  printf("data gen     : %.1f ms\n", to_ms(now_us() - t_gen));

  HnswGraph graph;
  BuildStats stats;
  const int64_t build_begin = now_us();
  StatusCode status = build_index(base.data(),
                                  opt.n,
                                  opt.dim,
                                  cfg.metric,
                                  cfg,
                                  opt.engine.c_str(),
                                  graph,
                                  stats,
                                  error);
  const int64_t wall_us = now_us() - build_begin;
  if (StatusCode::OK != status) {
    printf("build failed: %s (%s)\n", status_name(status), error.c_str());
    return 4;
  }

  printf("knn stage    : %.1f ms\n", to_ms(stats.knn_stage_us));
  printf("layer stage  : %.1f ms\n", to_ms(stats.layer_stage_us));
  printf("build total  : %.1f ms (wall %.1f ms)\n", to_ms(stats.total_us), to_ms(wall_us));
  printf("throughput   : %.0f vectors/s\n",
         stats.total_us > 0 ? (double)opt.n * 1e6 / (double)stats.total_us : 0.0);
  printf("max level    : %lld\n", (long long)stats.max_level);
  printf("edges        : %lld (%.1f per node)\n",
         (long long)stats.edge_count,
         (double)stats.edge_count / (double)opt.n);

  BaseVectors view;
  view.bind(base.data(), opt.n, opt.dim, cfg.metric);

  // ---- recall against exact ground truth ------------------------------------
  const int64_t query_count = std::min<int64_t>(opt.queries, opt.n);
  std::vector<std::vector<int32_t>> results;
  std::vector<std::vector<int32_t>> truth;
  results.reserve(static_cast<size_t>(query_count));
  truth.reserve(static_cast<size_t>(query_count));
  std::vector<float> query(static_cast<size_t>(opt.dim), 0.0f);

  SearchConfig scfg;
  scfg.ef_search = opt.ef_search;
  scfg.topk = opt.topk;

  const int64_t recall_begin = now_us();
  for (int64_t q = 0; q < query_count; ++q) {
    const int64_t anchor = static_cast<int64_t>(uniform01(static_cast<uint64_t>(cfg.seed) + 11,
                                                          static_cast<uint64_t>(q) + 1) *
                                                (double)opt.n) %
                           opt.n;
    const float *src = base.data() + anchor * opt.dim;
    for (int64_t d = 0; d < opt.dim; ++d) {
      query[static_cast<size_t>(d)] =
          src[d] + static_cast<float>(gaussian(static_cast<uint64_t>(cfg.seed) + 29,
                                               static_cast<uint64_t>(q * opt.dim + d) + 5)) *
                       0.05f;
    }
    const float query_norm = squared_norm(query.data(), opt.dim);
    std::vector<int32_t> truth_ids;
    std::vector<float> truth_costs;
    exact_search(view, query.data(), query_norm, opt.topk, truth_ids, truth_costs);
    truth.push_back(truth_ids);

    std::vector<int32_t> ids;
    std::vector<float> costs;
    search_graph(view, graph, query.data(), query_norm, scfg, ids, costs);
    results.push_back(ids);
  }
  const int64_t recall_us = now_us() - recall_begin;
  const double recall = recall_at_k(results, truth);

  printf("recall@%-5lld : %.4f (%lld queries, exact truth)\n",
         (long long)opt.topk,
         recall,
         (long long)query_count);
  printf("search       : %.1f ms total, %.1f us/query\n",
         to_ms(recall_us),
         query_count > 0 ? (double)recall_us / (double)query_count : 0.0);

  bool io_ok = true;
  if (!opt.save_path.empty() || opt.verify_io) {
    const std::string path = opt.save_path.empty() ? std::string("hnsw_gpu_graph.bin")
                                                   : opt.save_path;
    status = save_graph(graph, path.c_str(), error);
    if (StatusCode::OK != status) {
      printf("save failed: %s\n", error.c_str());
      io_ok = false;
    } else {
      printf("saved        : %s\n", path.c_str());
    }
    if (io_ok && opt.verify_io) {
      HnswGraph reloaded;
      status = load_graph(path.c_str(), reloaded, error);
      if (StatusCode::OK != status) {
        printf("load failed: %s\n", error.c_str());
        io_ok = false;
      } else {
        const bool same = reloaded.n == graph.n && reloaded.level0 == graph.level0 &&
                          reloaded.levels == graph.levels &&
                          reloaded.entry_point == graph.entry_point;
        io_ok = same;
        printf("io roundtrip : %s\n", same ? "identical" : "MISMATCH");
      }
    }
  }

  if (!opt.json_path.empty()) {
    FILE *json = fopen(opt.json_path.c_str(), "w");
    if (nullptr == json) {
      printf("cannot write %s\n", opt.json_path.c_str());
    } else {
      fprintf(json, "{\n");
      fprintf(json, "  \"engine\": \"%s\",\n", stats.engine);
      fprintf(json, "  \"metric\": \"%s\",\n", metric_name(cfg.metric));
      fprintf(json, "  \"n\": %lld,\n", (long long)opt.n);
      fprintf(json, "  \"dim\": %lld,\n", (long long)opt.dim);
      fprintf(json, "  \"m\": %lld,\n", (long long)cfg.m);
      fprintf(json, "  \"knn_k\": %lld,\n", (long long)stats.knn_k);
      fprintf(json, "  \"knn_stage_ms\": %.3f,\n", to_ms(stats.knn_stage_us));
      fprintf(json, "  \"layer_stage_ms\": %.3f,\n", to_ms(stats.layer_stage_us));
      fprintf(json, "  \"build_total_ms\": %.3f,\n", to_ms(stats.total_us));
      fprintf(json, "  \"build_wall_ms\": %.3f,\n", to_ms(wall_us));
      fprintf(json, "  \"vectors_per_sec\": %.1f,\n",
              stats.total_us > 0 ? (double)opt.n * 1e6 / (double)stats.total_us : 0.0);
      fprintf(json, "  \"max_level\": %lld,\n", (long long)stats.max_level);
      fprintf(json, "  \"edges\": %lld,\n", (long long)stats.edge_count);
      fprintf(json, "  \"topk\": %lld,\n", (long long)opt.topk);
      fprintf(json, "  \"ef_search\": %lld,\n", (long long)opt.ef_search);
      fprintf(json, "  \"recall_at_k\": %.4f,\n", recall);
      fprintf(json, "  \"search_us_per_query\": %.2f,\n",
              query_count > 0 ? (double)recall_us / (double)query_count : 0.0);
      fprintf(json, "  \"io_roundtrip_ok\": %s\n", io_ok ? "true" : "false");
      fprintf(json, "}\n");
      fclose(json);
      printf("json         : %s\n", opt.json_path.c_str());
    }
  }

  return io_ok ? 0 : 5;
}

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

#include "ob_hnsw_graph_file.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace oceanbase
{
namespace hnsw_gpu
{

namespace
{

struct GraphFileHeader
{
  char magic[8];
  uint32_t version;
  int32_t metric;
  int32_t m;
  int32_t m0;
  int32_t max_level;
  int32_t entry_point;
  int64_t n;
  int64_t dim;
  int64_t edge_count;
};

} // namespace

StatusCode save_graph(const HnswGraph &graph, const char *path, std::string &error)
{
  StatusCode status = StatusCode::OK;
  if (nullptr == path) {
    error = "save_graph: null path";
    status = StatusCode::INVALID_ARGUMENT;
  } else {
    FILE *file = fopen(path, "wb");
    if (nullptr == file) {
      error = std::string("save_graph: cannot open ") + path;
      status = StatusCode::IO_ERROR;
    } else {
      GraphFileHeader header;
      memset(&header, 0, sizeof(header));
      memcpy(header.magic, GRAPH_FILE_MAGIC, 8);
      header.version = GRAPH_FILE_VERSION;
      header.metric = static_cast<int32_t>(graph.metric);
      header.m = graph.m;
      header.m0 = graph.m0;
      header.max_level = graph.max_level;
      header.entry_point = graph.entry_point;
      header.n = graph.n;
      header.dim = graph.dim;
      header.edge_count = graph.edge_count();

      bool ok = fwrite(&header, sizeof(header), 1, file) == 1;
      if (ok && !graph.levels.empty()) {
        ok = fwrite(graph.levels.data(), sizeof(int32_t), graph.levels.size(), file) ==
             graph.levels.size();
      }
      if (ok && !graph.level0.empty()) {
        ok = fwrite(graph.level0.data(), sizeof(int32_t), graph.level0.size(), file) ==
             graph.level0.size();
      }
      for (int64_t i = 0; ok && i < graph.n; ++i) {
        const std::vector<int32_t> &row = graph.upper[static_cast<size_t>(i)];
        if (!row.empty()) {
          ok = fwrite(row.data(), sizeof(int32_t), row.size(), file) == row.size();
        }
      }
      if (0 != fclose(file)) {
        ok = false;
      }
      if (!ok) {
        error = std::string("save_graph: write failed for ") + path;
        status = StatusCode::IO_ERROR;
      }
    }
  }
  return status;
}

StatusCode load_graph(const char *path, HnswGraph &graph, std::string &error)
{
  StatusCode status = StatusCode::OK;
  FILE *file = nullptr == path ? nullptr : fopen(path, "rb");
  if (nullptr == file) {
    error = std::string("load_graph: cannot open ") + (nullptr == path ? "" : path);
    status = StatusCode::IO_ERROR;
  } else {
    GraphFileHeader header;
    bool ok = fread(&header, sizeof(header), 1, file) == 1;
    if (!ok || 0 != memcmp(header.magic, GRAPH_FILE_MAGIC, 8)) {
      error = "load_graph: bad magic, not an hnsw_gpu graph file";
      status = StatusCode::IO_ERROR;
    } else if (header.version != GRAPH_FILE_VERSION) {
      error = "load_graph: unsupported version";
      status = StatusCode::IO_ERROR;
    } else if (header.n <= 0 || header.dim <= 0 || header.m <= 0) {
      error = "load_graph: corrupted header";
      status = StatusCode::IO_ERROR;
    } else {
      graph.init(header.n,
                 header.dim,
                 header.m,
                 header.m0,
                 static_cast<MetricType>(header.metric));
      graph.max_level = header.max_level;
      graph.entry_point = header.entry_point;
      ok = fread(graph.levels.data(), sizeof(int32_t), graph.levels.size(), file) ==
           graph.levels.size();
      if (ok) {
        ok = fread(graph.level0.data(), sizeof(int32_t), graph.level0.size(), file) ==
             graph.level0.size();
      }
      for (int64_t i = 0; ok && i < graph.n; ++i) {
        const int32_t level = graph.levels[static_cast<size_t>(i)];
        if (level > 0) {
          std::vector<int32_t> &row = graph.upper[static_cast<size_t>(i)];
          row.assign(static_cast<size_t>(level) * graph.m, -1);
          ok = fread(row.data(), sizeof(int32_t), row.size(), file) == row.size();
        }
      }
      fclose(file);
      file = nullptr;
      if (!ok) {
        error = "load_graph: truncated file";
        status = StatusCode::IO_ERROR;
      }
    }
  }
  if (StatusCode::OK != status && nullptr != file) {
    fclose(file);
  }
  return status;
}

} // namespace hnsw_gpu
} // namespace oceanbase

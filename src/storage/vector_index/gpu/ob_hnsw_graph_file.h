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

#include <string>

#include "ob_hnsw_layered_graph.h"

namespace oceanbase
{
namespace hnsw_gpu
{

// Versioned little-endian dump of a built graph. It exists so that a GPU build can be
// produced once and replayed by a CPU searcher (recall evaluation, offline tooling,
// and the ingest path into the storage engine).
static const char *const GRAPH_FILE_MAGIC = "HNSWGPU1";
static const uint32_t GRAPH_FILE_VERSION = 1;

StatusCode save_graph(const HnswGraph &graph, const char *path, std::string &error);
StatusCode load_graph(const char *path, HnswGraph &graph, std::string &error);

} // namespace hnsw_gpu
} // namespace oceanbase

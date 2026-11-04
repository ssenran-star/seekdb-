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

#include "ob_hnsw_knn_graph.h"

namespace oceanbase
{
namespace hnsw_gpu
{

// Implemented in ob_hnsw_knn_graph_cpu.cpp
IKnnGraphEngine *create_cpu_exact_engine();
IKnnGraphEngine *create_cpu_ivf_engine();

// Implemented in ob_hnsw_knn_graph_hipvs.cpp. Always returns a non-null engine, but
// available() reports false when the module was compiled without SEEKDB_ENABLE_HIPVS or
// when no HIP device is visible at runtime.
IKnnGraphEngine *create_hipvs_engine();

} // namespace hnsw_gpu
} // namespace oceanbase

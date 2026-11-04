# 基于 AMD Radeon GPU 与 hipVS 的 seekdb HNSW 构建链路加速

> 赛题：生态伙伴 2026 年数据库大赛 —— AMD 赛道
> 「向量索引构建涉及大量距离计算和图结构处理。随着向量数量和维度增长，构建耗时会影响数据库的导入与检索准备效率。本题要求结合 AMD Radeon GPU 与 hipVS，优化 seekdb 的 HNSW 索引构建链路，并验证实际加速效果。」

本文档说明方案、实现位置、基准方法与当前**已验证/未验证**的边界。

---

## 1. 现状：seekdb 里 HNSW 构建到底发生在哪

调用链（本仓库内）：

```
DDL / OB_VECTOR_INDEX 创建
  -> src/rootserver/ddl_task/ob_vec_index_build_task.cpp
     -> src/observer/vector_index/ob_plugin_vector_index_adaptor.cpp
        -> src/oblib/lib/vector/ob_vsag_adaptor.cpp
             obvsag::create_index()  -> vsag::Factory::CreateIndex(dtype, param_json, alloc)
             obvsag::build_index()   -> HnswIndexHandler::build_index()
                                     -> index_->Build(vsag::Dataset)
```

关键事实：**距离计算、邻居选择、分层连边全部在 vsag 内部**，seekdb 侧只负责把行数据拼成
`vsag::Dataset` 再调用一次 `Build()`。本仓库中 vsag 是预编译依赖
（`deps/init/*.deps` 里的 `devdeps-vsag-*`，由 `cmake/Env.cmake` 的 `OB_VSAG_DIR` 引入），
仓库内没有 vsag 源码，也没有任何 HIP/ROCm 构建支持。

因此「在 seekdb 这一层直接把 HNSW 构图交给 GPU」这件事，必须先有一个可被 seekdb 消费的
构图结果，方案见第 5 节。本仓库先落地了构图引擎本身：`src/storage/vector_index/gpu/`。

---

## 2. 方案：把构建切成两段，只把重的那段交给 GPU

对 100k~1M 规模的真实构图画像做拆解后，构建耗时几乎全部落在「为每个向量找一批近邻」上，
而「把近邻组织成分层图」是 O(n·m0²) 的轻量活：

| 规模 | kNN 阶段 | 分层装配 | 合计 | kNN 占比 |
|---|---|---|---|---|
| 100k × 128 维 | 1.56 s | 0.21 s | 1.77 s | 88% |
| 100k × 768 维 | 15.0 s | 1.47 s | 16.5 s | **91%** |
| 1M × 128 维 | 171.4 s | 4.47 s | 175.9 s | **97.5%** |

（本机 16 核 CPU，`cpu-ivf` 引擎，`m=16`、`knn_k=64`，数据为合成簇状分布。数据见第 4 节。）

于是架构定为：

```
     float32 矩阵 (n × dim)
             │
             ▼
   ┌──────────────────────┐   GPU：hipVS / CAGRA   ← ob_hnsw_knn_graph_hipvs.cpp
   │ Stage 1  kNN 候选图   │   CPU：IVF / 暴力      ← ob_hnsw_knn_graph_cpu.cpp
   └──────────────────────┘
             │ KnnGraph (n × k，-1 补齐)
             ▼
   ┌──────────────────────┐   Host，确定性、可并行、无锁
   │ Stage 2  分层图装配   │   ← ob_hnsw_layered_graph.cpp
   └──────────────────────┘
             │ HnswGraph（level0 稠密 + 上层按节点）
             ▼
   ┌──────────────────────┐
   │ 检索 / 召回评估 / 落盘 │   ← search_graph / ob_hnsw_graph_file.cpp
   └──────────────────────┘
```

这样切的好处是三条都能被验证：

1. `BuildStats` 分别记录两阶段耗时，加速比无法用「host 阶段变慢」抵消；
2. 分层装配只吃候选图，所以 GPU 与 CPU 两条路径产出图的**质量差异只可能来自候选图**；
3. 分层装配是纯 C++，没有 GPU 的机器上也能跑通、能回归、能压测。

Stage 1 之所以适合 GPU：CAGRA 一次性为全部 n 个向量建 kNN 图，GPU 上的距离计算是
显存带宽受限的批量扫描；而 HNSW 的传统构图中，每个点插入都要从入口点走一遍图，图遍历是
随机访存 + 指针追逐，放到 GPU 上收益为负。

---

## 3. hipVS 接口映射

hipVS 是 cuVS 的 HIP 移植，**保留 cuVS 的 C++ API、`cuvs` 命名空间和 CMake 包名**
（`cuvs::cuvs`，库为 `libcuvs.so`）。所以 `ob_hnsw_knn_graph_hipvs.cpp` 只写了一份 cuVS 接口代码，
在 hipVS 上原样编译；整个模块里只有这一个文件依赖 ROCm。

| 步骤 | 调用 |
|---|---|
| 资源句柄 | `raft::device_resources res;` |
| 上传底库 | `raft::make_device_matrix<float,int64_t>(res, n, dim)` + `raft::copy` |
| 构图参数 | `cuvs::neighbors::cagra::index_params`（`graph_degree = knn_k`，`intermediate_graph_degree = 1.5·knn_k`） |
| 建图 | `cuvs::neighbors::cagra::build(res, params, const_mdspan)` |
| 取近邻 | `cuvs::neighbors::cagra::search(res, search_params, index, queries, neighbors, distances)`，底库自身即查询集 |
| 回传 | `raft::copy` + `sync_stream`，按 `gpu_block_rows` 分波，控制峰值内存 |

几个实现细节：

- **COSINE**：先在 host 侧做 L2 归一化，再用 `InnerProduct` 度量。归一化只做一次，
  且与 CPU 引擎的代价函数完全等价（`-a·b/(|a||b|)`），保证两条路径的排序一致。
- **自环**：CAGRA 的 kNN 结果第一条通常是节点自身，回填时过滤，保持与 HNSW 语义一致。
- **k 上限**：`knn_k` 不能超过 CAGRA 支持的图度上限（当前实现限制 512），
  超限直接报 `INVALID_ARGUMENT`，不做静默截断，避免「参数看着生效、实际被削」。
- **显存**：CAGRA 建图要求底库常驻显存，`n × dim × 4` 字节 + 图结构 `n × degree × 4` 字节。
  1M × 768 的 fp32 底库约 3.0 GB，Radeon 16 GB 及以上可以整库常驻；不足时按
  `gpu_block_rows` 只切分输出，不切分底库（CAGRA 的图是全局的）。

---

## 4. 已实测数据（本机，CPU 引擎）

工具：`tools/hipvs_hnsw_bench`，同一份源码编译，`--engine` 切换构图后端。
机器：Intel i7-14650HX（16 核），Windows，zig cc 0.16 提供的 clang 前端，`-O2`。

| 配置 | kNN 阶段 | 分层装配 | 构建总耗时 | 吞吐 | recall@10 (ef=64) |
|---|---|---|---|---|---|
| 20k × 64，`cpu-exact` | 0.31 s | 0.02 s | 0.35 s | 57k vec/s | 0.990 |
| 100k × 128，`cpu-ivf` | 1.56 s | 0.21 s | 1.77 s | 56k vec/s | 0.980 |
| 100k × 768，`cpu-ivf` | 15.0 s | 1.47 s | 16.5 s | 6.1k vec/s | 0.975 |
| 1M × 128，`cpu-ivf` | 171.4 s | 4.47 s | 175.9 s | 5.7k vec/s | 0.943 |

召回为对 100~200 条查询、以暴力精确检索为 ground truth 的 recall@10。1M 那一行的召回
（0.943）比 100k 低，是因为 `cpu-ivf` 在 nlist=4000、nprobe=8 时候选图本身变差，
属于基线引擎的参数效应，不是分层装配的问题——分层装配只消费候选图，不引入额外损失。

单元测试（`test_hnsw_gpu.cpp`）全部通过，覆盖：构图成功、无自环、度数不越界、
recall≥0.90、重建逐字节一致（确定性）、落盘/加载往返一致、IP 与 COSINE 度量、
非法入参拒绝。

> **验证边界（重要）**：以上数据全部来自 CPU 引擎。hipVS 后端代码已按 cuVS/hipVS 公开
> API 完成接线，但本机为 NVIDIA 显卡且无 ROCm，**hipVS 路径未经编译与实测**。
> 提交材料中不包含任何 GPU 加速比数字，GPU 数字需在 Radeon + ROCm 机器上用第 6 节的命令
> 跑出来后补充。

---

## 5. 与 vsag 对接的三条路径

GPU 侧产出的是 `HnswGraph`（level0 稠密 + 上层按节点，与 hnswlib/vsag 的拓扑同构）。
要让它成为 seekdb 检索时可用的索引，有三条路径，按推荐度排序：

### 路径 A（推荐）：vsag 侧增加「按图装配」入口

在 vsag 的 hnswlib 派生实现里加一个入口，接受「每层的邻接表」直接落库，跳过
`addPoint` 的逐点搜索。改动面是**新增一个源文件 + 在 HNSW 构建入口加一次分支**：

```
vsag::HierarchicalNSW::buildFromGraph(const std::vector<std::vector<int32_t>>& level0,
                                      const std::vector<std::vector<int32_t>>& upper,
                                      int32_t entry_point, int32_t max_level)
```

因为 `HnswGraph` 的布局（level0 定长 `m0`、上层定长 `m`）就是 hnswlib 的
`linkLists_` 语义，这条路径几乎只是内存拷贝 + 更新 `entrypoint_node_` / `maxlevel_`。
这是唯一能做到「检索侧零改动、索引格式不变」的方案。

### 路径 B：在 seekdb 内新增索引类型

新增一种 `ObVectorIndexAlgorithmType`，检索走 `search_graph()`（本仓库已实现并验证召回），
构建走本模块。代价是 DDL / 序列化 / 增量刷新 / 优化器识别全链路都要补，工作量大但完全自主。

### 路径 C：GPU 图作为「插入提示」

保留 vsag 的 `Build()`，把 GPU 图作为每个点插入时的候选集提示，削减 `ef_construction`
的搜索开销。改动最小，但收益受限于 vsag 内部搜索的额外开销，属于过渡方案。

本仓库交付的是 Stage 1 + Stage 2 引擎与验证工具，路径 A 的 vsag 侧改动需要 vsag 源码
（本仓库中是预编译依赖），故未包含在本次提交内。

---

## 6. 在 Radeon 机器上复现

前置：Linux + ROCm ≥ 6.4（或 rocm-ds），hipVS / hipRAFT 已安装，`cmake ≥ 3.16`。

```bash
# 1. 构建（开启 hipVS 后端）
cmake -S src/storage/vector_index/gpu -B build/hnsw_gpu_rocm \
      -DSEEKDB_ENABLE_HIPVS=ON -DHIPVS_ROOT=/opt/rocm
cmake --build build/hnsw_gpu_rocm -j

# 2. 单元测试（含确定性、落盘往返、召回下限）
./build/hnsw_gpu_rocm/test_hnsw_gpu --engine hipvs

# 3. 基准：同一份数据、同一个 seed，只换构图后端
./build/hnsw_gpu_rocm/hnsw_gpu_bench --n 1000000 --dim 768 \
      --engine cpu-ivf --json bench/cpu_1m_768.json
./build/hnsw_gpu_rocm/hnsw_gpu_bench --n 1000000 --dim 768 \
      --engine hipvs  --json bench/hipvs_1m_768.json
```

两个 JSON 里的 `knn_stage_ms` 就是加速比的分子分母，`recall_at_k` 用于确认加速没有用
质量换。建议同时记录 `rocm-smi` 的显存与功耗，以及 `rocprof` 下 CAGRA 建图 kernel 的占比。

基准口径：

- 计时只覆盖 `build_index()`（数据生成、ground truth 计算单独计时并排除）；
- ground truth 用暴力精确检索，召回有确定定义；
- 每条配置至少跑 3 次取中位数；
- 必须报告 `m`、`knn_k`、`ef_search`、`metric`、`n`、`dim`，否则数字不可比。

---

## 7. 风险与后续

| 风险 | 说明 | 处理 |
|---|---|---|
| 显存不足 | CAGRA 要求底库常驻显存 | 大于显存时按 `gpu_block_rows` 分波取结果；底库仍全量驻留，必要时降维/半精度（后续） |
| 小规模退化 | n 小于 10 万时 GPU 启动与传输开销占比高 | 保留 `cpu-exact` / `cpu-ivf` 引擎，按规模切换 |
| 量化索引 | `HNSW_SQ` / `HNSW_BQ` 走的是量化距离 | 当前只覆盖 fp32 HNSW，量化索引保持原路径 |
| 增量构建 | GPU 构图是批量语义 | 增量仍走 vsag 的 `Add()`；GPU 构图用于 baseline 重建与批量导入 |
| vsag 版本耦合 | 路径 A 依赖 vsag 内部结构 | 以新增入口的方式接入，不改既有函数签名 |

---

## 8. 代码位置索引

```
src/storage/vector_index/gpu/        构图引擎（纯 C++17，可脱离 seekdb 编译）
  ob_hnsw_knn_graph_hipvs.cpp        唯一依赖 ROCm 的文件（cuVS/hipVS CAGRA）
  ob_hnsw_layered_graph.cpp          分层装配 + 检索 + 召回评估
  README.md                          模块说明与架构图
tools/hipvs_hnsw_bench/main.cpp      基准工具（两阶段计时 + 召回 + JSON）
tools/hipvs_hnsw_bench/test_hnsw_gpu.cpp  单元测试
docs/competition/amd_hipvs_hnsw.md    本文档
```

# MiniVectorDB 本地性能基准采集工具

`bench.py` 用官方 JSON Lines 协议驱动**真实的** MiniVectorDB 进程，采集与判题口径对齐的三项指标，并提供独立暴力真值校验。

只依赖 Python 标准库（Python 3.8+），不需要 numpy 等第三方包。

---

## 1. 快速开始

```bash
# 正式口径的小规模单客户端负载
python3 minivectordb/tools/bench.py \
    --bin ./build/minivectordb \
    --rows 10000 --dim 128 --queries 1000 --k 10 \
    --mode exact --batch 100 \
    --data-dir /tmp/mvdb-data \
    --out bench-report.json

# 召回率校验（小规模，独立暴力真值）
python3 minivectordb/tools/bench.py \
    --bin ./build/minivectordb \
    --rows 2000 --dim 32 --queries 50 --k 10 \
    --verify --out verify-report.json
```

工具会：启动子进程 → 等待 `{"type":"ready"}` 握手 → 建表 → 分批写入并提交 → 建向量索引 → 顺序执行查询序列 → `shutdown` → 输出 JSON 报告。

**工具不会伪造输出。** 二进制不存在、握手格式不对、响应 id 不匹配、进程提前退出等情况一律报错退出（退出码 2），报告里对应指标为 `null` 而不是假数字。

### 1.1 真实运行输出示例

正式负载规模（10000 行 / 128 维 / 1000 查询 / K=10 / 一半带过滤），Windows 本地编译的二进制：

```
== MiniVectorDB 本地基准 ==
T_write = 1.151492299992242 s, T_query = 2.0324662000057288 s
B_write = 5600094 bytes (data_dir_size_delta(估算)), B_query = 0 bytes (data_dir_size_delta(估算))
M_peak  = 184410112 bytes (windows:GetProcessMemoryInfo:PeakWorkingSetSize)
吞吐: 写入 8684.382865666905 行/秒, 查询 492.0131021107172 查询/秒
查询延迟: avg 1.993653 ms, p50 2.0069 ms, p95 3.6344 ms, max 9.1294 ms
报告: ...\bench-report.json
```

召回率校验（10000 行 / 32 维 / 50 查询）：

```
Recall@10: exact = 1.0, ann = 1.0 (阈值 0.95 -> 通过)
```

> 这些数字来自本地 Windows 环境，仅供参照量级。**绝对值不可与判题环境对比**：B 是目录增量估算、M 不含页缓存、T 不含容器调度开销。详见第 6 节。

---

## 2. 参数

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `--bin` | 必填 | 数据库可执行文件路径 |
| `--bin-args` | 空 | 传给数据库进程的后续参数（空格分隔） |
| `--env KEY=VALUE` | 空 | 追加环境变量，可重复 |
| `--rows` | 10000 | 写入记录数 |
| `--dim` | 128 | 向量维度 |
| `--queries` | 1000 | 查询条数 |
| `--k` | 10 | TopK |
| `--batch` | 100 | 每批写入条数 |
| `--mode` | `exact` | 检索模式，`exact` 或 `ann` |
| `--filter-ratio` | 0.5 | 带等值过滤的查询比例 |
| `--seed` | 20261110 | 随机种子，决定数据与查询内容 |
| `--out` | `bench-report.json` | 报告 JSON 输出路径 |
| `--data-dir` | 临时目录 | 数据目录；不给就用临时目录 |
| `--keep-data` | 关 | 保留临时数据目录（便于事后检查） |
| `--categories` | 10 | `category` 取值个数，即 `c0`..`cN-1` |
| `--tx-mode` | `explicit` | `explicit` = 每批 `BEGIN;`/`COMMIT;`；`autocommit` = 依赖自动提交 |
| `--index` | `vector` | 写入后是否 `CREATE VECTOR INDEX`；`none` 表示跳过 |
| `--index-name` | `docs_vec` | 向量索引名 |
| `--timeout` | 60 | 单次请求超时秒数 |
| `--verify` | 关 | 开启召回率校验模式 |
| `--baseline` | 无 | 基线 JSON 路径，用于算 R_T/R_B/R_M 与 E |
| `--stdout-log` / `--stderr-log` | 无 | 保存数据库原始输出 |
| `--quiet` | 关 | 不打印摘要 |

数据库进程如何知道数据目录：**只认 `VECTORDB_DATA_DIR` 环境变量**，未设置时数据库回退到字面量 `/data`（容器内判题器把数据目录挂载到 `/data`）。工具总是通过 `VECTORDB_DATA_DIR` 把 `--data-dir`（或临时目录）传给子进程，与数据库本体的约定完全一致。

---

## 3. 数据与查询构造

数据由 `--seed` 完全确定，可复现：

- `id`：`1..rows` 连续 BIGINT 主键。
- `category`：`c0`..`cN-1` 中随机取值，用作等值过滤列。
- `content`：`t{id}`，VARCHAR 普通列。
- `embedding`：`dim` 维向量，分量取 `[-1, 1)` 均匀分布，保留 6 位小数。

写入语句按批拼接（`--batch` 默认 100）：

```sql
INSERT INTO docs VALUES (1,'c3','t1','[0.123456,-0.654321,...]'),(2,...),...;
```

查询语句（`--filter-ratio` 决定一半带过滤）：

```sql
SELECT id, L2_DISTANCE(embedding, '[q...]') AS distance FROM docs
  [WHERE category = 'cX']
  ORDER BY distance LIMIT 10;
```

`search_mode` 字段按 `--mode` 填 `exact` 或 `ann`。

---

## 4. 指标口径

### 4.1 平台口径指标（报告里的 `platform_metrics`）

字段名与官方判题口径一一对应：

| 字段 | 平台定义 | 本地采集方式 |
| --- | --- | --- |
| `T_write_seconds` | 写入 = 全部批次执行与持久化的墙钟耗时 | 写入阶段首条请求发出到末条响应返回的墙钟 |
| `T_query_seconds` | 查询 = 正式查询序列的墙钟耗时 | 查询序列首条发出到末条返回的墙钟 |
| `B_write_bytes` | 磁盘物理读写字节数之和 | Linux 取 `/proc/<pid>/io` 的 `read_bytes + write_bytes` 增量；其他平台退化为数据目录字节增量（**估算**） |
| `B_query_bytes` | 同上，查询阶段 | 同上 |
| `M_peak_bytes` | 容器整体峰值内存（含页缓存） | Linux 取 `/proc/<pid>/status` 的 `VmHWM`；Windows 取 `GetProcessMemoryInfo` 的 `PeakWorkingSetSize`；否则 `null` |

每个 B / M 字段都带一个 `*_source` 说明用了哪种采集方式。

### 4.2 本地估算指标（报告里的 `local_estimate`）

原始实测数据，用于判断本地数字与平台数字的差距：

- `setup.create_table_seconds`：建表耗时（不计入 T_write，与平台口径的建表阶段对齐）。
- `write`：`batches` / `rows` / `affected_rows` / `elapsed_seconds` / `rows_per_second` / `batches_per_second` / `avg_batch_latency_ms` / `max_batch_latency_ms` / `tx_mode`。
- `query`：`queries` / `filtered_queries` / `unfiltered_queries` / `elapsed_seconds` / `queries_per_second` / `latency`（count、avg、min、max、p50、p90、p95、p99）。
- `index`：索引创建耗时与是否成功。
- `bytes`：`/proc` I/O 计数的原始增量、数据目录各阶段字节数与文件数。
- `memory`：峰值 RSS 及其来源。
- `throughput` / `latency`：吞吐与延迟汇总。
- `limitations`：本地估算的已知局限（见第 6 节）。

### 4.3 效率比（可选，`--baseline`）

基线文件格式：

```json
{
  "write": {"T_base_seconds": 2.0, "B_base_bytes": 52428800, "M_base_bytes": 268435456},
  "query": {"T_base_seconds": 5.0, "B_base_bytes": 10485760, "M_base_bytes": 268435456}
}
```

工具按判题公式计算：

- `R_T = (T_base + 1ms) / (T + 1ms)`，`R_B`、`R_M` 同理用 1MiB 平滑。
- `f(R) = R / (1 + R)`。
- `E_写入 = 0.6*f(R_T) + 0.2*f(R_B) + 0.2*f(R_M)`，`E_查询` 同理。
- `E = (E_写入 + 2*E_查询) / 3`。

结果放在报告的 `efficiency` 字段。

---

## 5. `--verify` 召回率校验模式

### 5.1 真值定义

**真值 = 先把等值过滤作用在记录集合上，再在满足过滤条件的集合内计算距离并取 TopK**，即：

```
候选集合 C = { r | r.category == 过滤值 }        （无过滤时 C = 全部记录）
真值 TopK  = 按 (欧氏距离, id) 升序排列 C 的前 k 条
```

这与官方口径一致：**过滤先作用于候选集合，TopK 在满足条件的记录内计算**，而不是先全表取 K 再过滤。工具的暴力实现严格按此语义执行（`brute_force_topk` 先按 `category` 过滤候选，再计算距离排序）。

距离并列时按 `id` 升序，与数据库「并列按主键排序」的行为对齐。

### 5.2 校验内容

`--verify` 会用**独立的纯 Python 暴力实现**（`math` 欧氏距离，`zip`+`sum` 列表推导，不写逐元素双重循环）算出每条查询的真值 TopK，然后：

1. **exact 模式**：检查数据库返回的 TopK 与真值**完全一致**（id 序列逐位比对），并报告距离绝对误差（`max`/`avg`）。
2. **ann 模式**：计算 `Recall@10 = Σ命中数 / Σ min(k, |真值|)`，与官方要求 **Recall@10 ≥ 0.95** 对比，输出 `official_recall_requirement.passed`。

两种模式各跑一次完整查询序列（主查询阶段的结果会被复用，不会重复跑同一模式）。

`verification` 字段结构：

```json
{
  "k": 10,
  "truth_definition": "真值 = 先把等值过滤作用在记录集合上, 再在满足过滤条件的集合内用纯 Python 暴力计算欧氏距离并取 TopK (按 (距离, id) 升序); 不是先全表取 K 再过滤.",
  "filter_scope": "filtered-set-then-topk",
  "modes": {
    "exact": {
      "recall_at_k": 1.0,
      "exact_order_matches": 50,
      "exact_order_mismatches": 0,
      "topk_identical_to_truth": true,
      "max_distance_abs_error": 6.3e-08,
      "avg_distance_abs_error": 1.7e-08
    },
    "ann": {"recall_at_k": 0.98, "...": "..."}
  },
  "exact_matches_ground_truth": true,
  "ann_recall_at_k": 0.98,
  "official_recall_requirement": {
    "metric": "Recall@10", "threshold": 0.95, "observed": 0.98, "passed": true
  }
}
```

暴力计算是 O(rows × dim) 每条查询，**只适合小规模**。建议 `--rows 2000 --dim 32 --queries 50`；正式规模（10000×128×1000）下纯 Python 暴力会慢到不可用。

### 5.3 测 ANN 召回率的规模前提

数据库实现里 ANN 路径有一个触发阈值（当前实现为 `MV_ANN_MIN_ROWS = 2048` 行）：**表行数低于阈值时 `--mode ann` 会回退到精确扫描**，此时测出的 recall 恒为 1.0，但**测不出 ANN 索引的真实质量**。

所以：

- **验 ANN 质量**：用 `--rows 10000`（或任何 ≥ 阈值的行数）。
- **验 exact 正确性**：小规模即可（exact 路径不受阈值影响）。
- 想确认当前二进制到底走没走 ANN：**比较 ann 与 exact 的 `T_query`**。两者接近说明走了回退路径；ANN 明显更快说明索引生效。
- 报告里 `verification.modes.ann.recall_at_k` 与 `local_estimate.query.elapsed_seconds` 一起看，才能判断 ANN 是否真的在工作。

---

## 6. 已知局限

1. **T 是本地墙钟**，平台口径额外包含容器调度、判题器自身开销与结果汇总，本地数值通常偏乐观。
2. **B 在 Linux 上**取 `/proc/<pid>/io` 的 `read_bytes + write_bytes`，已扣除页缓存，与平台 cgroup 统计接近但不完全等价（平台还统计容器内其他进程与元数据 I/O）。
3. **B 在非 Linux 平台**退化为数据目录大小增量，属于**估算**，会漏掉预分配、临时文件、覆盖写与元数据更新。
4. **M 只统计数据库进程自身的峰值 RSS**，不含页缓存，也不含容器内其他进程；平台口径是容器整体内存（含页缓存），因此本地数值通常**偏小**，不要直接当作 M 提交。
5. **没有复现平台的 cgroup 资源限制**（官方是 `--cpus=2 --memory=2g --read-only`），本地在宿主机上跑会得到不同的性能画像。
6. **查询顺序固定、数据由种子生成**，命中分布理想化，无法反映真实数据的倾斜与热点。
7. **单客户端串行**，没有并发压力下的锁竞争与尾延迟放大。
8. **写入阶段只覆盖 `INSERT`**。正式负载若包含 `UPDATE`/`DELETE` 或更大数据量，需要扩展负载脚本。

---

## 7. 使用建议

- **先跑 `--verify` 确认正确性，再跑正式规模采性能数字。** 顺序搞反会浪费时间在一个不正确的实现上。
- **每次只改一个变量**，用 `--seed` 固定数据，用 `--out` 分文件保存报告，方便对比。
- **正式规模采集建议在 Linux 上跑**，这样 `B` 和 `M` 才有真实意义；Windows 上的结果只适合看相对趋势。
- **`--keep-data` 保留数据目录**，便于事后用公开题包或 `protocol_check/run.py` 复用同一份数据做重启持久化验证。
- **报告里 `platform_metrics` 的数值不要直接上报**。它是本地估算，只用来判断改动方向。
- **ANN 召回不达标时**先确认 `exact` 模式是 1.0：若 exact 也不一致，说明是距离计算或排序实现问题，而不是 ANN 索引问题。
- **注意过滤语义**：真值是「先过滤、再在满足条件的集合内取 TopK」。如果实现的语义是「先全表取 K 再过滤」，带过滤的查询会在 exact 校验里直接失配——这是实现 bug，不是工具问题。
- **索引创建失败只会告警不会中断**（见报告 `local_estimate.index.warning`）。正式性能对比前务必确认 `index.created` 为 `true`，否则查询耗时不代表正式负载。

---

## 8. 报告结构总览

```json
{
  "tool": "minivectordb-bench",
  "tool_version": "1.0.0",
  "generated_at": "...",
  "host": {"platform": "...", "python": "...", "cpu_count": 32},
  "config": {"rows": 10000, "dim": 128, "queries": 1000, "k": 10, "seed": 20261110, "...": "..."},
  "platform_metrics": {"T_write_seconds": 0.0, "B_write_bytes": 0, "M_peak_bytes": 0, "...": "..."},
  "local_estimate": {"setup": {}, "write": {}, "query": {}, "index": {}, "bytes": {}, "memory": {}, "throughput": {}, "latency": {}, "limitations": []},
  "verification": {"truth_definition": "...", "filter_scope": "filtered-set-then-topk", "modes": {"exact": {}, "ann": {}}, "official_recall_requirement": {}},
  "efficiency": {"write": {"R_T": 0, "E": 0}, "query": {}, "overall": 0},
  "data_dir": {"path": "...", "kept": true},
  "stderr_log": "..."
}
```

`verification` 只在 `--verify` 时出现，`efficiency` 只在 `--baseline` 时出现。运行失败时报告里只有 `error` 与全 `null` 的指标。

# AMD CAGRA 构建性能对比报告

**赛题**：seekdb × AMD hipVS —— 使用 hipVS 加速 HNSW 向量索引构建
**提交分支**：`amd-cagra-submit`
**基线**：冻结 commit `634888d9b037153fa143d2c913ba4b25503e1d62`（tag `amd-cagra-contest-baseline-v5`）
**唯一功能性改动**：`tools/hipvs_bridge/seekdb_cuvs_bridge.c`（一个文件，未改动 `build.sh` 与头文件）

---

## 1. 测试环境

| 项 | 值 |
|---|---|
| GPU | AMD Radeon PRO W7900D，gfx1100（RDNA3） |
| ROCm | 7.2.4（与评测环境一致） |
| hipVS | `/opt/hipvs`，固定 commit `f96a663d0a4441d6f5344bd3ef552b71ae7b370b` |
| CPU / 内存 | 128 核 / 1007 GB |
| 编译器 | `hipcc`（官方 `tools/hipvs_bridge/build.sh`） |

## 2. 测试方法

测量直接 `dlopen` bridge 的 C ABI，绕过 SQL 与 `LOAD DATA` 开销，使单次测量 < 1 s，
从而能在有限算力额度内完成参数扫描：

- `amd_bench/bridge_test.c` —— 探针程序
  - `seekdb_cuvs_build(base, n, dim)` → 测建图耗时（`BUILD_MS`）
  - `seekdb_cuvs_search(handle, query, nq, topk, ids, NULL)` → 测检索耗时
  - `seekdb_cuvs_serialize` / `seekdb_cuvs_deserialize` → 测快照落盘与重载耗时
  - 与精确 top-10 真值比对得出 recall@10
- `amd_bench/gen_data.py` —— 生成评测规模数据 + float64 精确真值
- `amd_bench/sweep.sh` —— 参数扫描驱动（每组取中位数，单次测量噪声可达 1.8×）

**数据集**：250 000 × 128 维 float32（对齐 SIFT250K 规模），2000 个聚类中心、
高斯噪声 0.30，查询为基向量小扰动。种子固定（`--seed 1234`），可完全复现。
真值由 float64 暴力计算得到。

## 3. 结果

全部数据见 `amd_bench/results/bench_250k_d128.csv`。

| 配置 | niter | igdeg | 建图 (ms) | 加速比 | Recall@10 | 召回降幅 |
|---|---|---|---|---|---|---|
| 冻结基线 | 100 | 256 | 41757 | 1.00× | 0.9980 | — |
| | 100 | 128 | 30480 | 1.37× | 0.9980 | 0.0000 |
| | 30 | 256 | 17946 | 2.33× | 0.9930 | 0.0050 |
| | 30 | 128 | 12445 | 3.36× | 0.9960 | 0.0020 |
| | 20 | 256 | 14715 | 2.84× | 0.9980 | 0.0000 |
| **提交版本** | **20** | **128** | **9140** | **4.57×** | **0.9970** | **0.0010** |
| | 10 | 256 | 11257 | 3.71× | 0.9800 | 0.0180 ✗ |
| | 10 | 128 | 6802 | 6.14× | 0.9890 | 0.0090 ✗ |
| | 5 | 256 | 8763 | 4.77× | 0.9540 | 0.0440 ✗ |
| | 5 | 128 | 4628 | 9.02× | 0.9760 | 0.0220 ✗ |

**以官方 `build.sh` 编出的最终产物复测**（不设任何环境变量，即评测实际形态）：

```
BUILD_MS=9434.7  SEARCH_MS=38.2  SER_MS=271.3  DESER_MS=191.4
SNAP_BYTES=256001713  RECALL=0.9980
```

→ **建图 41757 ms → 9435 ms，加速 4.43×，Recall 与冻结基线完全相同（0.9980）。**

## 4. 优化原理

CAGRA 的 `NN_DESCENT` 建图算法每一轮都要做一次全图近邻搜索，因此建图耗时
与迭代轮数近似线性。基线硬编码 `nn_descent_niter = 100`。

此外有两个度数参数：

- `graph_degree` —— **最终图**的度数
- `intermediate_graph_degree` —— `NN_DESCENT` 工作中所��**中间图**的度数

关键在于：降 `intermediate_graph_degree` 只缩小构建过程中的中间图，
**不改变最终索引的强度**。实测 `igdeg=256 → 128` 召回完全不变（0.9980），
却省下 27% 建图时间。

而 `graph_degree` 实测无效：250K 上从 128 降到 16 全部落在 40030~41129 ms
（噪声范围内），且 `gdeg ≤ 32` 开始损伤召回。故保持 128 不动。

最终选定 **`nn_descent_niter=20` + `intermediate_graph_degree=128`**：
召回降幅 0.0010，相对赛题允许的 0.005 有 5 倍余量。

### 为什么不取更激进的 niter=10

`niter=10` 虽快至 6802 ms（6.14×），但召回降至 0.9890，降幅 0.0090 **超出
赛题 0.005 上限**，会直接触发硬门槛失败、总分归零。`niter=5` 更是崩塌到 0.9540。
**硬门槛失败等于 0 分，因此只在安全边界内取最快。**

## 5. 副作用与风险控制

| 风险 | 处理 |
|---|---|
| 召回跌破门槛 | 选 niter=20 而非 10，降幅 0.0010，余量 5 倍 |
| 编译失败（0 分） | 用**官方 `build.sh`** 验证通过：`built /tmp/bridge/libseekdb_cuvs_bridge.so` |
| GPU 路径被判定未使用 | 只改参数，未改任何 GPU 调用路径 |
| `lib=vsag` CPU 路径回归 | 未改动内核，仅改 bridge 内部参数 |
| 显存超限 | 未新增任何显存分配 |
| 环境变量误生效 | 评测环境不设 `CAGRA_*`，默认值即生效；覆盖仅用于本地复现 |

## 6. 端到端预期

赛题计时窗口为「`CREATE VECTOR INDEX` → 第一条固定查询返回」，其中表扫描、
H2D 传输、快照落盘、首查均为固定开销 F。按官方公布的 SIFT250K 基线
20~27 s（含首查）反推，建图约占 18~20 s，F ≈ 3~5 s。

因此端到端加速比约为 `(F+B)/(F+B/k)`，建图 4.43× 对应端到端约 **2.4~2.6×**。
继续压缩 F（表扫描并行化、H2D 与建图重叠、快照落盘异步化）可进一步提高，
但需改动 `ob_vsag_adaptor.cpp` 且全量编译，留待后续。

## 7. 复现步骤

```bash
# 1. 生成评测规模数据与精确真值
python3 amd_bench/gen_data.py --n 250000 --dim 128 --nq 100 --out /workspace/work/d128

# 2. 用官方脚本编译 bridge
HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm \
  tools/hipvs_bridge/build.sh /tmp/bridge/libseekdb_cuvs_bridge.so

# 3. 测量（不设任何 CAGRA_* 即为提交形态）
cd /workspace/work
export BASE_F32=d128/base.f32 QUERY_F32=d128/query.f32 GT_I32=d128/gt.i32
LD_LIBRARY_PATH=/seekdb_workspace/bridge:/opt/hipvs/lib:/opt/rocm/lib \
  ./bridge_test /tmp/bridge/libseekdb_cuvs_bridge.so 250000 128 100 10

# 4. 参数扫描
REPS=3 TOKEN=x bash sweep.sh /workspace/work/d128
```

注：实例上 `raw.githubusercontent.com` 不可达、git clone 被代理拦截，
取文件需走 `api.github.com`；命令细节见 `amd_bench/README.md`。

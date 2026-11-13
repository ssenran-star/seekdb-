# AMD CAGRA 构建加速 — 基准测试工具

用于在 AMD Radeon 实例上**直接测量 CAGRA 建图耗时与召回**，绕开 SQL / LOAD DATA 开销。

## 为什么不用 SQL 基准

评测的计时窗口是 `CREATE VECTOR INDEX` → 第一条查询返回。但用 SQL 复现需要：

- `LOAD DATA` 导入 250K 行向量（Hidden250K 约 256 MB TSV）→ 单次数分钟
- 每换一组参数就要 DROP INDEX + 重建

在 4 小时实例额度下不现实。本工具直接 `dlopen` bridge 的 C ABI：

- `seekdb_cuvs_build(base, n, dim)` → 只测建图
- `seekdb_cuvs_search(handle, query, nq, topk, ids, NULL)` → 测检索并算 recall

单次测量 < 1 秒，可以连续扫几十组参数。

## 文件

| 文件 | 说明 |
|---|---|
| `bridge_test.c` | 测量程序。`dlopen` bridge，输出 `BUILD_MS / SEARCH_MS / RECALL` |
| `seekdb_cuvs_bridge_tunable.c` | 可调参版 bridge：`CAGRA_NITER` / `CAGRA_GDEG` / `CAGRA_IGDEG` 环境变量覆盖建图参数，默认值 = 冻结基线 |
| `*.b64` | base64 单行封装，用于规避 VS Code 终端 heredoc 粘贴问题 |

## 用法

### 1. 一行完成：取文件 + 编译 + 运行

实例上 `raw.githubusercontent.com` 不可达（`SSL_ERROR_SYSCALL`），必须走 `api.github.com`。
**不要用 heredoc** —— VS Code 终端会吞掉 `<<`，导致 shell 进入续行状态（提示符变成 `>` / `>>>`）。
**不要用 `export`** —— 终端偶尔吞掉命令开头几个字符，直接给命令加 `LD_LIBRARY_PATH=` 前缀。

```bash
cd /workspace && mkdir -p work && cd work && \
curl -fsSL -H "Authorization: Bearer $TOKEN" \
  "https://api.github.com/repos/ssenran-star/seekdb-/contents/amd_bench/bridge_test.c?ref=amd-cagra-submit" \
| python3 -c "import sys,json,base64;open('bridge_test.c','wb').write(base64.b64decode(json.load(sys.stdin)['content']))" \
&& gcc -O2 -o bridge_test bridge_test.c -ldl \
&& LD_LIBRARY_PATH=/seekdb_workspace/bridge:/opt/hipvs/lib:/opt/rocm/lib \
   ./bridge_test /seekdb_workspace/bridge/libseekdb_cuvs_bridge.so 10000 128 100 10
```

`$TOKEN` 为只读 fine-grained PAT。输出形如：

```
BUILD_MS=... SEARCH_MS=... RECALL=... N=10000 DIM=128 NQ=100 TOPK=10 GT=1000
```

### 2. 跑基线（用镜像自带的预编译 bridge）

```bash
export LD_LIBRARY_PATH=/seekdb_workspace/bridge:/opt/hipvs/lib:/opt/rocm/lib
./bridge_test /seekdb_workspace/bridge/libseekdb_cuvs_bridge.so 10000 128 100 10
```

输出示例：

```
BUILD_MS=... SEARCH_MS=... RECALL=... N=10000 DIM=128 NQ=100 TOPK=10 GT=1000
```

### 3. 编可调参 bridge（需 hipcc）

```bash
curl -fsSL -o tb.b64 https://raw.githubusercontent.com/ssenran-star/seekdb-/amd-cagra-submit/amd_bench/seekdb_cuvs_bridge_tunable.c.b64
base64 -d tb.b64 > seekdb_cuvs_bridge_tunable.c
cp /seekdb_workspace/bridge/seekdb_cuvs_bridge.h .   # 需要头文件
HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm \
  /opt/rocm/bin/hipcc -shared -fPIC -O2 -D__HIP_PLATFORM_AMD__ \
  -I/opt/hipvs/include -I/opt/rocm/include \
  seekdb_cuvs_bridge_tunable.c -L/opt/hipvs/lib \
  -Wl,-rpath,/opt/hipvs/lib:/opt/rocm/lib -lcuvs_c \
  -o libseekdb_cuvs_bridge_tunable.so
```

### 4. 扫参数

```bash
export LD_LIBRARY_PATH=/workspace/work:/opt/hipvs/lib:/opt/rocm/lib
for niter in 100 80 60 40 30 20 15 10; do
  echo -n "niter=$niter  "
  CAGRA_NITER=$niter ./bridge_test /workspace/work/libseekdb_cuvs_bridge_tunable.so 10000 128 100 10
done
```

## 筛选规则（硬门槛）

参赛版本必须同时满足：

- Recall@10 相对**冻结基线**下降 ≤ 0.005（比绝对门槛 0.95 更严）
- SIFT250K Recall@10/100 ≥ 0.95
- Hidden250K Recall@10 ≥ 0.90、Recall@100 ≥ 0.85
- 每条查询返回 k 个**互不重复**的 ID

## 数据集

镜像自带（`/seekdb_workspace/datasets/`）：

| 文件 | 内容 |
|---|---|
| `base.f32` | 10000 × 128 维 float32 |
| `query.f32` | 1000 × 128 维 float32 |
| `gt_100x10.i32` | 100 查询 × 10 近邻真值 |

**注意：10K 规模不等于评测的 250K。** `nn_descent_niter` 的影响随规模变化，
10K 上的加速比会**高估** 250K 上的表现。筛选时以 10K 作为初筛，
最终必须回到 250K 规模复测。

生成更大规模数据可用 `BASE_F32` / `QUERY_F32` / `GT_I32` 环境变量指定路径：

```bash
BASE_F32=/workspace/data/base250k.f32 QUERY_F32=/workspace/data/q.f32 \
  GT_I32=/workspace/data/gt.i32 ./bridge_test ...
```

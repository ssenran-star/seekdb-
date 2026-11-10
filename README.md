# MiniVectorDB —— OceanBase 2026 竞赛参赛实现

一个用 **C11、零第三方依赖**实现的单机轻量向量数据库:SQL 子集 + 向量 TopK 检索 +
事务 + 崩溃恢复,通过 stdin/stdout 的 JSON Lines 协议与测试驱动通信。

- 赛题分支:`2026-competition-round2`
- 实现:`minivectordb/`(源码 `src/`、头文件 `include/`、基准工具 `tools/`)
- 本地验证:`scripts/`(公开测试运行器、SIGKILL 崩溃恢复测试)

## 1. 语言与版本

| 项 | 说明 |
| --- | --- |
| 语言 | C11(POSIX 200809),无任何第三方库 |
| 编译器 | gcc ≥ 12(镜像内 `gcc:13`);本地 Windows 开发用 `python -m ziglang cc` 交叉验证 |
| 构建 | `minivectordb/Makefile` 或根目录 `Dockerfile`(多阶段) |
| 运行平台 | Linux x86_64 容器(正式);代码层兼容 Windows/POSIX |

## 2. 构建与启动

### 2.1 Docker(正式评测路径)

```bash
docker build -t minivectordb .
docker run --rm -i \
  --read-only \
  --cpus=2 --memory=2g \
  -v "$(pwd)/data:/data" \
  minivectordb
```

镜像为两阶段构建:`gcc:13` 编译 → `debian:bookworm-slim` 运行,运行阶段不联网、
无运行时依赖。容器以前台方式运行,stdin 接请求、stdout 回结果、stderr 收日志。

### 2.2 本地构建

```bash
cd minivectordb && make            # 产物 build/minivectordb
# 或(无 make 环境时,任意 C11 编译器):
cc -std=c11 -O3 -D_POSIX_C_SOURCE=200809L -I include src/*.c -o build/minivectordb
```

数据目录由环境变量 `VECTORDB_DATA_DIR` 指定,缺省 `/data`。

## 3. 协议

完整协议见 `STDIO_PROTOCOL.md`。摘要:

1. 启动后完成恢复,输出 `{"type":"ready"}` 并 flush;
2. 每行一个请求 `{"id":N,"op":"execute","sql":"...","search_mode":"exact|ann"}`,恰回一个响应并原样回传 `id`;
3. `{"id":N,"op":"shutdown"}` → `{"id":N,"ok":true}` → 回滚未提交事务、落盘、退出码 0;
4. stdout 只有协议报文;诊断全部写 stderr;每次输出立即 flush。

## 4. SQL 支持范围

```sql
CREATE TABLE t (id BIGINT PRIMARY KEY, note VARCHAR(32), v VECTOR(4));
CREATE VECTOR INDEX idx_t_v ON t (v);
INSERT INTO t VALUES (1, 'a', '[1,2,3,4]'), (2, 'b', '[4,3,2,1]');
SELECT * FROM t WHERE note = 'a' AND id >= 1 ORDER BY id LIMIT 10;
SELECT id, L2_DISTANCE(v, '[1,0,0,0]') AS d FROM t WHERE note = 'a'
  ORDER BY d LIMIT 10;
UPDATE t SET note = 'c' WHERE id = 2;
DELETE FROM t WHERE id < 10;
BEGIN; ... COMMIT; / ROLLBACK;
```

- 每表恰好一个 BIGINT 主键、至多一个 VECTOR 列;VARCHAR(n) 长度按 Unicode 字符数计;
- 条件仅支持普通列的 `= < <= > >=` 与 `AND` 组合;ORDER BY 并列项按主键升序;
- 过滤 TopK 在满足 WHERE 的集合内计算(先过滤后取 K),距离并列按主键升序;
- `search_mode=ann` 时使用近似索引(行数 ≥ 2048 时生效),exact 恒为精确扫描。

### 错误码

| code | 场景 |
| --- --- |
| `duplicate_key` | 主键冲突 |
| `dimension_mismatch` | 向量维度与列定义不符 |
| `unsupported_sql` | JOIN/聚合/NULL/嵌套事务等不支持语法 |
| `syntax_error` | 无法解析的语句 |
| `table_not_exists` / `column_not_exists` | 名字解析失败 |

语句失败不留部分修改(先校验后写入);显式事务中已成功的语句仍可提交或回滚。

## 5. 从空目录开始的操作示例

```bash
mkdir -p /tmp/mv && VECTORDB_DATA_DIR=/tmp/mv ./build/minivectordb
```

```text
>> {"type":"ready"}
>> {"id":1,"op":"execute","sql":"CREATE TABLE t (id BIGINT PRIMARY KEY, note VARCHAR(32), v VECTOR(2))"}
<< {"id":1,"ok":true}
>> {"id":2,"op":"execute","sql":"BEGIN"}
<< {"id":2,"ok":true}
>> {"id":3,"op":"execute","sql":"INSERT INTO t VALUES (1,'a','[1,0]'),(2,'b','[0,1]')"}
<< {"id":3,"ok":true,"affected_rows":2}
>> {"id":4,"op":"execute","sql":"COMMIT"}
<< {"id":4,"ok":true}
>> {"id":5,"op":"execute","sql":"SELECT id, L2_DISTANCE(v, '[1,1]') AS d FROM t WHERE note = 'a' ORDER BY d LIMIT 1","search_mode":"exact"}
<< {"id":5,"ok":true,"columns":["id","d"],"rows":[[1,1.0]]}
>> {"id":6,"op":"execute","sql":"ROLLBACK"}
>> {"id":7,"op":"shutdown"}
<< {"id":7,"ok":true}      # 进程退出码 0;重启后 id=1、2 仍在
```

## 6. 简要设计与验证记录

### 6.1 存储布局(单目录、四类文件 + 文本 catalog)

```
<data>/catalog.txt     # 文本 catalog:表/列/索引定义(T/C/I 行),启动时重建 schema
<data>/<table>.rows    # 追加式行日志:[len:4][txn:8][flags:1][pk:8][slot:8][payload][crc:4]
<data>/<table>.vec     # 稠密 float32 向量文件,按 slot 定位(定长可随机读)
<data>/<table>.commit  # 事务提交标记:已持久化到哪个 txn_id
```

- 写入均为追加 + `fsync`,不原地改写;CRC32 检测撕裂写;
- 恢复 = 读 catalog → 重放行日志(按已提交 txn_id 过滤)→ 截断未提交尾部 → 重建内存索引;
- 自动提交 = 单语句事务;`COMMIT` 响应前先落盘提交标记,保证已确认事务不丢。

### 6.2 查询执行

- 距离:4 路独立 double 累加器打破依赖链,-O3;
- 精确 TopK:二叉小顶堆保留 K 个候选(并列按主键升序决胜);
- 过滤 TopK:两遍扫描——第一遍标 WHERE 位图,第二遍只对命中 slot 算距离;
- 热向量缓存(上限 32 MiB,几何扩容):小数据集时向量扫描不读盘。

### 6.3 近似索引(ann 模式)

- 单层 NSW 图(M=24),质心作入口点,best-first 搜索,`ef = max(64, 4K)`;
- 行数 ≥ 2048 才启用,否则退回精确扫描(正确性优先);
- 建索引后 INSERT/UPDATE/DELETE 即时反映(失效重建图);
- 官方规模(10k 行 × 128 维)本地 Recall@10 = 1.0(exact 与 ann 相同)。

### 6.4 事务

- 同一时刻一个显式事务;内存 undo 栈支持 ROLLBACK 精确回退;
- 事务内查询与向量检索都能看到自己的修改(先查内存态再落盘)。

### 6.5 验证记录(本地,Windows + zig cc)

| 项目 | 结果 |
| --- | --- |
| 公开测试 20/20 | 全部 PASS(`scripts/run_public_tests.py`,含重启/事务/恢复) |
| SIGKILL 崩溃恢复 | PASS:已提交存活,未提交事务回滚(`scripts/crash_kill_test.py`) |
| 官方规模 bench(10k×128,1000 查询,K=10,一半带过滤) | T_write ≈ 0.78 s,T_query ≈ 0.83 s(exact),M_peak ≈ 17–22 MiB |
| Recall@10 | exact = 1.0,ann = 1.0(阈值 0.95) |

正式成绩以平台对锁定 commit 的 cgroup 统计为准,本地数字仅作开发参考。

### 6.6 已知限制

- 并发:单客户端,不支持多连接(赛题允许);
- 事务内向量修改的 ANN 检索:行数 < 2048 时自动走精确路径,无正确性差异;
- `B_write` 依赖追加式日志,更新/删除不回收空间(靠重启重放可物理紧凑化,未实现);
- ORDER BY 仅支持单列(赛题范围)。

## 7. 目录结构

```
minivectordb/         # 数据库实现(C11)
  include/  src/      # db.h/os.c/util.c/json.c/sql.c/store.c/vector.c/exec.c/main.c
  tools/bench.py      # 本地基准工具(官方规模负载、Recall 验证)
  Makefile
scripts/              # 本地验证:公开测试运行器、SIGKILL 恢复测试
protocol_check/      # 题包自带(官方)
agent/conversation.json   # Agent prompt 评价材料(赛题 4.4)
Dockerfile            # 两阶段构建
```

# MiniVectorDB 运行说明

## 语言与版本

- 实现语言：C++17（开发验证环境 g++ 15.2.0，Ubuntu 26.04；CMake ≥ 3.16）。
- 唯一第三方依赖：nlohmann/json v3.11.3（单头文件，已锁定并随仓库提交于 `third_party/nlohmann/json.hpp`）。
- 存储、SQL 执行、事务、恢复与向量索引（HNSW）全部自行实现，未使用任何现成数据库引擎或向量索引库。

## 镜像构建

```bash
docker build -t minivectordb:latest .
```

Dockerfile 为多阶段构建：`ubuntu:24.04` 构建阶段安装 g++/cmake/make 并从源码编译；运行阶段仅保留二进制与 libstdc++6。正式执行无需联网。

## 启动入口

```bash
docker run --rm -i \
  -v minivectordb-data:/data \
  --cpus=2 --memory=2g \
  minivectordb:latest
```

- 入口：`/app/minivectordb --data-dir /data`（数据目录可用 `--data-dir` 覆盖）。
- 程序前台运行，通过 stdin/stdout 的 JSONL 协议与测试驱动通信（见 `STDIO_PROTOCOL.md`）：启动完成恢复后主动输出 `{"type":"ready"}`；随后逐行处理 `execute` / `shutdown` 请求；日志只写 stderr。
- 不监听任何网络端口，无需 EXPOSE。

## /data 数据目录

所有持久化文件写入 `/data`（当前实现为单一日志文件 `mvdb.log`，含表定义、数据版本与事务提交标记）。每个用例应从空目录开始；重启复用原目录，程序不会在启动时清空数据。

## SQL 支持范围

- `CREATE TABLE`：BIGINT、VARCHAR(n)、VECTOR(n)；一个 BIGINT 主键，每表至多一个向量列。
- `INSERT`（多行）、`SELECT`、`UPDATE`、`DELETE`；比较运算符 `= < <= > >=`、`AND`、`ORDER BY [ASC|DESC]`、`LIMIT`、`SELECT *`、列投影。
- `L2_DISTANCE(vector_col, '[...]')`（欧氏距离），支持 `AS` 别名与按距离排序的 TopK；先过滤后 TopK，距离并列按主键升序。
- `BEGIN` / `COMMIT` / `ROLLBACK`；未显式 BEGIN 时语句自动提交；事务内可见自身修改。
- `CREATE VECTOR INDEX name ON table(vector_col)`：自实现 HNSW；`search_mode=ann` 时用于向量 TopK，`exact` 始终精确扫描。
- 不支持：NULL、JOIN、聚合、子查询、ALTER TABLE、外键、嵌套事务、主键更新；不支持的语句返回错误而非静默执行。

## 错误处理

失败响应 `{"id":N,"ok":false,"error":{"code":"...","message":"..."}}`，进程继续运行。已使用的语义错误码：`duplicate_key`、`unsupported_sql`、`dimension_mismatch`、`syntax_error`、`table_not_found`、`column_not_found`、`table_exists`、`index_exists`、`invalid_vector`（NaN/Infinity/溢出）、`varchar_too_long`、`type_mismatch`、`bigint_overflow`、`transaction_active`、`no_transaction`、`internal_error`。

## 从空数据目录开始的操作示例

准备请求文件 `requests.jsonl`：

```jsonl
{"id":1,"op":"execute","sql":"CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(32), embedding VECTOR(4));","search_mode":"exact"}
{"id":2,"op":"execute","sql":"INSERT INTO docs VALUES (1,'db','[1,0,0,0]'),(2,'db','[0,1,0,0]'),(3,'travel','[0,0,1,0]');","search_mode":"exact"}
{"id":3,"op":"execute","sql":"SELECT id, L2_DISTANCE(embedding,'[0.9,0.1,0,0]') AS distance FROM docs WHERE category='db' ORDER BY distance LIMIT 2;","search_mode":"exact"}
{"id":4,"op":"execute","sql":"BEGIN;","search_mode":"exact"}
{"id":5,"op":"execute","sql":"UPDATE docs SET embedding='[0.8,0.2,0,0]' WHERE id=1;","search_mode":"exact"}
{"id":6,"op":"execute","sql":"ROLLBACK;","search_mode":"exact"}
{"id":7,"op":"execute","sql":"SELECT id, embedding FROM docs ORDER BY id;","search_mode":"exact"}
{"id":8,"op":"shutdown"}
```

运行（空数据目录）：

```bash
mkdir -p demo-data
docker run --rm -i -v "$PWD/demo-data:/data" --cpus=2 --memory=2g minivectordb:latest < requests.jsonl
```

预期：id=3 返回 `[[1,0.1414...],[2,1.0050...]]`（距离按 float32 存储值计算）；id=7 中 id=1 的向量仍为 `[1,0,0,0]`（回滚生效）。

重启验证持久化（复用同一目录，重新发送查询）：

```bash
printf '%s\n' '{"id":1,"op":"execute","sql":"SELECT id FROM docs ORDER BY id;","search_mode":"exact"}' '{"id":2,"op":"shutdown"}' \
  | docker run --rm -i -v "$PWD/demo-data:/data" minivectordb:latest
```

预期返回 3 条记录：已提交数据在重启后保留。

## 本地自测

```bash
# 本地进程（快速迭代）
python3 protocol_check/run.py \
  --command '["./build/minivectordb","--data-dir","{data_dir}"]' \
  --handshake-only --output protocol-results/handshake
python3 protocol_check/run.py \
  --command '["./build/minivectordb","--data-dir","{data_dir}"]' \
  --output protocol-results/public

# 正式镜像
export VECTORDB_IMAGE=minivectordb:latest
python3 protocol_check/run.py --image "$VECTORDB_IMAGE" --handshake-only --output protocol-results/docker-hs
python3 protocol_check/run.py --image "$VECTORDB_IMAGE" --output protocol-results/docker-public
```

## 设计与验证记录（简要）

- **记录布局**：单一日志结构文件 `/data/mvdb.log`。记录帧 = 长度 + (txn, stmt, type, payload) + 结束魔数；数据记录含表 id、主键、前一版本偏移与整行内容（BIGINT int64、VARCHAR UTF-8、VECTOR float32 数组）。主键索引为内存 `pk → 最新版本偏移`，版本链按偏移回退。
- **事务**：单显式事务。语句先整体校验（类型/维度/长度/主键冲突）再写版本，保证语句原子性；版本按 (txn, stmt) 判定可见性。COMMIT 追加提交标记并 `fdatasync` 后才响应；ROLLBACK/异常退出使未提交版本不可见。
- **恢复**：启动顺序扫描日志，重建表定义、主键索引与提交集合；无提交标记的事务整体回滚； torn 尾部按记录帧截断。向量索引在报告 ready 前从可见记录重建。
- **向量索引**：自实现 HNSW（M=16，efConstruction=200，查询 ef≥64）；增删改即时维护；ann 检索结果按可见性与 WHERE 过滤，候选不足 K 时回退精确扫描。
- **已知限制**：日志无 compaction，频繁更新会增大文件与恢复扫描量；DDL 不支持在显式事务内执行；部分列 INSERT 不支持；ANN 为召回率优先的保守参数，未做 SIMD 深度优化。
- **验证**（2026-11-12，Ubuntu 26.04 VM，32 核/14GiB，g++ 15.2）：
  - 源码编译通过；protocol_check 握手 + 全部 20 个公开用例（95 条 SQL）通过。
  - 崩溃恢复自测（tests/crash_test.py）：强杀回滚、重复恢复、提交持久性、索引重建、torn 尾部恢复全部通过。
  - 基准（tests/bench.py，10k 行/128 维/1000 查询/K=10 半过滤，本地进程）：写入 0.47s；精确查询 0.69s；ANN（建索引后）Recall@10=0.985。
  - Docker 镜像（ubuntu:24.04 多阶段源码构建）：握手 + 全部 20 个公开用例通过（2026-11-12）。
# MiniVectorDB · 第六届 OceanBase 数据库大赛加赛提交说明

单机、单进程、单客户端的轻量向量数据库实现。语言为 **Python 3.12+**，除可选的
`numpy==2.1.3`（仅作距离计算加速）外只使用标准库；存储、SQL 执行、向量检索、
事务与恢复均为自行实现，未使用 SQLite / RocksDB / Faiss / HNSWlib 等现成引擎。

## 1. 目录

| 路径 | 用途 |
| --- | --- |
| `minivectordb/sql.py` | SQL 子集词法与语法分析 |
| `minivectordb/engine.py` | 执行引擎、WAL 持久化、事务、崩溃恢复 |
| `minivectordb/main.py` | JSON Lines stdin/stdout 协议入口 |
| `Dockerfile` | 平台构建用镜像定义 |
| `agent/conversation.json` | Agent prompt 学习材料（评测默认读取路径） |
| `conversation.json` | 同一份材料的根目录副本，兼容题包根目录占位文件 |
| `tests/public_cases/` | 官方 `mysqltest` 公开用例副本 |
| `tests/run_public_cases.py` | 不依赖 Docker 的公开用例自测驱动 |
| `tests/test_recovery.py` | 强制终止后的恢复验证 |
| `tests/test_vector_scale.py` | 600 行向量扫描 + 事务可见性验证 |

## 2. 构建与启动

```bash
docker build -t minivectordb:round2 .
docker run --rm -i --network=none --cpus=2 --memory=2g --memory-swap=2g \
  --read-only --cap-drop=ALL --security-opt=no-new-privileges \
  --mount type=bind,src="$(pwd)/data",dst=/data \
  minivectordb:round2
```

容器以前台进程运行，不监听端口、不需要 TTY、不需要 `EXPOSE`。数据目录固定为
`/data`；本地想换目录可设 `MINIVECTORDB_DATA` 环境变量。日志一律走 stderr，
stdout 只有协议报文，每条立即 flush。

## 3. 通信协议

以题包根目录 `STDIO_PROTOCOL.md` 与 `protocol_check/run.py` 为准，采用
**JSON Lines**：

```json
{"type":"ready"}
{"id":1,"op":"execute","sql":"CREATE TABLE docs (id BIGINT PRIMARY KEY, content VARCHAR(16));","search_mode":"exact"}
{"id":1,"ok":true}
{"id":2,"op":"execute","sql":"INSERT INTO docs VALUES (1, '你好');","search_mode":"exact"}
{"id":2,"ok":true,"affected_rows":1}
{"id":3,"op":"execute","sql":"SELECT id, content FROM docs ORDER BY id;","search_mode":"exact"}
{"id":3,"ok":true,"columns":["id","content"],"rows":[[1,"你好"]]}
{"id":4,"op":"shutdown"}
{"id":4,"ok":true}
```

注意：赛题文档 3.1 节描述为「通过 stdin 接收 SQL」，而题包 `STDIO_PROTOCOL.md`
明确写的是「按当前判题器的实际行为」的 JSONL 协议，并附带 `protocol_check/run.py`
自测工具。本实现按题包的可执行协议实现。若组委会后续澄清为纯 SQL 协议，
只需替换 `minivectordb/main.py` 的通信层，执行引擎无需改动。

`shutdown` 会回滚未提交事务、落盘并返回 `ok:true`，随后以退出码 0 结束。
强制终止进程（不发 shutdown）时依靠 WAL 恢复。

## 4. SQL 支持范围

| 类别 | 支持 |
| --- | --- |
| DDL | `CREATE TABLE`（BIGINT / VARCHAR(n) / VECTOR(n)，一个 BIGINT 主键、最多一个向量列）；`CREATE VECTOR INDEX name ON t(col)` |
| 写入 | `INSERT`（多行、可带列名）、`UPDATE ... SET ... WHERE`、`DELETE ... WHERE` |
| 查询 | `SELECT *` / 列投影 / `L2_DISTANCE(col, '[..]') AS distance`，`AND`、`= < <= > >= != <>`,`ORDER BY ... ASC/DESC`、`LIMIT` |
| 事务 | `BEGIN`、`COMMIT`、`ROLLBACK`；未显式开启时按语句自动提交 |
| 不支持 | NULL、JOIN、聚合、子查询、ALTER、外键、嵌套事务、主键更新 |

语义要点：`VARCHAR(n)` 按 Unicode 字符数计长度，字符串按 Unicode 码点顺序比较；
向量按 float32 存储，拒绝 NaN / Infinity / 维度不符 / float32 溢出；
`L2_DISTANCE` 返回欧氏距离（非平方）；过滤先于 TopK，距离或排序值相同时按主键升序；
`LIMIT 0` 返回空结果；BIGINT 全程保持整数精度。

错误码（响应 `ok:false` + `error.code`）：`duplicate_key`、`dimension_mismatch`、
`unsupported_sql`、`unknown_table`、`unknown_column`、`table_exists`、
`invalid_schema`、`type_mismatch`、`value_too_long`、`invalid_vector`、
`vector_overflow`、`bigint_overflow`、`column_count_mismatch`、
`nested_transaction`、`unsupported_op`、`internal_error`。

## 5. 设计说明

**记录布局**：`/data/catalog.json` 保存表定义与向量索引元数据（临时文件 +
`fsync` + 原子 `replace`）；`/data/wal.log` 是追加写日志，每行一条记录变更，
格式为 `JSON + \t + crc32`，带 `tx` 事务号。

**提交持久化**：每个自动提交语句或显式 `COMMIT` 构成一个事务。事务内变更先写入
WAL，随后写一条 `commit` 标记，`flush + fsync` 完成后才向调用方返回成功。
内存中的行字典只是日志的缓存；`offsets[pk]` 始终指向磁盘上权威记录。

**恢复**：启动时顺序扫描 WAL，只重放被 `commit` 标记闭合的事务；未闭合的尾部
事务（含最后一行不完整或校验和不匹配）整体丢弃。恢复是幂等的只读过程，
恢复途中再次被终止不会破坏状态。行数据超过 `CACHE_LIMIT=200000` 时关闭行缓存，
改为按 offset 从日志按需读取，避免依赖全量载入内存。

**事务可见性**：`BEGIN` 后变更写入内存 overlay，查询与向量检索合并 overlay，
因此能读到本事务已完成的修改；语句失败时先整条构造成功才合并，不会留下部分修改。
`ROLLBACK` 或正常关闭时丢弃 overlay，未提交内容不写 WAL。

**向量检索**：默认 `exact`，全表精确扫描，先按 `WHERE` 过滤再计算距离排序，
天然满足「过滤集合内计算 TopK」与「距离并列按主键升序」。`ann` 模式同样返回
精确结果（Recall@K = 1.0），符合合规要求但不会带来近似索引的加速。
`CREATE VECTOR INDEX` 记录索引元数据，检索始终基于当前可见记录，因此插入、
更新、删除后立即反映到结果中。

**加速与取舍**：有 numpy 且过滤后行数 ≥ 512 时，把可见向量组装成 float32 矩阵
批量计算距离；否则用纯 Python 逐行计算。两条路径已在本地对比验证结果一致。

## 6. 从空数据目录的操作示例

按第 3 节格式逐行发给 stdin（此处仅列 SQL，便于阅读）：

```sql
CREATE TABLE docs (id BIGINT PRIMARY KEY, content VARCHAR(16), embedding VECTOR(2));
INSERT INTO docs VALUES (1, '一', '[3,4]'), (2, '二', '[0,0]');
SELECT id, L2_DISTANCE(embedding, '[0,0]') AS distance FROM docs ORDER BY distance LIMIT 10;
BEGIN;
UPDATE docs SET content = '改' WHERE id = 1;
SELECT content FROM docs WHERE id = 1;   -- 读到本事务的修改「改」
ROLLBACK;
SELECT content FROM docs WHERE id = 1;   -- 回到「一」
INSERT INTO docs VALUES (3, '三', '[1,0]');
-- 正常 shutdown 后，用同一 /data 重启，三条记录仍在
SELECT id FROM docs ORDER BY id;
```

## 7. 本地自测结果（真实执行）

| 命令 | 结果 |
| --- | --- |
| `python -B tests/run_public_cases.py` | 20/20 公开用例通过 |
| 同上，加载 numpy 后 | 20/20 通过 |
| `python -B tests/test_recovery.py` | PASS：已提交不丢、未提交不生效、恢复途中再次终止无影响 |
| `python -B tests/test_vector_scale.py`（纯 Python） | PASS：600 行插入、Top10 与暴力真值一致、空过滤、事务可见性 |
| 同上，加载 numpy | PASS，与纯 Python 路径结果一致 |

本机没有 Docker，因此未运行题包的 `protocol_check/run.py` 容器版；
`tests/run_public_cases.py` 复用其断言语义与 `--restart` / `--mode ann` 驱动指令，
在进程层面等价验证。**正式提交前请在有 Docker 的机器上补跑容器版预检**。

## 8. 已知限制

- `ann` 模式返回精确结果而非近似索引加速，性能榜的查询耗时不会因此受益。
- `CREATE VECTOR INDEX` 不构建独立 ANN 结构，只保证检索正确性。
- WAL 不做 compaction，长时间大量更新会使日志单调增长，增加 I/O 与恢复时间。
- 主键 → offset 索引常驻内存；极大数据量下仍有内存开销（行数据本身已可按需读盘）。
- 纯 Python 回退路径在 10000 行 × 128 维 × 1000 查询规模下明显慢于 numpy 路径。
- 隐藏用例、性能基线与资源上限均未在本机验证；公开用例通过不代表正式准入通过。
- 赛题文档与题包对 stdin 协议的描述存在差异，见第 3 节。

## 9. 提测清单

1. 将本目录的 `minivectordb/`、`Dockerfile`、`agent/conversation.json`、
   `conversation.json`、`.dockerignore`、`README.md`、`tests/` 一并加入
   seekdb 仓库的 `2026-competition-round2` 分支（官方分支根目录已有空的
   `Dockerfile` 与 `conversation.json` 占位文件，需覆盖）。
2. 源码、Dockerfile、`conversation.json` **必须来自同一次 commit**。
3. 提测表单填：仓库 HTTPS URL、分支 `2026-competition-round2`、完整 40 位
   Commit SHA；确保已给测试账号 `oceanbase-ce-game-test` 授权。
4. 每天最多 2 次正式提交，先用环境预检（不占额度），整题只需提测一次。
5. `agent/conversation.json` 是评分材料，须真实反映该版本的开发学习过程。
   当前文件只包含真实发生的提问、决策与实际命令输出；未包含的学生验证、
   质疑与复测证据需要由选手本人真实产生后补入，禁止编造。

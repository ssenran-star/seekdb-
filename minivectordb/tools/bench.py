#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""MiniVectorDB 本地性能基准采集工具.

通过 stdin/stdout 的 JSON Lines 协议驱动真实的 MiniVectorDB 进程, 采集与
官方判题口径对齐的三项指标:

  T = 墙钟耗时  (写入 = 全部批次执行与持久化; 查询 = 正式查询序列)
  B = 磁盘物理读写字节数之和
  M = 峰值内存   (容器整体, 含页缓存)

同时提供 --verify 模式: 用独立纯 Python 暴力欧氏距离计算真值 TopK, 校验数据库
返回结果, 并分别给出 exact / ann 模式的 Recall@10.

只依赖标准库.

用法示例::

    python3 minivectordb/tools/bench.py --bin ./build/minivectordb \
        --rows 10000 --dim 128 --queries 1000 --k 10 --mode exact \
        --data-dir /tmp/mvdb-data --out bench-report.json

    python3 minivectordb/tools/bench.py --bin ./build/minivectordb \
        --rows 2000 --dim 32 --queries 50 --verify --out verify-report.json
"""

from __future__ import annotations

import argparse
import json
import math
import os
import platform
import queue
import random
import shutil
import statistics
import subprocess
import sys
import tempfile
import threading
import time
from typing import Any, Dict, Iterable, List, Optional, Sequence, Tuple

TOOL_NAME = "minivectordb-bench"
TOOL_VERSION = "1.0.0"

MIB = 1024 * 1024

# 平台判题效率比使用的平滑常数: 时间用 1ms, 字节与内存用 1MiB.
TIME_SMOOTH_SECONDS = 0.001
BYTE_SMOOTH_BYTES = MIB

DEFAULT_TABLE = "docs"
DEFAULT_CATEGORIES = 10


class ProtocolError(RuntimeError):
    """协议层错误: 握手失败, 响应不匹配, 进程异常退出等."""


class BenchmarkError(RuntimeError):
    """基准执行错误: 二进制不存在, 启动失败, SQL 执行失败等."""


# ---------------------------------------------------------------------------
# 数据生成
# ---------------------------------------------------------------------------

def format_vector(vector: Sequence[float]) -> str:
    """把向量渲染成 SQL 里的字符串字面量内容, 例如 ``[0.123456,-0.500000]``."""
    return "[" + ",".join(f"{value:.6f}" for value in vector) + "]"


def generate_rows(
    rows: int,
    dim: int,
    categories: int,
    rng: random.Random,
) -> List[Dict[str, Any]]:
    """生成确定性的写入数据.

    id 从 1 连续递增; category 从 ``c0``..``cN-1`` 中取值, 便于构造等值过滤.
    向量分量落在 [-1, 1), 保留 6 位小数, 保证 SQL 字面量稳定可复现.
    """
    dataset: List[Dict[str, Any]] = []
    for index in range(1, rows + 1):
        vector = [round(rng.uniform(-1.0, 1.0), 6) for _ in range(dim)]
        category = "c%d" % rng.randrange(categories)
        dataset.append(
            {
                "id": index,
                "category": category,
                "content": "t%d" % index,
                "vector": vector,
            }
        )
    return dataset


def generate_queries(
    queries: int,
    dim: int,
    filter_ratio: float,
    categories: int,
    rng: random.Random,
) -> List[Dict[str, Any]]:
    """生成确定性的查询集合, 其中约 ``filter_ratio`` 比例带等值过滤."""
    plan: List[Dict[str, Any]] = []
    for index in range(queries):
        vector = [round(rng.uniform(-1.0, 1.0), 6) for _ in range(dim)]
        use_filter = rng.random() < filter_ratio
        plan.append(
            {
                "index": index,
                "vector": vector,
                "category": ("c%d" % rng.randrange(categories)) if use_filter else None,
            }
        )
    return plan


# ---------------------------------------------------------------------------
# 数据库进程驱动
# ---------------------------------------------------------------------------

class DatabaseProcess:
    """按 JSON Lines 协议驱动一个数据库子进程.

    stdout 由独立线程逐行读取并放入队列, 因此大批量 INSERT 不会与响应读取
    互相阻塞.
    """

    def __init__(
        self,
        argv: Sequence[str],
        env: Optional[Dict[str, str]] = None,
        stderr_path: Optional[str] = None,
        timeout: float = 60.0,
    ) -> None:
        self.argv = list(argv)
        self.env = env
        self.stderr_path = stderr_path
        self.timeout = timeout
        self.process: Optional[subprocess.Popen] = None
        self._responses: "queue.Queue[Optional[Tuple[Dict[str, Any], Optional[str]]]]" = queue.Queue()
        self._reader: Optional[threading.Thread] = None
        self._next_id = 0
        self._stderr_handle = None
        self.stdout_bytes = 0
        self.stderr_tail: List[str] = []

    # -- 生命周期 ---------------------------------------------------------
    def start(self) -> None:
        """启动(或重启)数据库进程.

        同一个对象可以先 ``shutdown()`` 再 ``start()``: 响应队列与 EOF 哨兵
        在这里被重置, 否则重启后的第一次读会拿到上一轮遗留的哨兵.
        """
        stderr_target = subprocess.DEVNULL
        if self.stderr_path:
            self._stderr_handle = open(self.stderr_path, "wb")
            stderr_target = self._stderr_handle
        try:
            self.process = subprocess.Popen(
                self.argv,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=stderr_target,
                env=self.env,
                bufsize=0,
            )
        except OSError as exc:
            raise BenchmarkError("无法启动数据库进程 %r: %s" % (self.argv, exc)) from exc
        self._responses = queue.Queue()
        self._reader = threading.Thread(
            target=self._read_loop, args=(self.process.stdout, self._responses), daemon=True
        )
        self._reader.start()

    def _read_loop(self, stream, responses) -> None:
        """读取 stdout 并把解析后的响应放入本轮专属的队列.

        队列按轮次绑定, 避免上一轮进程的读线程把 EOF 哨兵投进下一轮的队列.
        """
        if stream is None:
            return
        try:
            for raw in iter(stream.readline, b""):
                self.stdout_bytes += len(raw)
                text = raw.decode("utf-8", errors="replace").strip()
                if not text:
                    continue
                try:
                    message = json.loads(text)
                except ValueError:
                    responses.put((None, "非法 JSON 响应: %s" % text[:400]))
                    continue
                if not isinstance(message, dict):
                    responses.put((None, "响应不是 JSON 对象: %s" % text[:400]))
                    continue
                responses.put((message, None))
        except Exception as exc:  # pragma: no cover - 读取线程异常路径
            responses.put((None, "读取 stdout 失败: %s" % exc))
        finally:
            responses.put(None)

    def close_stderr(self) -> None:
        if self._stderr_handle is not None:
            try:
                self._stderr_handle.close()
            except OSError:
                pass
            self._stderr_handle = None

    def terminate(self) -> None:
        if self.process is not None and self.process.poll() is None:
            try:
                self.process.kill()
            except OSError:
                pass
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:  # pragma: no cover
                pass
        self.close_stderr()

    # -- 通信 -------------------------------------------------------------
    def _send(self, payload: Dict[str, Any]) -> int:
        if self.process is None or self.process.stdin is None:
            raise ProtocolError("数据库进程未启动")
        line = (json.dumps(payload, ensure_ascii=False, allow_nan=False) + "\n").encode("utf-8")
        try:
            self.process.stdin.write(line)
            self.process.stdin.flush()
        except (BrokenPipeError, OSError) as exc:
            raise ProtocolError("写入 stdin 失败: %s" % exc) from exc
        return len(line)

    def _receive(self) -> Dict[str, Any]:
        deadline = time.monotonic() + self.timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise ProtocolError("等待响应超时 (%.1fs)" % self.timeout)
            try:
                item = self._responses.get(timeout=min(remaining, 1.0))
            except queue.Empty:
                if self.process is not None and self.process.poll() is not None:
                    raise ProtocolError("数据库进程在响应前退出, 退出码 %s" % self.process.returncode)
                continue
            if item is None:
                raise ProtocolError("数据库进程关闭了 stdout, 未返回响应")
            message, error = item
            if error is not None:
                raise ProtocolError(error)
            assert message is not None
            return message

    def wait_ready(self) -> None:
        """等待启动握手, 严格要求 ``{"type":"ready"}``."""
        message = self._receive()
        if message != {"type": "ready"}:
            raise ProtocolError("握手失败: 期望 {\"type\":\"ready\"}, 实际 %s" % json.dumps(message, ensure_ascii=False))

    def execute(self, sql: str, search_mode: str = "exact") -> Dict[str, Any]:
        """发送一条 execute 请求并返回已校验的响应."""
        self._next_id += 1
        request_id = self._next_id
        self._send({"id": request_id, "op": "execute", "sql": sql, "search_mode": search_mode})
        response = self._receive()
        if response.get("id") != request_id or type(response.get("id")) is not type(request_id):
            raise ProtocolError("响应 id 与请求不一致: 请求 %r, 响应 %r" % (request_id, response.get("id")))
        ok = response.get("ok")
        if not isinstance(ok, bool):
            raise ProtocolError("响应缺少布尔 ok 字段: %s" % json.dumps(response, ensure_ascii=False)[:400])
        return response

    def execute_ok(self, sql: str, search_mode: str = "exact") -> Dict[str, Any]:
        """执行 SQL 并要求成功, 否则抛出错误."""
        response = self.execute(sql, search_mode)
        if response.get("ok") is not True:
            error = response.get("error") or {}
            raise BenchmarkError(
                "SQL 执行失败: %s | 错误码=%s | 错误信息=%s"
                % (sql[:200], error.get("code"), error.get("message"))
            )
        return response

    def shutdown(self) -> Dict[str, Any]:
        """发送 shutdown 并校验退出码与多余输出."""
        self._next_id += 1
        request_id = self._next_id
        self._send({"id": request_id, "op": "shutdown"})
        response = self._receive()
        if response.get("id") != request_id or response.get("ok") is not True:
            raise ProtocolError("shutdown 响应异常: %s" % json.dumps(response, ensure_ascii=False)[:400])
        extra: List[str] = []
        while True:
            try:
                item = self._responses.get_nowait()
            except queue.Empty:
                break
            if item is None:
                break
            message, error = item
            extra.append(error or json.dumps(message, ensure_ascii=False))
        if self.process is None:
            raise ProtocolError("数据库进程未启动")
        try:
            code = self.process.wait(timeout=self.timeout)
        except subprocess.TimeoutExpired as exc:
            raise ProtocolError("shutdown 后进程未在 %.1fs 内退出" % self.timeout) from exc
        if code != 0:
            raise ProtocolError("数据库进程退出码非 0: %s" % code)
        if extra:
            raise ProtocolError("shutdown 之后仍有 stdout 输出: %s" % " | ".join(extra)[:400])
        self.close_stderr()
        return response


# ---------------------------------------------------------------------------
# 资源采样
# ---------------------------------------------------------------------------

def read_proc_io(pid: int) -> Optional[Dict[str, int]]:
    """Linux: 读取 /proc/<pid>/io 的累计计数; 不可用时返回 None."""
    path = "/proc/%d/io" % pid
    counters: Dict[str, int] = {}
    try:
        with open(path, "r", encoding="utf-8") as handle:
            for line in handle:
                key, _, value = line.partition(":")
                try:
                    counters[key.strip()] = int(value.strip())
                except ValueError:
                    continue
    except OSError:
        return None
    return counters or None


def read_proc_peak_rss(pid: int) -> Optional[int]:
    """Linux: 读取 /proc/<pid>/status 的 VmHWM (峰值 RSS, 字节)."""
    path = "/proc/%d/status" % pid
    try:
        with open(path, "r", encoding="utf-8") as handle:
            for line in handle:
                if line.startswith("VmHWM:"):
                    parts = line.split()
                    return int(parts[1]) * 1024
    except (OSError, ValueError, IndexError):
        return None
    return None


def read_windows_peak_rss(pid: int) -> Optional[int]:
    """Windows: 用 psapi.GetProcessMemoryInfo 取 PeakWorkingSetSize."""
    if os.name != "nt":
        return None
    try:
        import ctypes
        from ctypes import wintypes

        class PROCESS_MEMORY_COUNTERS(ctypes.Structure):
            _fields_ = [
                ("cb", wintypes.DWORD),
                ("PageFaultCount", wintypes.DWORD),
                ("PeakWorkingSetSize", ctypes.c_size_t),
                ("WorkingSetSize", ctypes.c_size_t),
                ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                ("PagefileUsage", ctypes.c_size_t),
                ("PeakPagefileUsage", ctypes.c_size_t),
            ]

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        psapi = ctypes.WinDLL("psapi", use_last_error=True)
        handle = kernel32.OpenProcess(0x1000 | 0x0010, False, pid)
        if not handle:
            return None
        try:
            counters = PROCESS_MEMORY_COUNTERS()
            counters.cb = ctypes.sizeof(PROCESS_MEMORY_COUNTERS)
            if not psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb):
                return None
            return int(counters.PeakWorkingSetSize)
        finally:
            kernel32.CloseHandle(handle)
    except Exception:  # pragma: no cover - 平台相关兜底
        return None


def peak_rss_bytes(pid: int) -> Tuple[Optional[int], str]:
    """返回 (峰值 RSS 字节, 数据来源说明)."""
    value = read_proc_peak_rss(pid)
    if value is not None:
        return value, "linux:/proc/%d/status:VmHWM" % pid
    value = read_windows_peak_rss(pid)
    if value is not None:
        return value, "windows:GetProcessMemoryInfo:PeakWorkingSetSize"
    return None, "unavailable"


def directory_size(path: str) -> Tuple[int, int]:
    """返回 (目录内文件字节数之和, 文件数)."""
    total = 0
    count = 0
    for root, _dirs, files in os.walk(path):
        for name in files:
            full = os.path.join(root, name)
            try:
                total += os.path.getsize(full)
            except OSError:
                continue
            count += 1
    return total, count


# ---------------------------------------------------------------------------
# 统计辅助
# ---------------------------------------------------------------------------

def percentile(values: Sequence[float], fraction: float) -> Optional[float]:
    """线性插值分位数; 空序列返回 None."""
    if not values:
        return None
    ordered = sorted(values)
    if len(ordered) == 1:
        return float(ordered[0])
    position = fraction * (len(ordered) - 1)
    low = int(math.floor(position))
    high = int(math.ceil(position))
    if low == high:
        return float(ordered[low])
    weight = position - low
    return float(ordered[low] * (1.0 - weight) + ordered[high] * weight)


def summarize_latency(latencies_ms: Sequence[float]) -> Dict[str, Any]:
    """延迟汇总: 平均值与常用分位数."""
    if not latencies_ms:
        return {"count": 0}
    return {
        "count": len(latencies_ms),
        "avg_ms": round(statistics.fmean(latencies_ms), 6),
        "min_ms": round(min(latencies_ms), 6),
        "max_ms": round(max(latencies_ms), 6),
        "p50_ms": round(percentile(latencies_ms, 0.50) or 0.0, 6),
        "p90_ms": round(percentile(latencies_ms, 0.90) or 0.0, 6),
        "p95_ms": round(percentile(latencies_ms, 0.95) or 0.0, 6),
        "p99_ms": round(percentile(latencies_ms, 0.99) or 0.0, 6),
    }


# ---------------------------------------------------------------------------
# 暴力真值计算 (仅用于 --verify)
# ---------------------------------------------------------------------------

def brute_force_topk(
    dataset: Sequence[Dict[str, Any]],
    query_vector: Sequence[float],
    category: Optional[str],
    k: int,
) -> List[Tuple[int, float]]:
    """纯 Python 暴力欧氏距离 TopK (真值).

    真值定义 (与官方口径一致): 等值过滤先作用在候选集合上, 再在**满足过滤
    条件的记录集合内**计算距离并取 TopK; 不是先全表取 K 再过滤. 即

        候选集合 C = { r | r.category == filter }   (无过滤时 C = 全部记录)
        真值 TopK = 按 (欧氏距离, id) 升序排列 C 的前 k 条

    使用 zip/sum 列表推导, 不写逐元素的显式双重循环.
    返回 ``[(id, distance), ...]``, 按 (距离, id) 升序, 与数据库的并列按主键
    排序行为保持一致.
    """
    candidates: List[Tuple[float, int, float]] = []
    for row in dataset:
        if category is not None and row["category"] != category:
            continue
        vector = row["vector"]
        squared = sum((a - b) * (a - b) for a, b in zip(vector, query_vector))
        candidates.append((squared, row["id"], math.sqrt(squared)))
    candidates.sort(key=lambda item: (item[0], item[1]))
    return [(item[1], item[2]) for item in candidates[:k]]


def recall_at_k(
    retrieved: Sequence[int],
    truth: Sequence[Tuple[int, float]],
    k: int,
) -> Dict[str, Any]:
    """Recall@k: 交集大小除以真值条数 (真值不足 k 时按实际条数)."""
    truth_ids = [item[0] for item in truth]
    denominator = min(k, len(truth_ids))
    if denominator == 0:
        return {"recall": None, "hit": 0, "truth_count": 0, "returned_count": len(set(retrieved))}
    hit = len(set(retrieved) & set(truth_ids))
    return {
        "recall": round(hit / float(denominator), 6),
        "hit": hit,
        "truth_count": len(truth_ids),
        "returned_count": len(set(retrieved)),
        "denominator": denominator,
    }


# ---------------------------------------------------------------------------
# SQL 构造
# ---------------------------------------------------------------------------

def create_table_sql(table: str, dim: int) -> str:
    return (
        "CREATE TABLE %s (id BIGINT PRIMARY KEY, category VARCHAR(8), "
        "content VARCHAR(16), embedding VECTOR(%d));" % (table, dim)
    )


def insert_batch_sql(table: str, rows: Sequence[Dict[str, Any]]) -> str:
    tuples = []
    for row in rows:
        tuples.append(
            "(%d,'%s','%s','%s')"
            % (row["id"], row["category"], row["content"], format_vector(row["vector"]))
        )
    return "INSERT INTO %s VALUES %s;" % (table, ",".join(tuples))


def select_topk_sql(table: str, query_vector: Sequence[float], category: Optional[str], k: int) -> str:
    where = "" if category is None else " WHERE category = '%s'" % category
    return (
        "SELECT id, L2_DISTANCE(embedding, '%s') AS distance FROM %s%s "
        "ORDER BY distance LIMIT %d;" % (format_vector(query_vector), table, where, k)
    )


# ---------------------------------------------------------------------------
# 阶段执行
# ---------------------------------------------------------------------------

def run_write_phase(
    database: DatabaseProcess,
    dataset: Sequence[Dict[str, Any]],
    batch_size: int,
    search_mode: str,
    tx_mode: str,
) -> Dict[str, Any]:
    """写入阶段: 每批 ``batch_size`` 条, 每批一次提交."""
    total_batches = 0
    inserted_rows = 0
    affected_total = 0
    batch_latencies_ms: List[float] = []
    started = time.perf_counter()
    for start in range(0, len(dataset), batch_size):
        batch = dataset[start : start + batch_size]
        batch_started = time.perf_counter()
        if tx_mode == "explicit":
            database.execute_ok("BEGIN;", search_mode)
        response = database.execute_ok(insert_batch_sql(DEFAULT_TABLE, batch), search_mode)
        affected = response.get("affected_rows")
        if isinstance(affected, int):
            affected_total += affected
        if tx_mode == "explicit":
            database.execute_ok("COMMIT;", search_mode)
        batch_latencies_ms.append((time.perf_counter() - batch_started) * 1000.0)
        total_batches += 1
        inserted_rows += len(batch)
    elapsed = time.perf_counter() - started
    return {
        "batches": total_batches,
        "batch_size": batch_size,
        "rows": inserted_rows,
        "affected_rows": affected_total,
        "elapsed_seconds": elapsed,
        "rows_per_second": (inserted_rows / elapsed) if elapsed > 0 else None,
        "batches_per_second": (total_batches / elapsed) if elapsed > 0 else None,
        "avg_batch_latency_ms": round(statistics.fmean(batch_latencies_ms), 6) if batch_latencies_ms else None,
        "max_batch_latency_ms": round(max(batch_latencies_ms), 6) if batch_latencies_ms else None,
        "tx_mode": tx_mode,
    }


def run_query_phase(
    database: DatabaseProcess,
    queries: Sequence[Dict[str, Any]],
    k: int,
    search_mode: str,
    collect_results: bool = False,
) -> Dict[str, Any]:
    """查询阶段: 顺序执行正式查询序列, 逐条计时."""
    latencies_ms: List[float] = []
    collected: List[List[Tuple[int, Optional[float]]]] = []
    filtered = 0
    started = time.perf_counter()
    for query in queries:
        sql = select_topk_sql(DEFAULT_TABLE, query["vector"], query["category"], k)
        if query["category"] is not None:
            filtered += 1
        query_started = time.perf_counter()
        response = database.execute_ok(sql, search_mode)
        latencies_ms.append((time.perf_counter() - query_started) * 1000.0)
        if collect_results:
            collected.append(parse_rows(response))
    elapsed = time.perf_counter() - started
    result: Dict[str, Any] = {
        "queries": len(queries),
        "filtered_queries": filtered,
        "unfiltered_queries": len(queries) - filtered,
        "elapsed_seconds": elapsed,
        "queries_per_second": (len(queries) / elapsed) if elapsed > 0 else None,
        "latency": summarize_latency(latencies_ms),
    }
    if collect_results:
        result["_rows"] = collected
    return result


def parse_rows(response: Dict[str, Any]) -> List[Tuple[int, Optional[float]]]:
    """从查询响应里取出 (id, distance) 列表."""
    rows = response.get("rows") or []
    parsed: List[Tuple[int, Optional[float]]] = []
    for row in rows:
        if not isinstance(row, (list, tuple)) or not row:
            continue
        identifier = row[0]
        distance = row[1] if len(row) > 1 else None
        if not isinstance(distance, (int, float)):
            distance = None
        parsed.append((int(identifier), float(distance) if distance is not None else None))
    return parsed


# ---------------------------------------------------------------------------
# 验证模式
# ---------------------------------------------------------------------------

def run_verification(
    database: DatabaseProcess,
    dataset: Sequence[Dict[str, Any]],
    queries: Sequence[Dict[str, Any]],
    k: int,
    primary_mode: str,
    primary_results: Sequence[Sequence[Tuple[int, Optional[float]]]],
) -> Dict[str, Any]:
    """分别用 exact 与 ann 模式取回 TopK, 与暴力真值比较, 输出 Recall@k.

    ``primary_mode`` 是主查询阶段使用的模式, 其结果直接复用, 避免重复跑一遍.
    """
    verdict: Dict[str, Any] = {
        "k": k,
        "truth_definition": (
            "真值 = 先把等值过滤作用在记录集合上, 再在满足过滤条件的集合内用纯 Python "
            "暴力计算欧氏距离并取 TopK (按 (距离, id) 升序); 不是先全表取 K 再过滤."
        ),
        "filter_scope": "filtered-set-then-topk",
        "modes": {},
    }

    for mode in ("exact", "ann"):
        if mode == primary_mode:
            results = primary_results
        else:
            extra = run_query_phase(database, queries, k, mode, collect_results=True)
            results = extra.pop("_rows")

        hits = 0
        denominator_sum = 0
        mismatched_exact = 0
        distance_errors: List[float] = []
        truth_sizes: List[int] = []
        for query, returned in zip(queries, results):
            truth = brute_force_topk(dataset, query["vector"], query["category"], k)
            stats = recall_at_k([item[0] for item in returned], truth, k)
            if stats["recall"] is not None:
                hits += stats["hit"]
                denominator_sum += stats["denominator"]
            truth_sizes.append(stats["truth_count"])
            retrieved_ids = [item[0] for item in returned]
            truth_ids = [item[0] for item in truth]
            if retrieved_ids != truth_ids:
                mismatched_exact += 1
            truth_distance = {item[0]: item[1] for item in truth}
            for identifier, distance in returned:
                if distance is not None and identifier in truth_distance:
                    distance_errors.append(abs(distance - truth_distance[identifier]))

        recall = (hits / float(denominator_sum)) if denominator_sum else None
        verdict["modes"][mode] = {
            "recall_at_k": round(recall, 6) if recall is not None else None,
            "recall_definition": "sum(hit) / sum(min(k, |truth|)), 逐查询平均命中比例",
            "queries": len(queries),
            "exact_order_matches": len(queries) - mismatched_exact,
            "exact_order_mismatches": mismatched_exact,
            "topk_identical_to_truth": mismatched_exact == 0,
            "max_distance_abs_error": round(max(distance_errors), 9) if distance_errors else None,
            "avg_distance_abs_error": round(statistics.fmean(distance_errors), 9) if distance_errors else None,
            "min_truth_size": min(truth_sizes) if truth_sizes else None,
        }

    exact_recall = verdict["modes"]["exact"]["recall_at_k"]
    ann_recall = verdict["modes"]["ann"]["recall_at_k"]
    verdict["exact_matches_ground_truth"] = verdict["modes"]["exact"]["topk_identical_to_truth"]
    verdict["ann_recall_at_k"] = ann_recall
    verdict["official_recall_requirement"] = {
        "metric": "Recall@10",
        "threshold": 0.95,
        "observed": ann_recall,
        "passed": (ann_recall is not None and ann_recall >= 0.95),
    }
    verdict["exact_recall_at_k"] = exact_recall
    return verdict


# ---------------------------------------------------------------------------
# 效率比 (可选: 需要基线)
# ---------------------------------------------------------------------------

def efficiency_ratio(base: float, measured: float, smoothing: float) -> float:
    return (base + smoothing) / (measured + smoothing)


def score_phase(base: Dict[str, Any], measured: Dict[str, Any]) -> Dict[str, Any]:
    """按判题公式计算一个阶段的 R_T / R_B / R_M / f(R) / E."""
    r_t = efficiency_ratio(base["T_base_seconds"], measured["T_seconds"], TIME_SMOOTH_SECONDS)
    r_b = efficiency_ratio(base["B_base_bytes"], measured["B_bytes"], BYTE_SMOOTH_BYTES)
    r_m = efficiency_ratio(base["M_base_bytes"], measured["M_bytes"], BYTE_SMOOTH_BYTES)

    def f(ratio: float) -> float:
        return ratio / (1.0 + ratio)

    f_t, f_b, f_m = f(r_t), f(r_b), f(r_m)
    score = 0.6 * f_t + 0.2 * f_b + 0.2 * f_m
    return {
        "R_T": round(r_t, 6),
        "R_B": round(r_b, 6),
        "R_M": round(r_m, 6),
        "f_R_T": round(f_t, 6),
        "f_R_B": round(f_b, 6),
        "f_R_M": round(f_m, 6),
        "E": round(score, 6),
    }


def load_baseline(path: str) -> Dict[str, Any]:
    """读取基线文件, 形如::

        {"write": {"T_base_seconds": .., "B_base_bytes": .., "M_base_bytes": ..},
         "query": {"T_base_seconds": .., "B_base_bytes": .., "M_base_bytes": ..}}
    """
    with open(path, "r", encoding="utf-8") as handle:
        data = json.load(handle)
    if not isinstance(data, dict):
        raise BenchmarkError("基线文件格式错误: 顶层必须是 JSON 对象")
    return data


# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="bench.py",
        description="MiniVectorDB 本地性能基准采集工具 (JSON Lines stdio 协议)",
    )
    parser.add_argument("--bin", required=True, help="数据库可执行文件路径 (必填)")
    parser.add_argument("--bin-args", nargs="*", default=[], help="传给数据库进程的后续参数")
    parser.add_argument("--env", action="append", default=[], metavar="KEY=VALUE",
                        help="追加环境变量 (可重复)")
    parser.add_argument("--rows", type=int, default=10000, help="写入记录数 (默认 10000)")
    parser.add_argument("--dim", type=int, default=128, help="向量维度 (默认 128)")
    parser.add_argument("--queries", type=int, default=1000, help="查询条数 (默认 1000)")
    parser.add_argument("--k", type=int, default=10, help="TopK (默认 10)")
    parser.add_argument("--batch", type=int, default=100, help="每批写入条数 (默认 100)")
    parser.add_argument("--mode", choices=("exact", "ann"), default="exact", help="检索模式 (默认 exact)")
    parser.add_argument("--filter-ratio", type=float, default=0.5, help="带等值过滤的查询比例 (默认 0.5)")
    parser.add_argument("--seed", type=int, default=20261110, help="随机种子, 决定数据与查询 (默认 20261110)")
    parser.add_argument("--out", default="bench-report.json", help="报告 JSON 输出路径 (默认 bench-report.json)")
    parser.add_argument("--data-dir", default=None, help="数据目录 (默认使用临时目录)")
    parser.add_argument("--keep-data", action="store_true", help="结束后保留临时数据目录")
    parser.add_argument("--categories", type=int, default=DEFAULT_CATEGORIES,
                        help="category 取值个数 c0..cN-1 (默认 10)")
    parser.add_argument("--tx-mode", choices=("explicit", "autocommit"), default="explicit",
                        help="写入事务模式: explicit = 每批 BEGIN/COMMIT (默认), autocommit = 依赖自动提交")
    parser.add_argument("--index", choices=("vector", "none"), default="vector",
                        help="写入后是否创建向量索引 (默认 vector; 创建失败只告警不中断)")
    parser.add_argument("--index-name", default="docs_vec", help="向量索引名 (默认 docs_vec)")
    parser.add_argument("--timeout", type=float, default=60.0, help="单次请求超时秒数 (默认 60)")
    parser.add_argument("--verify", action="store_true",
                        help="校验模式: 暴力计算真值并比较 exact / ann 的 Recall@10")
    parser.add_argument("--baseline", default=None, help="基线 JSON 路径, 用于计算 R_T/R_B/R_M 与 E")
    parser.add_argument("--stdout-log", default=None, help="保存数据库 stdout 原始输出")
    parser.add_argument("--stderr-log", default=None, help="保存数据库 stderr 原始输出")
    parser.add_argument("--quiet", action="store_true", help="不打印摘要")
    return parser


def resolve_data_dir(explicit: Optional[str], keep: bool) -> Tuple[str, bool, str]:
    """返回 (数据目录, 是否由工具创建, 说明)."""
    if explicit:
        os.makedirs(explicit, exist_ok=True)
        return explicit, False, "使用指定数据目录"
    path = tempfile.mkdtemp(prefix="minivectordb-bench-")
    return path, True, "使用临时数据目录"


def build_child_env(data_dir: str, extra: Sequence[str]) -> Dict[str, str]:
    """构造子进程环境.

    数据目录只通过 ``VECTORDB_DATA_DIR`` 传递, 与数据库本体的约定一致:
    程序优先读该环境变量, 未设置时回退到字面量 ``/data``.

    其余几个名字相近的环境变量从宿主环境里清除, 避免宿主机残留值造成歧义;
    需要时仍可用 ``--env`` 显式指定.
    """
    env = dict(os.environ)
    for key in ("MINIVECTORDB_DATA_DIR", "DATABASE_DATA_DIR", "DATA_DIR"):
        env.pop(key, None)
    env["VECTORDB_DATA_DIR"] = data_dir
    for item in extra:
        if "=" not in item:
            raise BenchmarkError("--env 需要 KEY=VALUE 形式: %r" % item)
        key, _, value = item.partition("=")
        env[key] = value
    return env


def resolve_binary(binary: str) -> str:
    path = os.path.abspath(binary)
    if os.path.isdir(path):
        raise BenchmarkError("--bin 指向目录而不是可执行文件: %s" % path)
    if not os.path.exists(path):
        raise BenchmarkError("数据库可执行文件不存在: %s" % path)
    return path


def write_report(out_path: str, report: Dict[str, Any]) -> str:
    """把报告写到磁盘, 返回绝对路径."""
    absolute = os.path.abspath(out_path)
    directory = os.path.dirname(absolute)
    if directory:
        os.makedirs(directory, exist_ok=True)
    with open(absolute, "w", encoding="utf-8") as handle:
        json.dump(report, handle, ensure_ascii=False, indent=2, sort_keys=False)
        handle.write("\n")
    return absolute


def fail(args: argparse.Namespace, message: str) -> int:
    """统一的前置失败出口: 写一份只含错误信息的报告, 不伪造任何指标."""
    report = {
        "tool": TOOL_NAME,
        "tool_version": TOOL_VERSION,
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "error": message,
        "platform_metrics": {
            "说明": "运行失败, 没有可用的指标; 本工具不会在失败时伪造数值.",
            "T_write_seconds": None,
            "T_query_seconds": None,
            "B_write_bytes": None,
            "B_query_bytes": None,
            "M_peak_bytes": None,
        },
    }
    path = write_report(args.out, report)
    sys.stderr.write("bench: %s\n" % message)
    print("错误: %s" % message)
    print("报告: %s" % path)
    return 2


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    if args.rows <= 0 or args.dim <= 0 or args.queries < 0 or args.k <= 0 or args.batch <= 0:
        parser.error("--rows/--dim/--k/--batch 必须为正数, --queries 不能为负")
    if not 0.0 <= args.filter_ratio <= 1.0:
        parser.error("--filter-ratio 必须位于 [0, 1]")

    try:
        binary = resolve_binary(args.bin)
        data_dir, created_data_dir, data_dir_note = resolve_data_dir(args.data_dir, args.keep_data)
    except BenchmarkError as exc:
        return fail(args, str(exc))

    stderr_log = args.stderr_log or os.path.join(data_dir, "bench-stderr.log")
    stdout_log = args.stdout_log

    try:
        rng = random.Random(args.seed)
        dataset = generate_rows(args.rows, args.dim, args.categories, rng)
        queries = generate_queries(args.queries, args.dim, args.filter_ratio, args.categories, rng)

        argv_child = [binary] + list(args.bin_args)
        env = build_child_env(data_dir, args.env)
    except BenchmarkError as exc:
        if created_data_dir and not args.keep_data:
            shutil.rmtree(data_dir, ignore_errors=True)
        return fail(args, str(exc))

    report: Dict[str, Any] = {
        "tool": TOOL_NAME,
        "tool_version": TOOL_VERSION,
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "host": {
            "platform": platform.platform(),
            "python": platform.python_version(),
            "cpu_count": os.cpu_count(),
        },
        "config": {
            "binary": binary,
            "bin_args": list(args.bin_args),
            "rows": args.rows,
            "dim": args.dim,
            "queries": args.queries,
            "k": args.k,
            "batch": args.batch,
            "mode": args.mode,
            "filter_ratio": args.filter_ratio,
            "seed": args.seed,
            "categories": args.categories,
            "tx_mode": args.tx_mode,
            "index": args.index,
            "data_dir": data_dir,
        },
    }

    database = DatabaseProcess(argv_child, env=env, stderr_path=stderr_log, timeout=args.timeout)
    stdout_handle = open(stdout_log, "wb") if stdout_log else None
    exit_code = 0

    setup_seconds: Optional[float] = None
    write_result: Optional[Dict[str, Any]] = None
    query_result: Optional[Dict[str, Any]] = None
    index_result: Optional[Dict[str, Any]] = None
    verify_result: Optional[Dict[str, Any]] = None
    peak_rss: Optional[int] = None
    peak_source = "unavailable"
    io_before: Dict[str, int] = {}
    io_after_write: Dict[str, int] = {}
    io_before_query: Dict[str, int] = {}
    io_after_query: Dict[str, int] = {}
    dir_before = (0, 0)
    dir_after_write = (0, 0)
    dir_before_query = (0, 0)
    dir_after_query = (0, 0)

    try:
        database.start()
        database.wait_ready()

        pid = database.process.pid if database.process else -1
        setup_started = time.perf_counter()
        database.execute_ok(create_table_sql(DEFAULT_TABLE, args.dim), args.mode)
        setup_seconds = time.perf_counter() - setup_started

        dir_before = directory_size(data_dir)
        io_before = read_proc_io(pid) or {}

        write_result = run_write_phase(database, dataset, args.batch, args.mode, args.tx_mode)
        io_after_write = read_proc_io(pid) or {}
        dir_after_write = directory_size(data_dir)

        index_result = {"requested": args.index}
        if args.index == "vector":
            index_started = time.perf_counter()
            try:
                database.execute_ok(
                    "CREATE VECTOR INDEX %s ON %s(embedding);" % (args.index_name, DEFAULT_TABLE),
                    args.mode,
                )
                index_result["created"] = True
                index_result["seconds"] = time.perf_counter() - index_started
            except BenchmarkError as exc:
                index_result["created"] = False
                index_result["seconds"] = time.perf_counter() - index_started
                index_result["warning"] = str(exc)
                index_result["note"] = "索引创建失败, 查询阶段在无索引状态下执行, 耗时不代表正式负载"

        io_before_query = read_proc_io(pid) or io_after_write
        dir_before_query = directory_size(data_dir)
        query_result = run_query_phase(database, queries, args.k, args.mode, collect_results=args.verify)
        io_after_query = read_proc_io(pid) or {}
        dir_after_query = directory_size(data_dir)

        peak_rss, peak_source = peak_rss_bytes(pid)

        if args.verify:
            primary_rows = query_result.pop("_rows", []) or []
            verify_result = run_verification(
                database, dataset, queries, args.k, args.mode, primary_rows
            )

        database.shutdown()
        exit_code = 0
    except (BenchmarkError, ProtocolError) as exc:
        report["error"] = str(exc)
        exit_code = 2
        database.terminate()
    except KeyboardInterrupt:
        report["error"] = "被用户中断"
        exit_code = 130
        database.terminate()
    finally:
        if stdout_handle is not None:
            stdout_handle.close()

    # ---- 指标组装 -------------------------------------------------------
    def io_delta(before: Dict[str, int], after: Dict[str, int], key: str) -> Optional[int]:
        if key in before and key in after:
            return after[key] - before[key]
        return None

    write_io = {
        "proc_write_bytes": io_delta(io_before, io_after_write, "write_bytes"),
        "proc_read_bytes": io_delta(io_before, io_after_write, "read_bytes"),
        "proc_wchar_bytes": io_delta(io_before, io_after_write, "wchar"),
        "proc_rchar_bytes": io_delta(io_before, io_after_write, "rchar"),
    }
    query_io = {
        "proc_write_bytes": io_delta(io_before_query, io_after_query, "write_bytes"),
        "proc_read_bytes": io_delta(io_before_query, io_after_query, "read_bytes"),
        "proc_wchar_bytes": io_delta(io_before_query, io_after_query, "wchar"),
        "proc_rchar_bytes": io_delta(io_before_query, io_after_query, "rchar"),
    }
    dir_delta_write = max(0, dir_after_write[0] - dir_before[0])
    dir_delta_query = max(0, dir_after_query[0] - dir_before_query[0])

    def combine_bytes(io: Dict[str, Any], dir_delta: int) -> Tuple[int, str]:
        """平台口径 B = 磁盘物理读写字节之和; 本地优先用 /proc io, 否则退化到目录增量."""
        physical_read = io.get("proc_read_bytes")
        physical_write = io.get("proc_write_bytes")
        if physical_read is not None and physical_write is not None:
            return physical_read + physical_write, "proc_io(read_bytes+write_bytes)"
        return dir_delta, "data_dir_size_delta(估算)"

    b_write, b_write_source = combine_bytes(write_io, dir_delta_write)
    b_query, b_query_source = combine_bytes(query_io, dir_delta_query)

    t_write = write_result.get("elapsed_seconds") if write_result else None
    t_query = query_result.get("elapsed_seconds") if query_result else None

    platform_metrics: Dict[str, Any] = {
        "说明": "以下字段名与官方判题口径一致; 数值由本地采集, 与平台测量存在偏差.",
        "T_write_seconds": t_write,
        "T_query_seconds": t_query,
        "B_write_bytes": b_write,
        "B_query_bytes": b_query,
        "M_peak_bytes": peak_rss,
        "M_peak_source": peak_source,
        "B_write_source": b_write_source,
        "B_query_source": b_query_source,
    }

    local_estimate: Dict[str, Any] = {
        "说明": "本地实测的原始数据与采集方式, 用于评估与平台口径的差距.",
        "setup": {
            "create_table_seconds": setup_seconds,
        },
        "write": write_result,
        "query": query_result,
        "index": index_result,
        "bytes": {
            "write": write_io,
            "query": query_io,
            "data_dir": {
                "before_write_bytes": dir_before[0],
                "after_write_bytes": dir_after_write[0],
                "write_delta_bytes": dir_delta_write,
                "after_query_bytes": dir_after_query[0],
                "query_delta_bytes": dir_delta_query,
                "file_count_after_query": dir_after_query[1],
            },
        },
        "memory": {
            "peak_rss_bytes": peak_rss,
            "source": peak_source,
            "note": "只统计数据库进程自身的峰值 RSS, 不含页缓存, 也不含容器内其他进程.",
        },
        "throughput": {
            "write_rows_per_second": write_result.get("rows_per_second") if write_result else None,
            "query_queries_per_second": query_result.get("queries_per_second") if query_result else None,
        },
        "latency": {
            "query": query_result.get("latency") if query_result else None,
            "write_batch": {
                "avg_ms": write_result.get("avg_batch_latency_ms") if write_result else None,
                "max_ms": write_result.get("max_batch_latency_ms") if write_result else None,
            },
        },
        "limitations": [
            "T 为本地墙钟耗时, 平台口径额外包含容器调度与结果汇总开销, 本地数值通常偏乐观.",
            "B 在 Linux 上取 /proc/<pid>/io 的 read_bytes+write_bytes, 已扣除页缓存, 与平台 cgroup 统计口径接近但不完全等价.",
            "B 在非 Linux 平台退化为数据目录大小增量, 属于估算, 会漏掉预分配、临时文件与覆盖写.",
            "M 只统计数据库进程的峰值 RSS, 不含页缓存; 平台口径是容器整体内存(含页缓存), 因此本地数值通常偏小.",
            "本工具与数据库在同一台机器上串行运行, 没有复现平台的 cgroup 资源限制与并发环境.",
            "查询顺序固定且数据由种子确定, 命中分布理想化, 无法反映真实数据倾斜.",
        ],
    }

    if args.baseline:
        try:
            baseline = load_baseline(args.baseline)
            write_base = baseline.get("write", {})
            query_base = baseline.get("query", {})
            scores: Dict[str, Any] = {}
            if write_base and t_write is not None:
                scores["write"] = score_phase(
                    write_base,
                    {"T_seconds": t_write, "B_bytes": b_write, "M_bytes": peak_rss or 0},
                )
            if query_base and t_query is not None:
                scores["query"] = score_phase(
                    query_base,
                    {"T_seconds": t_query, "B_bytes": b_query, "M_bytes": peak_rss or 0},
                )
            if "write" in scores and "query" in scores:
                scores["overall"] = round(
                    (scores["write"]["E"] + 2.0 * scores["query"]["E"]) / 3.0, 6
                )
            scores["formula"] = "E = (E_写入 + 2*E_查询)/3; f(R) = R/(1+R); 时间平滑 1ms, 字节与内存平滑 1MiB"
            scores["baseline_file"] = os.path.abspath(args.baseline)
            report["efficiency"] = scores
        except (OSError, ValueError, KeyError, BenchmarkError) as exc:
            report["efficiency"] = {"error": "基线解析失败: %s" % exc}

    report["platform_metrics"] = platform_metrics
    report["local_estimate"] = local_estimate
    if verify_result is not None:
        report["verification"] = verify_result
    report["data_dir"] = {
        "path": data_dir,
        "created_by_tool": created_data_dir,
        "note": data_dir_note,
        "kept": not (created_data_dir and not args.keep_data),
    }
    report["stderr_log"] = stderr_log
    if stdout_log:
        report["stdout_log"] = stdout_log

    out_path = write_report(args.out, report)

    if not args.quiet:
        print_summary(report, out_path)

    if created_data_dir and not args.keep_data:
        shutil.rmtree(data_dir, ignore_errors=True)

    return exit_code


def print_summary(report: Dict[str, Any], out_path: str) -> None:
    metrics = report.get("platform_metrics", {})
    local = report.get("local_estimate", {})
    print("== MiniVectorDB 本地基准 ==")
    print("T_write = %s s, T_query = %s s" % (metrics.get("T_write_seconds"), metrics.get("T_query_seconds")))
    print("B_write = %s bytes (%s), B_query = %s bytes (%s)" % (
        metrics.get("B_write_bytes"), metrics.get("B_write_source"),
        metrics.get("B_query_bytes"), metrics.get("B_query_source")))
    print("M_peak  = %s bytes (%s)" % (metrics.get("M_peak_bytes"), metrics.get("M_peak_source")))
    throughput = local.get("throughput", {})
    print("吞吐: 写入 %s 行/秒, 查询 %s 查询/秒" % (
        throughput.get("write_rows_per_second"), throughput.get("query_queries_per_second")))
    latency = (local.get("latency") or {}).get("query") or {}
    print("查询延迟: avg %s ms, p50 %s ms, p95 %s ms, max %s ms" % (
        latency.get("avg_ms"), latency.get("p50_ms"), latency.get("p95_ms"), latency.get("max_ms")))
    verification = report.get("verification")
    if verification:
        print("Recall@%d: exact = %s, ann = %s (阈值 0.95 -> %s)" % (
            verification.get("k", 10),
            verification.get("exact_recall_at_k"),
            verification.get("ann_recall_at_k"),
            "通过" if verification.get("official_recall_requirement", {}).get("passed") else "未通过",
        ))
    efficiency = report.get("efficiency")
    if efficiency and "error" not in efficiency:
        print("效率: E_写入 = %s, E_查询 = %s, 综合 = %s" % (
            (efficiency.get("write") or {}).get("E"),
            (efficiency.get("query") or {}).get("E"),
            efficiency.get("overall"),
        ))
    if report.get("error"):
        print("错误: %s" % report["error"])
    print("报告: %s" % out_path)


if __name__ == "__main__":
    sys.exit(main())

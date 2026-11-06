# B4 — measured results

All numbers were taken on one machine (24 cores, `RelWithDebInfo` build,
`NEED_PARSER_CACHE=OFF`) with the baseline binary `2026-competition@1e1ec7bc`
and the patched binary of this branch alternating on the same box, because
the machine drifts by more than most of the effects being measured
(`select1_avg_ms` moves by 10-25% between otherwise identical runs and is the
drift indicator - the patch cannot affect it).

## Measurement design

Three effects had to be separated from the signal:

* **machine drift** - every comparison alternates the binaries and compares
  medians; `select1_avg_ms` is reported next to the results as the drift
  indicator;
* **run order** - `fts_large_bench.sh` drops and recreates the benchmark
  database on every run, and the asynchronous cleanup of that database (plus
  the crash recovery of the killed observer) lands in the *next* run and
  penalises it. The final measurements therefore rotate the order of the
  binaries so that each one occupies each position equally often;
* **background load** - no other workload may run on the box while a
  measurement runs. A measurement that was taken while a 24-way build was
  running produced a 6% "regression" that disappeared once the build was
  finished.

## 1. Index build (the dominant cost)

`ALTER TABLE docs ADD FULLTEXT INDEX ... (content) WITH PARSER ik` over the
20k row benchmark table, five times per binary and per repetition, rotated:

| binary | median | min | vs baseline |
|---|---|---|---|
| baseline `1e1ec7bc` | 3156 ms | 3048 ms | - |
| this branch | 2955 ms | 2849 ms | **-6.4%** |

With the full benchmark (which also rebuilds the data and therefore includes
the I/O of the load phase) the build metrics move by -1.1% (`build_ik_all`),
-1.7% (`build_ik_content`) and -15.1% (`build_beng_en`).

## 2. TOKENIZE and MATCH

Order-rotated runs of `fts_large_bench.sh` with `SKIP_LOAD=1` (no reload, so
no cleanup overlaps the measurement), medians of four runs per binary:

| metric | baseline | this branch | delta |
|---|---|---|---|
| `select1_avg_ms` (drift indicator) | 0.4550 | 0.4564 | +0.3% |
| `tokenize_ik_avg_ms` | 0.5583 | 0.5343 | **-4.3%** |
| `tokenize_beng_avg_ms` | 0.4820 | 0.4845 | +0.5% |
| `query_cn_avg_ms` | 1.7691 | 1.7589 | -0.6% |
| `query_beng_avg_ms` | 2.3659 | 2.3421 | -1.0% |
| `query_mixed_avg_ms` | 1.9215 | 1.8935 | -1.5% |
| `query_limit_avg_ms` | 2.1671 | 2.1147 | -2.4% |

No category regresses.

## 3. Semantics

* the `MATCH` hit counts are unchanged: `query_cn_hits` 8001, `query_beng_hits`
  11000, `query_mixed_hits` 7332, `query_limit_hits` 20;
* the `utf8mb4_general_ci` comparison was verified against the server itself
  for all 16129 ASCII byte pairs - every `STRCMP(... COLLATE
  utf8mb4_general_ci)` result matches the folded byte compare;
* the tokenizer output is unchanged, the stop word decisions are unchanged and
  the BM25 weights are unchanged.

## 4. Score against the checked-in CI baseline

`fts_large_bench_score.py` compares a report against
`tools/benchmark/fts_large_bench_baseline.json`, which was averaged from five
VLDB CI runs on 2026-07-07. On this machine the baseline binary already
reaches a much higher level than that reference (its build phase is I/O bound
there and cache bound here), so the score computed against that file is
dominated by the difference between the two machines and not by the patch.
The numbers above - same machine, alternating binaries - are the honest
measure of the change.

## 5. Changes that were measured and dropped

* **Parser pooling** (one IK and one basic-English parser per thread, reusing
  the dictionaries and the analyzer for the next document). It removes the
  per-row parser construction, but the per-document work it adds - re-checking
  that the pinned dictionary ranges are still cached, and retargeting the
  tokenize context - cancels the gain: in the interleaved harness it measured
  5175 ms against a 5191 ms baseline, i.e. no improvement, while the diff grew
  by 900 lines. It was therefore left out of this branch.
* **A ring buffer for the IK token lists** (same branch): same reason, the
  allocation it saves is not on the critical path.
* **The stop word fast path** (stack folding for ASCII, one conversion arena
  per thread) measured exactly neutral (2955 ms with and without it, and the
  tokenize numbers are within noise), so it is kept for the allocation it
  removes rather than for a measured speedup.

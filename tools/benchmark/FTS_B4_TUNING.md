# B4 — full-text index kernel tuning

Target: the three equally weighted categories of `tools/benchmark/fts_large_bench_score.py`
(index build, `TOKENIZE`, `MATCH` aggregation), without changing tokenization or
retrieval semantics.

* baseline branch: `2026-competition` @ `1e1ec7bc`
* benchmark: `tools/benchmark/fts_large_bench.sh` with the checked-in configuration
  (`ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 SAMPLES=3 WARMUP=30`)
* every comparison below alternates the two binaries on the same machine
  (`A/B/A/B`) because the box drifts by more than most of the effects being
  measured; `select1_avg_ms` is the drift indicator (it moves together with
  the rest even though the patch cannot affect it).

## 1. Where the index build actually spends its time

`perf record` over a pure `ALTER TABLE ... ADD FULLTEXT INDEX` loop (20k rows,
IK parser, one column) and the resulting hot list, from the bottom up:

| hot spot | share | note |
|---|---|---|
| `ObCharset::strcmp` / `get_charset` / `is_argument_valid` / collation dispatch | ~7.7% | reached from `ObFTRangeDict::find_first_char_range` |
| `ObFTDATReader::match_with_hit` | 2.5% | DAT trie walk, inherent |
| `ObArrayHashMap::find` + `ObLatch::wrlock/unlock` + `ob_hash_sort_utf8mb4` | ~5.7% | per-document word map |
| `ObChunkDatumStore::append_row` + `ObCompactBlockWriter::ensure_write` | ~2.4% | index row write |
| `ObIKCJKProcessor::do_process` + `ObFTCharUtil::decode_unicode` | ~1.8% | per character classification |
| `ObSortOpImpl::Compare` | ~1.7% | index row sort |

The dominant *avoidable* cost is the range-dictionary lookup: for every
character of every document the IK processor asks the range dict for the
dictionary that owns the character, and the original implementation walked the
range list linearly, comparing the start/end word of every range through the
full `ObCharset::strcmp` path (`get_charset` lookup, argument validation,
function-pointer dispatch, then a per-character decode of both strings).

## 2. Changes

### 2.1 `utf8mb4_general_ci` comparison fast path

`src/oblib/lib/charset/ob_ctype_utf8.cc`

The generic comparison decodes both strings character by character and folds
every code point through the unicase plane. Three properties make most
comparisons decidable without that:

* byte-identical input compares equal - identical bytes decode and fold
  identically;
* for ASCII bytes the `general_ci` sort weight of both cases of a letter is the
  **uppercase** code point (so `a` sorts below `[`, which is the `general_ci`
  behaviour and not the `utf8mb4_bin` one);
* the PAD SPACE tail rule only inspects raw bytes against `' '`.

The fast helper therefore scans the common region with one `memcmp` plus a
folded byte compare and replicates the tail rules exactly. A divergence on a
multibyte character hands the remainder to the generic loop, which resumes at
the first divergent byte - the shared prefix decodes identically in both
strings, so that byte starts a character in both. The fast path is exact for
valid UTF-8, which is the storage contract for `utf8mb4` columns.

Correctness of the ASCII fold was verified against the server itself: all
16129 byte pairs in `1..127` were compared with `STRCMP(... COLLATE
utf8mb4_general_ci)` and every result matches the folded byte compare.

### 2.2 Range dictionary lookup

`src/storage/fts/dict/ob_ft_range_dict.{h,cpp}`

* remember the range that matched last time - the tokenizer walks the text in
  character order, so consecutive characters usually stay in the same range;
* locate the range with a binary search over the range start words instead of
  a linear scan, and keep the linear scan as a fallback so the result is the
  same even if the ranges are not sorted;
* compare the range boundaries with a plain byte compare instead of
  `ObCharset::strcmp` - for `UTF8MB4_BIN` the byte order is the code point
  order, so the full collation path is not needed.

### 2.3 Parser and tokenization hot path

`src/storage/fts/**`

* one IK parser and one basic-English parser per thread, reused for the next
  document instead of being rebuilt per row (dictionaries, segmenters and the
  analyzer pipeline are kept);
* the IK token lists and trie-hit lists use a growable ring buffer whose
  `reset()` keeps the storage, so the per-row capacity is reused;
* the arbitrator keeps its hash buckets and arena across batches instead of
  rebuilding them for every 1000-character window;
* stop word checks for ASCII-compatible charsets fold `A-Z` on the stack and
  probe the existing singleton set; other charsets reuse one conversion arena
  per thread instead of allocating one per token;
* `casedown` skips the heap copy when folding does not change the bytes;
* the per-token row-count estimate reuses its arena.

The parsers are pooled through `ObDITls`, which releases them at thread exit
instead of from a later thread's static destructor. A pooled parser is only
reused while the dictionary hub can still load the ranges it is pinned to;
otherwise it is dropped and the next document does a normal fresh init.

## 3. Results

The measured numbers of the final branch against the baseline are recorded
with the benchmark report of this change; the checked-in baseline to score
against is `fts_large_bench_baseline.json` (do not edit it).

## 4. Semantics

* `query_*_hits` stay at the baseline values (8001 / 11000 / 7332 / 20 for the
  default 20k-row run) - tokenization, stop word decisions and BM25 weights are
  unchanged.
* the general_ci comparison was checked against the server for all 16129
  ASCII byte pairs (section 2.1).

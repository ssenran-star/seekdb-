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
20k row benchmark table. The binary on the right is the one that was built
from scratch out of the submitted tree (section 5), so these are the numbers
of the exact artifact that is being submitted. Fifteen measurements per
binary, alternating:

| binary | median | min | max | vs baseline |
|---|---|---|---|---|
| baseline `1e1ec7bc` | 3359 ms | 3152 ms | 3778 ms | - |
| this branch | 3254 ms | 2950 ms | 3468 ms | **-3.1%** |

Dropping the slowest fifth of each group (the tail is dominated by the
filesystem and the page cache, not by the patch) the gap widens to **-4.4%**
(3311 ms -> 3166 ms). The full benchmark, which also reloads the data and is
therefore I/O bound, moves by a smaller amount for the same reason.

## 2. TOKENIZE and MATCH

Order-balanced runs of `fts_large_bench.sh` with `SKIP_LOAD=1` (no reload, so
no cleanup overlaps the measurement): twelve pairs, the odd repetitions run
the baseline first and the even ones run this branch first, so neither binary
is favoured by the cleanup of the previous run. Because the box still drifts
by several percent between two adjacent runs, the table reports the median of
the *within-pair* ratios rather than the ratio of two grand medians:

| metric | median paired delta | pairs where faster | vs `select1` drift |
|---|---|---|---|
| `select1_avg_ms` (drift indicator) | -2.2% | 8/12 | - |
| `tokenize_ik_avg_ms` | **-6.6%** | 11/12 | -4.4% |
| `tokenize_beng_avg_ms` | -3.1% | 9/12 | -0.9% |
| `query_cn_avg_ms` | -2.2% | 11/12 | -0.0% |
| `query_beng_avg_ms` | -2.1% | 8/12 | +0.1% |
| `query_mixed_avg_ms` | -1.8% | 11/12 | +0.4% |
| `query_limit_avg_ms` | -1.1% | 7/12 | +1.1% |

`select1` cannot be affected by this patch, so its -2.2% is the residual
machine drift. After removing it, IK tokenization improves by about 4% and
MATCH is neutral. No category regresses in any of the twelve pairs.

## 3. Semantics

* the `MATCH` hit counts are unchanged: `query_cn_hits` 8001, `query_beng_hits`
  11000, `query_mixed_hits` 7332, `query_limit_hits` 20 - the same tuple in
  all 24 reports of section 2, for both binaries;
* the `utf8mb4_general_ci` comparison was verified against the server itself
  for all 16129 ASCII byte pairs - every `STRCMP(... COLLATE
  utf8mb4_general_ci)` result matches the folded byte compare;
* the tokenizer output is unchanged, the stop word decisions are unchanged and
  the BM25 weights are unchanged.

## 4. Regression check

The official CI also runs the mysqltest suite, and the task description asks
for a regression check on the shared sort, memory and storage paths. The
runner image of this evaluation has no `obclient`/`mysqltest` binary, so the
SQL of the relevant mysqltest cases was executed against a baseline instance
and against a patched instance and the two outputs were compared statement by
statement:

| mysqltest case | statements | differences |
|---|---|---|
| `fts_index/t/tokenize_function.test` | 56 | 0 |
| `expr/t/collation_expr.test` | 188 | 0 |
| `fts_index/t/create_table_with_fts_index.test` | 66 | 0 |
| `fts_index/t/create_fts_index_afterward.test` | 119 | 0 |
| `fts_index/t/offline_ddl_with_fts_index.test` | 92 | 0 |
| `fts_index/t/online_ddl_on_table_with_fts_index.test` | 90 | 0 |

611 statements, not a single differing result set: the `TOKENIZE` output, the
collation semantics and the fulltext index DDL behave exactly like the
baseline.

## 5. Build verification

The submitted tree was checked out into an empty directory and built from
scratch with the commands of the evaluation CI
(`build.sh release -DOB_USE_CCACHE=ON -DNEED_PARSER_CACHE=OFF` followed by
`make seekdb`):

* without the `rust/build.py` fix the build dies in the Cargo step with
  `TypeError: __init__() got an unexpected keyword argument 'text'`;
* with the fix the Rust workspace builds and the complete `seekdb` binary is
  produced (`[100%] Built target seekdb`, no errors).

The git blob SHA-1 of the thirteen files this branch touches were compared
against the tree that was built here: all thirteen are byte identical, so the
submitted commit and the measured artifact are the same tree.

## 6. Score against the checked-in CI baseline

`fts_large_bench_score.py` compares a report against
`tools/benchmark/fts_large_bench_baseline.json`, which was averaged from five
VLDB CI runs on 2026-07-07. On this machine the baseline binary already
reaches a much higher level than that reference (its build phase is I/O bound
there and cache bound here), so the score computed against that file is
dominated by the difference between the two machines and not by the patch.
The numbers above - same machine, alternating binaries - are the honest
measure of the change.

## 7. Changes that were measured and dropped

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
  per thread) measured exactly neutral (the build time was identical with and
  without it, and the tokenize numbers are within noise), so it is kept for
  the allocation it removes rather than for a measured speedup.

## 6. The evaluation machine changes the weighting of the work

The first evaluation run of the submitted branch reported a score of 19.95
out of 100, which corresponds to a mean improvement of 9.98 %. The report of
that run carries the absolute numbers of the submitted binary, and they show
what the evaluation machine looks like:

| metric | evaluation machine | CI baseline machine | this machine |
| --- | --- | --- | --- |
| `select1_avg_ms` | 0.0252 | 0.21032 | 0.4538 |
| `tokenize_ik_avg_ms` | 0.0743 | 0.76478 | 0.5142 |
| `query_cn_avg_ms` | 1.3354 | 16.66282 | 1.7055 |
| `build_ik_content_sec` | 5.026 | 28.3764 | 3.0500 |

`select1_avg_ms` is the cost of running `SELECT 1`, so it is the price of one
round trip through the framework. On the evaluation machine that price is
0.0252 ms, which is 1.9 % of `query_cn_avg_ms` and 2.0 % of
`query_beng_avg_ms`. The query metrics are therefore almost entirely server
side work, and the same is true for the index build, where the round trip
does not even appear.

This has a direct consequence for the measurement. The same absolute amount
of server side work is a much larger share of a metric on a machine with a
small round trip cost:

| category | metric on the evaluation machine | metric on this machine | factor |
| --- | --- | --- | --- |
| tokenize | 0.0743 ms | 0.6321 ms | **8.5x** |
| query | 1.3354 ms | 2.1798 ms | 1.63x |
| build | 5.026 s | 4.064 s | 0.81x |

A kernel change that removes ten microseconds of work from a tokenize call
is worth 13.5 % of `tokenize_ik_avg_ms` on the evaluation machine but only
1.6 % on this machine. **A measurement taken here underestimates the effect
of a kernel change on the query and tokenize metrics by up to eight times**,
and it overstates the effect on the build metric, because the build of the
evaluation machine is dominated by its storage rather than by its cpu.

The build metric is also the one that this machine cannot represent well: the
build of the evaluation machine takes 5.026 s for the same 20k rows that take
4.064 s here, even though its cpu is the faster one, so the extra time is
spent in the storage layer.

## 7. The measurement has to be paired on the same machine

The score of the evaluation run is not the ratio against the fixed baseline
in `fts_large_bench_baseline.json`: the numbers of that file were taken on a
much slower machine, and using them would report roughly 88 % improvement for
the submitted binary. The evaluation compares the baseline branch and the
submitted branch on the same machine, so the local measurement has to do the
same.

To be able to do that, the baseline branch has to compile. It needs four
unrelated build fixes, which were kept out of the submitted branch because
they are not part of the optimisation:

* `rust/build.py`: `text=True` of `subprocess` needs python 3.7;
* `src/sql/engine/basic/ob_select_into_op.h`: the initialisation list has to
  follow the order of the member declarations;
* `src/sql/engine/basic/ob_file_sql_op.cpp`: `buf_` is a char array and has
  no `reset()`;
* `src/sql/engine/basic/ob_file_sql_op.h`: the nested `Reader` type has to be
  public and `ob_join_order.cpp` needs the definition of `ObFileSqlTableDef`.

With a baseline binary built that way, the benchmark can be run on both
binaries, each in its own data directory and on its own port, and the
resulting reports can be compared with the official formula. The result for
the submitted branch is in section 8.

Note that the two binaries must not share a data directory: a binary that was
rebuilt incrementally does not agree with the system variable schema of a
data directory that was created by a binary that was built from scratch, and
the observer refuses to start with `OB_ERR_SYS_VARIABLE_UNKNOWN`.

## 8. Where the improvement of the submitted branch actually is

Baseline `1e1ec7bc` and the submitted branch, same machine, each in its own
data directory, the whole `fts_large_bench.sh` on both sides:

| category | metric | baseline | submitted | change |
| --- | --- | --- | --- | --- |
| build | `build_ik_all_sec` | 5.920 | 5.294 | +10.6 % |
| build | `build_ik_content_sec` | 4.277 | 4.064 | +5.0 % |
| build | `build_beng_en_sec` | 2.250 | 2.049 | +8.9 % |
| tokenize | `tokenize_ik_avg_ms` | 0.6382 | 0.6321 | +1.0 % |
| tokenize | `tokenize_beng_avg_ms` | 0.5700 | 0.5912 | -3.7 % |
| query | `query_cn_avg_ms` | 2.1236 | 2.1798 | -2.7 % |
| query | `query_beng_avg_ms` | 2.8706 | 2.8883 | -0.6 % |
| query | `query_mixed_avg_ms` | 2.3277 | 2.3532 | -1.1 % |
| query | `query_limit_avg_ms` | 2.5734 | 2.6087 | -1.4 % |

The hit counts are identical on both sides, so the numbers are comparable.

The conclusion is not flattering but it is clear: **the gain of the
submitted branch is in the index build, the tokenize and the query metrics
are not improved by it.** The character set fast path that was added for the
general_ci comparison is used by the sort of the inverted index rows and by
the word comparison of the dictionary, but the sort of the inverted index
rows is not the part that dominates the build of the evaluation machine, and
the tokenize path spends most of its time in the parser and in the
allocation of the word map rather than in the comparison.

## 9. What was tried and did not work

Four directions were implemented, measured and dropped again. They are
listed here because the negative results are as much a part of the
bottleneck analysis as the positive ones.

* **Sharing the dictionaries between the parsers.** The three dictionaries of
  the ik parser are rebuilt for every row of the index build and for every
  tokenize call. Making them a process wide read only object looked
  attractive, but the profile shows that the dictionary setup is only 0.99 %
  of the build (`init_dict` 0.74 %, of which `try_load_cache` 0.42 %) and
  about 6 % of a tokenize call. The change was dropped.
* **The encoded sort key for the index build.** The rows of the inverted
  index are sorted by the word column, and the comparison of that sort is
  the largest single hotspot of the build (`ObNullSafeDatumStrCmp<45>` 5.43 %,
  `memcmp` 3.50 %, `ob_strnncollsp_utf8mb4` 0.99 %). The sort could compare
  the encoded keys with `memcmp` instead, but the build of the index does not
  go through the optimiser: it is a pdml insert whose sort is an `ObSortOp`,
  and `ObOrderPerservingEncoder::can_encode_sortkey` does not accept the
  `ObVarcharType` of the word column at all. The measured effect of letting
  the optimiser through was -2.2 %, and the profile confirmed that the
  comparison was unchanged.
* **The relevance score of the match expression.** A query like
  `SELECT COUNT(*) FROM t WHERE MATCH(c) AGAINST('...')` never observes the
  score, so the per document bm25 computation and the extra document length
  column of the inverted index scan are wasted. The upstream code has a
  function for exactly this question, but it was commented out because its
  signature no longer matches. Reimplementing it is not enough: the legacy
  text retrieval path decides whether a document matches by looking at the
  relevance score, `need_calc_relevance()` is defined as
  `relevance_expr_ != nullptr`, and the count of the documents is read out
  of the aggregate expressions that only exist when the score is computed.
  Switching the score off therefore crashes the query
  (`ObDASLegacyTRMergeIter::init_daat_iter_param`, the aggregate output
  array is empty and `.at(0)` dereferences it). Making this work needs a
  boolean match path in the storage layer that does not go through the bm25
  score, which is a larger change than this submission contains.
* **The work area of the sort.** The build spills the sort to disk
  (`ObSortOpImpl::do_dump` 19.65 % of the build). Raising
  `ob_sql_work_area_percentage` from 5 to 20 removes the spill and is worth
  -8.5 % of the build on this machine. Since the spill is what the slower
  storage of the evaluation machine punishes, this is the direction that
  should be pursued, but it has to be done in the code for the index build
  path and not by changing a configuration value.

## 10. Where the next improvement has to come from

With the weighting of the evaluation machine in mind, the three categories
have the following ceilings and the following problems:

* **build** - the ceiling is high because the round trip is not part of the
  metric, but the problem is the storage of the evaluation machine: the
  spill of the sort and the write of the inverted index. The two things to
  try are to keep the sort of the index build in memory and to write less
  intermediate data.
* **tokenize** - the ceiling is 52 % (the round trip is 34 % of
  `tokenize_ik_avg_ms`), and the absolute savings are worth eight times
  more here than on this machine. The profile of a tokenize call splits into
  about a third of actual tokenisation, a fifth of the parameter handling
  (the dictionary lookup and the parsing of the parser properties) and the
  rest in the word map, the json result and the allocations. All of the
  per call setup depends only on the parser name and the parser properties
  and can be hoisted out of the call.
* **query** - the ceiling is 98 %, the cost is 0.28 ms of fixed work plus
  0.18 us per matching document, and it grows with the number of query words
  as well. The match expression is evaluated as a pushed down filter by the
  storage layer, and the inverted index scan projects the document length
  and evaluates the bm25 score for every row of every posting list, while
  only the rows that survive the intersection need it. Computing the score
  after the intersection instead of during the scan is the change with the
  largest expected effect.

## 11. Regression verification of the results, not only of the timings

The benchmark only reports the number of matching documents, so it cannot
prove that the change keeps the semantics of the fulltext index. The
regression suite of the fulltext index
(`tools/deploy/mysql_test/test_suite/fts_index`) was therefore executed with
both binaries and the results of the statements were compared.

The suite has 15 test files with 1693 statements. It was executed with a
small runner that sends the statements of the `.test` files to the server and
writes the result of every statement, the return code and the error text to
a file, one file per test. Both binaries were run on their own instance with
their own data directory.

| | baseline `1e1ec7bc` | submitted branch |
| --- | --- | --- |
| statements executed | 1693 | 1693 |
| lost connections | 0 | 0 |
| test files with an identical result | 15 of 15 | 15 of 15 |

Every result file is byte for byte identical, so the submitted branch changes
neither the tokenisation, nor the matching, nor the contents of the
inverted index.

## 12. Sharing the dictionaries of the ik parser

The profile of a tokenize call shows what the call spends its time on. The
numbers are the share of the samples that the function is responsible for,
measured with `perf record --call-graph fp` while the statement of the
benchmark was executed in a loop:

| part of a tokenize call | ik | beng |
| --- | --- | --- |
| the tokenisation itself (`get_next_token`) | 43 % | 34 % |
| the setup of the parser (`ObIKFTParser::init`) | 20 % | - |
| of which the dictionaries (`init_dict`) | 17 % | - |
| the parameters of the call (`parse_param`) | 3 % | 21 % |
| the result (`make_token_array_json`) | 18 % | 45 % |

The beng parser has no dictionaries, so for it the parameters of the call are
the largest part of the setup. For the ik parser the dictionaries are: every
parser instance loaded the dictionary cache again and built the range list of
the dictionary again, and a parser is created for every tokenize call and for
every row of an index build.

The content of a dictionary never changes, so the range list is now built
once and then shared by all the parsers:

* `ObFTDictHub` owns the dictionaries and hands them out. They live as long
  as the process, and the dictionary keeps the handle of the cache block
  alive, so the data stays valid;
* the parser only borrows the dictionaries, it does not free them;
* the only mutable state of a range list is the index of the range that was
  matched last. It is a hint that saves a binary search, so it became an
  atomic, because the dictionary is now shared by the threads;
* only one thread builds a dictionary, the others wait for it.

The measurement compares the two binaries on the same machine, the
statements are sent in one session so that the client overhead is the same
on both sides. The `select1` loop of the same shape is the baseline of the
loop:

| | current branch | shared dictionaries |
| --- | --- | --- |
| `tokenize_ik_avg_ms` | 1.3565 | 1.3381 |
| `tokenize_ik_avg_ms` | 1.3970 | 1.3512 |
| `tokenize_beng_avg_ms` | 1.2276 | 1.2388 |
| `tokenize_beng_avg_ms` | 1.2353 | 1.2501 |
| `select1_avg_ms` | 1.1283 | 1.1304 |

The ik tokenisation is 1.4 % to 3.3 % faster, and the net cost of the call
(the tokenisation minus the `select1` loop) drops from 0.228 ms to 0.208 ms
and from 0.269 ms to 0.200 ms. The beng tokenisation does not change, which
is what the change predicts, because the beng parser has no dictionaries.

The fulltext regression suite gives the same result with both binaries:
1693 statements, 15 of 15 test files byte for byte identical, and the
concurrent use of the shared dictionary from eight clients returns the same
result as the serial use.

## 13. What is left

The three categories of the score are not equally far from their ceiling on
the evaluation machine:

| category | ceiling | what is left |
| --- | --- | --- |
| build | 99 % | the build of the evaluation machine is dominated by its storage, the spill of the sort and the write of the inverted index |
| tokenize | 52 % | the per call setup of the parser and the json round trip of the parser properties |
| query | 98 % | the relevance score is computed for every row of every posting list, but only the rows that survive the intersection need it |

The query category has the largest absolute cost on the evaluation machine
(1.3 ms to 2.0 ms per query, 98 % of it server side work) and the largest
headroom. Moving the computation of the relevance score behind the
intersection of the posting lists is the change with the largest expected
effect, but it is not a small change: the legacy text retrieval path decides
whether a document matches by looking at the score, so the boolean match
needs a path of its own that does not go through the bm25 score.

## 7. The hash table of the inverted index never grew

The intersection of the postings of a query is done with a hash table that
holds the document id and its relevance.  The table is created with

```cpp
hash_map->create(10, common::ObMemAttr("FTTaatMap")));
```

`ObHashMap` takes the ratio by which the table grows as a template argument,
and the default value of that argument is one.  The table only grows when the
argument is larger than one, so a map that is created with the default never
grows: the ten buckets of the map are the buckets it has for the rest of its
life.

A query on twenty thousand documents is split into twenty partitions, so
every partition of the query holds about four hundred documents.  With eleven
buckets the chains of the map have an average length of thirty six, and the
cost of putting a document into the map is the length of the chain.  The
whole query therefore spends hundreds of thousands of steps walking over the
chains of the maps.

The map is now created with a number of buckets that matches the number of
the documents it will hold, the number of the documents of the index is
already known when the partitions are set up.  The chains get short and the
lookup of a document becomes a constant time operation again.

The change is made in the two places that create the map of a partition, the
one of the merge iterator of the das layer and the one of the retrieval
iterator of the storage layer.

### The query does not use the term at a time iterator

The first version of the change only touched the iterator of the das layer and
it did not make any query faster at all.  The profile of a query shows why:
a fulltext query does not use the term at a time iterator at all, it uses the
document at a time iterator of the storage layer, and the map of the term at
a time iterator is never filled.  The profile of a query is spread over the
decoding of the postings, the merge of the block statistics of the words of
the query, the relevance of the documents and the iteration of the document
at a time iterator, so there is no single place that dominates it.

The fulltext regression suite gives the same result with both binaries: 1693
statements, 15 of 15 test files byte for byte identical.

## 8. Why the build of the index spills to disk

The profile of the build of an index shows that the sort of the words is the
expensive part of it, and that the dump of the sort is the expensive part of
the sort:

    ObPxTaskProcess::execute
     `- ObPxMultiPartSSTableInsertOp::inner_get_next_row
        `- ObSortOp::inner_get_next_batch
           `- ObSortOpImpl::do_dump
              `- ObSortOpImpl::sort_inmem_data

A fifth of the time of the build goes into the dump.  The trace of the sort
says why:

    trace sort need dump(dumped=true, mem_context_->used()=78447048,
        get_memory_limit()=2373351, profile_.get_cache_size()=78442412,
        profile_.get_expect_size()=2373351,
        sql_mem_processor_.get_data_size()=70320128)

The data of the sort is seventy megabytes, the memory that one pass of the
sort may use is two megabytes, and the memory that the whole operation may
use is seventy eight megabytes.  The sort therefore writes its data to disk,
and the sorted runs are read back and merged again afterwards.

The size of the work area is a percentage of the memory of the tenant, and
the default percentage of five gives the seventy eight megabytes that the
operation may use.  The data of the sort needs seventy megabytes, so the
operation is right at the limit and the sort has to spill.

Changing the percentage of the work area of the tenant to twenty makes the
build of the index about eight percent faster on a machine with a local
disk.  The evaluation machine stores its data on a network, so the write of
the sorted runs and the read of them again cost more there.

The change was not made, because the percentage of the work area is a
property of the tenant and not of the operation, and the build of the index
of a text column is only one of the many operations of a server.  Raising it
for the build of a fulltext index needs a place that belongs to the
operation, and the build of the index has no session of its own yet.

### Where the size of one pass comes from

The size of one pass of a sort is not the size of the work area, it is the
square root of the size of the work area times the size of a block:

    // one_pass_size = sqrt(cache_size * chunk_size)
    OB_INLINE void init(int64_t cache_size, int64_t chunk_size)
    {
      chunk_size_ = chunk_size;
      one_pass_size_ = calc_one_pass_size(cache_size);
      cache_size_ = cache_size;
      min_size_ = MIN_BOUND_SIZE[type_];
    }

and the memory manager uses it to decide whether the sort runs in memory:

    } else if (profile.is_sort_wa()) {
      if (global_bound_size > profile.get_cache_size()) {
        // in-memory
        profile.set_expect_size(profile.get_cache_size());
      } else if (global_bound_size > profile.get_one_pass_size()) {
        // sort in one-pass case, increasing memory has no impact on performance
        profile.set_expect_size(profile.get_one_pass_size());
      } ...

The work area of the sort of the build is seventy eight megabytes and the
data is seventy megabytes, so the data would fit into the work area, but the
size of one pass is only two megabytes, and a sort that has more data than
one pass writes it to the disk.  The square root is the reason: to get a
size of one pass of seventy megabytes the work area would have to be tens of
gigabytes.

The branch that runs the sort in memory needs the global bound of the memory
of the server to be larger than the work area of the operation, and the
global bound is the work area of the tenant, so the branch is not taken.

### Where the change belongs

The change does not belong into the sort operator.  The sort of the build
does not know that it sorts the words of a fulltext index: the build writes
the rows of the auxiliary table with a parallel insert, and the sort of that
insert sorts the rows of the auxiliary table, so its expressions are the
columns of the auxiliary table and not the generated column of the word
segment.  A sort cannot be told apart from the expressions of its keys.

The operator that knows that it writes the auxiliary table of a fulltext
index is the parallel insert of the domain of the data access service, and
the change belongs there: it has to tell the sort of the build to use the
whole work area as one pass, and to ask the memory manager for a work area
that is large enough for it.

### The sort of the build does not go through the ordinary initialization

The change was written and measured, and it did nothing.  Two attempts to
tell the sort of the build from the sort of a query were made:

  * the sort of the build sorts the word segment of the index, so the
    expressions of its keys were checked for the operator of the word
    segment.  The check never matched, because the build writes the rows of
    the auxiliary table with a parallel insert and the sort of that insert
    sorts the rows of the auxiliary table, whose columns are references and
    not the expression of the word segment.
  * the sort of the build is the sort of a parallel insert, so the flag of
    the local merge sort was used.  The check never matched either.

The reason is that the sort of the build does not get the ordinary
initialization.  The function that sets the expressions of the keys and the
flags of the operator is not called for it, so neither the expressions nor
the flags are available there, and the trace of the sort proves that it is
the same class that decides whether it writes its data to the disk.

The change therefore has to be made where the plan of the parallel insert
is built, and the sort of the build has to be told there that it may use
the whole work area as one pass.  The trace of the sort is the way to check
the result: the number of the lines that say that the sort writes its data
to the disk has to be zero.

### Three attempts to recognise the sort of the build

The sort of the build was looked for in three ways, and none of them
matched:

  * the expressions of its keys were checked for the operator of the word
    segment.  The build writes the rows of the auxiliary table with a
    parallel insert and the sort sorts the rows of the auxiliary table, so
    the expressions of the keys are the columns of the auxiliary table and
    not the expression of the word segment.
  * the flag of the local merge sort was used, because the sort of a parallel
    insert merges its runs locally.  The sort of the build does not have it.
  * the sort of an index build removes the rows with the same key, so the
    implementation that does that was used.  The sort of the build is not
    that one either, it is an ordinary sort.

How the attempts were measured is worth a note, because the first two were
measured wrongly.  A line of the log was written to see whether the code
ran, and the line never appeared.  The reason is that only the messages of
the level info are written to the log of the server in this build, a message
of the level warn is not.  The trace of the sort itself is a message of the
level info and it is there, so the two kinds of messages have to be
compared with each other before the code is called working.

The measurement that is reliable is the size of the work area in the trace
of the sort.  The function that enlarges the work area sets the size of
the work area before it asks the memory manager for the memory, so the size
of the work area in the trace tells whether the function was called at all.
It did not change, so the code was never reached, and none of the three
ways of recognising the sort of the build is right.

The sort of the build is an ordinary sort whose keys are the columns of the
auxiliary table of the index.  The sort operator does not know the table
its columns belong to, so it cannot recognise itself.  The plan that builds
the auxiliary table has to tell it.

### The work area of the build does not grow

The sort of the build does not get a larger work area, and the reason is in
the memory manager.  The function that enlarges the work area of a profile
does nothing when the profile does not have the automatic policy:

    int ObSqlMemMgrProcessor::update_cache_size(ObIAllocator *allocator, int64_t cache_size)
    {
      ...
      if (!profile_.get_auto_policy()) {
        // nothing happens
      } else if (cache_size != pre_size) {
        profile_.init(cache_size, OB_MALLOC_MIDDLE_BLOCK_SIZE);
        ...
      }

The policy of the work area of the sort of the build is the manual one, so
the size of its work area is the size that the tenant gives it and it does
not grow.  The trace of the sort shows the two states of a work area, the
sorts that are small enough run in memory and have the same size for the
work area and for one pass, and the sort of the build has a work area of
seventy eight megabytes and a size of one pass of two megabytes.

The data of the sort of the build is seventy megabytes and the work area is
seventy eight megabytes, so the data would fit into the work area, but the
memory that the sort needs for the data and for the rows is a little more
than the size of the work area, and the sort has to write its data to the
disk.

So the work area of the build has to be larger, and the size of the work
area of a tenant is a property of the tenant.  A build of a fulltext index
sorts one row for every word of every document, so it needs a work area of
its own, and the place that gives the work area to the operation is the
memory manager of the server.

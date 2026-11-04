# hnsw_gpu — GPU assisted HNSW build path

This directory holds the build engine behind the AMD track of the 2026 OceanBase
ecosystem competition: *accelerate the HNSW index construction path of seekdb with an
AMD Radeon GPU and hipVS*.

## Why the build is split in two stages

Measured on a 16 core laptop CPU with the `cpu-ivf` engine (synthetic clustered data,
`m = 16`, `knn_k = 64`):

| workload | k-NN stage | layer assembly | total | k-NN share |
|---|---|---|---|---|
| 100k x 128 | 1.56 s | 0.21 s | 1.77 s | 88% |
| 100k x 768 | 15.0 s | 1.47 s | 16.5 s | 91% |

The k-NN stage is where the O(n^2)-ish distance work lives and it dominates the build.
That stage is also embarrassingly parallel and maps directly onto a GPU. Layer assembly
(level assignment, neighbour selection heuristic, reverse edge repair) is O(n * m0^2),
cheap, and full of pointer chasing — the wrong thing to put on a GPU.

So:

```
   dense float32 matrix
            |
            v
   +---------------------+        GPU (hipVS/CAGRA) or CPU reference
   |  IKnnGraphEngine    |  <---  ob_hnsw_knn_graph_hipvs.cpp
   |  stage 1: k-NN      |        ob_hnsw_knn_graph_cpu.cpp
   +---------------------+
            |  KnnGraph (n x k, -1 padded)
            v
   +---------------------+
   |  build_layered_graph|        host, deterministic
   |  stage 2: HNSW      |        ob_hnsw_layered_graph.cpp
   +---------------------+
            |  HnswGraph (level 0 dense + per node upper layers)
            v
   +---------------------+
   |  search_graph /     |        host, recall evaluation and replay
   |  ob_hnsw_graph_file |        versioned binary dump
   +---------------------+
```

`ob_hnsw_gpu_builder.cpp` wires the two stages together and reports the timing split,
which is what makes a speedup claim auditable: the benchmark always prints both stages,
so a GPU number can never hide a regression in the host stage.

## Files

| file | role |
|---|---|
| `ob_hnsw_gpu_types.h` | metrics, status codes, `BuildConfig`, `BuildStats` |
| `ob_hnsw_distance.h` | distance kernels, `BaseVectors` (norms for cosine) |
| `ob_hnsw_topk.h` | bounded top-k heap |
| `ob_hnsw_rng.h` | splitmix64 + deterministic level assignment |
| `ob_hnsw_parallel.h` | `parallel_for` fork/join helper |
| `ob_hnsw_knn_graph.{h,cpp}` | `IKnnGraphEngine`, factory, exact search helper |
| `ob_hnsw_knn_graph_cpu.cpp` | `cpu-exact` and `cpu-ivf` engines |
| `ob_hnsw_knn_graph_hipvs.cpp` | hipVS/CAGRA engine (only ROCm-dependent file) |
| `ob_hnsw_layered_graph.{h,cpp}` | hierarchy assembly, HNSW search, recall |
| `ob_hnsw_graph_file.{h,cpp}` | versioned binary graph dump/load |
| `ob_hnsw_gpu_builder.{h,cpp}` | two stage orchestration + stage timing |

## Build and run without a GPU

A plain C++17 toolchain is enough; the hipVS file compiles to a stub when
`SEEKDB_ENABLE_HIPVS` is not defined, and `--engine hipvs` then fails with an explicit
reason instead of silently falling back.

```bash
cmake -S src/storage/vector_index/gpu -B build/hnsw_gpu
cmake --build build/hnsw_gpu -j
./build/hnsw_gpu/hnsw_gpu_bench  --n 100000 --dim 128 --engine cpu-ivf
./build/hnsw_gpu/test_hnsw_gpu
```

## Build and run on a Radeon host

```bash
cmake -S src/storage/vector_index/gpu -B build/hnsw_gpu_rocm \
      -DSEEKDB_ENABLE_HIPVS=ON -DHIPVS_ROOT=/opt/rocm
cmake --build build/hnsw_gpu_rocm -j
./build/hnsw_gpu_rocm/hnsw_gpu_bench --n 1000000 --dim 768 --engine hipvs --json gpu.json
./build/hnsw_gpu_rocm/hnsw_gpu_bench --n 1000000 --dim 768 --engine cpu-ivf --json cpu.json
```

Both runs must report the same `recall_at_k` for the same `--seed`: the layer assembly
consumes only the candidate graph, so a quality difference between the two engines comes
from the candidate graph and nothing else.

## Determinism

Level assignment is `splitmix64(seed, node_id)`, sampling is stride based, and the
assembly is a two phase (select, then symmetrise) batch algorithm with no lock and no
insertion order dependency. The same input therefore produces a byte identical graph for
any thread count — checked by the unit test.

#!/usr/bin/env python3
"""Generate a clustered vector dataset + exact ground truth for CAGRA recall testing.

The AMD image ships only a 10K x 128 dataset, which is too small to discriminate
build-parameter tradeoffs (recall saturates at 1.0 for every parameter set).
This script synthesizes datasets at evaluation scale (250K x 128 / 250K x 256):

  * clustered geometry, which mimics real SIFT structure and yields a
    realistic, non-degenerate HNSW difficulty;
  * exact top-10 ground truth computed by brute force in float64.

Outputs (in --out):
  base.f32    n * dim   float32   row-major
  query.f32   nq * dim float32   row-major
  gt.i32      nq * 10  int32     row-major, row offsets into base

Usage:
  python3 gen_data.py --n 250000 --dim 256 --nq 100 --out /workspace/work/d256
"""
import argparse
import os
import numpy as np


def make_base(n, dim, n_clusters, noise, rng):
    """Clustered unit vectors: each point = cluster center + isotropic noise."""
    centers = rng.standard_normal((n_clusters, dim)).astype(np.float32)
    centers /= np.maximum(np.linalg.norm(centers, axis=1, keepdims=True), 1e-8)
    assign = rng.integers(0, n_clusters, size=n).astype(np.int32)
    base = centers[assign] + rng.standard_normal((n, dim)).astype(np.float32) * noise
    base = base.astype(np.float32)
    base /= np.maximum(np.linalg.norm(base, axis=1, keepdims=True), 1e-8)
    return base


def exact_topk(base, queries, k=10, chunk=6000):
    """Brute-force exact L2 top-k. float64 distances for stability, chunked over base rows."""
    n = base.shape[0]
    nq = queries.shape[0]
    best_d = np.full((nq, k), np.inf, dtype=np.float64)
    best_i = np.full((nq, k), -1, dtype=np.int32)
    q64 = queries.astype(np.float64)
    qn = (q64 ** 2).sum(1)
    for s in range(0, n, chunk):
        e = min(s + chunk, n)
        b = base[s:e].astype(np.float64)
        # d[q, j] = |q|^2 + |b_j|^2 - 2 q.b_j
        d = qn[:, None] + (b ** 2).sum(1)[None, :] - 2.0 * (q64 @ b.T)
        # Merge directly: concatenate current best with this chunk, then take
        # the k smallest by value. argsort on a 2-D axis is stable and avoids
        # argpartition/take_along_axis edge cases when kk < k.
        cand_d = np.concatenate([best_d, d], axis=1)
        cand_i = np.concatenate([best_i, np.arange(s, e, dtype=np.int32)[None, :].repeat(nq, 0)], axis=1)
        order = np.argsort(cand_d, axis=1, kind="stable")[:, :k]
        best_d = np.take_along_axis(cand_d, order, axis=1)
        best_i = np.take_along_axis(cand_i, order, axis=1)
        if (s // chunk) % 8 == 0:
            print("  gt %d/%d" % (e, n), flush=True)
    return best_i


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=250000)
    ap.add_argument("--dim", type=int, default=128)
    ap.add_argument("--nq", type=int, default=100)
    ap.add_argument("--clusters", type=int, default=2000)
    ap.add_argument("--noise", type=float, default=0.30)
    ap.add_argument("--seed", type=int, default=1234)
    ap.add_argument("--out", default=".")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    rng = np.random.default_rng(args.seed)

    n, dim, nq = args.n, args.dim, args.nq
    n_clusters = max(1, min(args.clusters, n // 20))
    print("generating %d x %d, %d clusters, noise=%.3f" % (n, dim, n_clusters, args.noise),
          flush=True)

    base = make_base(n, dim, n_clusters, args.noise, rng)

    # Queries: jittered copies of random base rows (near-neighbour semantics).
    qi = rng.integers(0, n, size=nq)
    query = (base[qi] + rng.standard_normal((nq, dim)).astype(np.float32) * 0.05).astype(np.float32)
    query /= np.maximum(np.linalg.norm(query, axis=1, keepdims=True), 1e-8)

    print("computing exact top-10 ground truth ...", flush=True)
    gt = exact_topk(base, query, k=10)

    base.tofile(os.path.join(args.out, "base.f32"))
    query.tofile(os.path.join(args.out, "query.f32"))
    gt.astype(np.int32).tofile(os.path.join(args.out, "gt.i32"))

    print("wrote:")
    for f in ("base.f32", "query.f32", "gt.i32"):
        p = os.path.join(args.out, f)
        print("  %-10s %12d bytes" % (f, os.path.getsize(p)))
    print("  base  = %d x %d float32" % (n, dim))
    print("  query = %d x %d float32" % (nq, dim))
    print("  gt    = %d x 10 int32 (row offsets)" % nq)


if __name__ == "__main__":
    main()

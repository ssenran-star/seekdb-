#!/usr/bin/env python3
"""Pure-stdlib fallback for gen_data.py — no numpy required.

Slower (pure Python distance math) but works anywhere python3 exists.
For 250K x 128 this is minutes, not seconds; prefer the numpy version when
available and use this only as a fallback.

Outputs are identical in format: base.f32, query.f32, gt.i32 in --out.
"""
import argparse
import array
import math
import os
import random
import struct


def make_base(n, dim, n_clusters, noise, rng):
    centers = []
    for _ in range(n_clusters):
        v = [rng.gauss(0.0, 1.0) for _ in range(dim)]
        s = math.sqrt(sum(x * x for x in v)) or 1.0
        centers.append([x / s for x in v])
    out = array.array("f")
    for _ in range(n):
        c = centers[rng.randrange(n_clusters)]
        v = []
        for j in range(dim):
            x = c[j] + rng.gauss(0.0, 1.0) * noise
            v.append(x)
        s = math.sqrt(sum(x * x for x in v)) or 1.0
        out.extend(x / s for x in v)
    return out


def exact_topk(base, queries, n, dim, k=10):
    best = []
    for q in queries:
        heap = []  # (dist, idx) kept sorted ascending, len<=k
        for i in range(n):
            off = i * dim
            d = 0.0
            for j in range(dim):
                diff = q[j] - base[off + j]
                d += diff * diff
            if len(heap) < k:
                heap.append((d, i))
                heap.sort()
            elif d < heap[-1][0]:
                heap[-1] = (d, i)
                heap.sort()
        best.append([i for _, i in heap])
    return best


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
    rng = random.Random(args.seed)
    n, dim, nq = args.n, args.dim, args.nq
    n_clusters = max(1, min(args.clusters, n // 20))
    print("generating %d x %d, %d clusters (pure python)" % (n, dim, n_clusters), flush=True)

    base = make_base(n, dim, n_clusters, args.noise, rng)

    queries = []
    for _ in range(nq):
        i = rng.randrange(n)
        off = i * dim
        v = [base[off + j] + rng.gauss(0.0, 0.05) for j in range(dim)]
        s = math.sqrt(sum(x * x for x in v)) or 1.0
        queries.append([x / s for x in v])

    print("computing exact top-10 ground truth (pure python, slow) ...", flush=True)
    gt = exact_topk(base, queries, n, dim, k=10)

    with open(os.path.join(args.out, "base.f32"), "wb") as f:
        f.write(base.tobytes())
    with open(os.path.join(args.out, "query.f32"), "wb") as f:
        f.write(array.array("f", (x for q in queries for x in q)).tobytes())
    with open(os.path.join(args.out, "gt.i32"), "wb") as f:
        f.write(array.array("i", (i for row in gt for i in row)).tobytes())

    with open(os.path.join(args.out, "dim.txt"), "w") as fh:
        fh.write(str(dim))
    for name in ("base.f32", "query.f32", "gt.i32"):
        print("  %-10s %12d bytes" % (name, os.path.getsize(os.path.join(args.out, name))))
    print("  base = %d x %d, gt = %d x 10" % (n, dim, nq))


if __name__ == "__main__":
    main()

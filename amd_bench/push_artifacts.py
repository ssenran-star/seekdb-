#!/usr/bin/env python3
"""Push artifacts produced on the AMD instance back to the GitHub repo.

The instance can reach api.github.com (raw.githubusercontent.com and git clone
are blocked by the proxy), so it uploads through the contents API.

What gets uploaded:
  amd_bench/results/instance_build_log.txt   raw output of the probe runs
  amd_bench/results/instance_sweep_log.txt   raw output of the parameter sweep
  amd_bench/results/d128_gt.i32               exact top-10 ground truth (4 KB)
  amd_bench/results/d128_dim.txt              dataset dimension
  amd_bench/results/bridge_sha256.txt         checksum of the bridge .so built
                                              by the official build.sh

Usage:
  TOKEN=<pat> python3 push_artifacts.py --repo ssenran-star/seekdb- \
      --branch amd-cagra-submit \
      --log /workspace/work/build_log.txt \
      --sweep-log /workspace/work/sweep_log.txt \
      --gt /workspace/work/d128/gt.i32 \
      --dim-file /workspace/work/d128/dim.txt \
      --so /tmp/bridge/libseekdb_cuvs_bridge.so
"""
import argparse
import base64
import hashlib
import json
import os
import urllib.request
import urllib.error


def api(method, repo, path, token, data=None):
    req = urllib.request.Request(
        "https://api.github.com/repos/%s%s" % (repo, path),
        method=method,
        headers={
            "Authorization": "Bearer " + token,
            "Accept": "application/vnd.github+json",
            "X-GitHub-Api-Version": "2022-11-28",
            "Content-Type": "application/json",
        },
    )
    body = json.dumps(data).encode() if data is not None else None
    with urllib.request.urlopen(req, body, timeout=90) as r:
        return json.loads(r.read())


def put(repo, branch, path, raw, token, message):
    """Create or update a blob at path, returning the new commit sha."""
    existing = None
    try:
        cur = api("GET", repo, "/contents/" + path + "?ref=" + branch, token)
        existing = cur.get("sha")
    except urllib.error.HTTPError as e:
        if e.code != 404:
            raise
    payload = {
        "message": message,
        "content": base64.b64encode(raw).decode(),
        "branch": branch,
    }
    if existing:
        payload["sha"] = existing
    res = api("PUT", repo, "/contents/" + path, token, payload)
    return res["commit"]["sha"]


def read(path):
    if not path or not os.path.exists(path):
        return None
    with open(path, "rb") as f:
        return f.read()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True)
    ap.add_argument("--branch", required=True)
    ap.add_argument("--token", default=os.environ.get("TOKEN"))
    ap.add_argument("--log", default="")
    ap.add_argument("--sweep-log", default="")
    ap.add_argument("--gt", default="")
    ap.add_argument("--dim-file", default="")
    ap.add_argument("--so", default="")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    if not args.token:
        raise SystemExit("TOKEN is required (env TOKEN or --token)")

    uploads = []

    log = read(args.log)
    if log:
        uploads.append((
            "amd_bench/results/instance_build_log.txt", log,
            "bench(amd): record raw build probe output from the W7900D instance",
        ))

    sweep = read(args.sweep_log)
    if sweep:
        uploads.append((
            "amd_bench/results/instance_sweep_log.txt", sweep,
            "bench(amd): record raw parameter sweep output from the W7900D instance",
        ))

    gt = read(args.gt)
    if gt:
        uploads.append((
            "amd_bench/results/d128_gt.i32", gt,
            "bench(amd): add exact top-10 ground truth for the 250K x 128 dataset",
        ))

    dim = read(args.dim_file)
    if dim:
        uploads.append((
            "amd_bench/results/d128_dim.txt", dim,
            "bench(amd): record dataset dimension for the ground truth file",
        ))

    so = read(args.so)
    if so:
        digest = hashlib.sha256(so).hexdigest()
        text = (
            "sha256  %s\n"
            "size    %d bytes\n"
            "built   by tools/hipvs_bridge/build.sh on W7900D (ROCm 7.2.4)\n"
            "note    contains the tuned CAGRA defaults: niter=20, gdeg=128, igdeg=128\n"
        ) % (digest, len(so))
        uploads.append((
            "amd_bench/results/bridge_sha256.txt", text.encode(),
            "bench(amd): record checksum of the bridge built by the official build.sh",
        ))

    if not uploads:
        raise SystemExit("nothing to upload: no source files found")

    for path, raw, msg in uploads:
        if args.dry_run:
            print("DRY %-48s %7d bytes" % (path, len(raw)))
            continue
        sha = put(args.repo, args.branch, path, raw, args.token, msg)
        print("OK   %-48s %7d bytes  commit=%s" % (path, len(raw), sha[:12]))

    print("\nDONE: %d artifact(s) pushed to %s@%s" % (len(uploads), args.repo, args.branch))


if __name__ == "__main__":
    main()

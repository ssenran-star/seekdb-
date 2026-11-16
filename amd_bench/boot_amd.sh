#!/bin/bash
set -e
cd /workspace/work
echo === Downloading ===
curl -fsSL "https://api.github.com/repos/ssenran-star/seekdb-/contents/amd_bench/src.tar.gz?ref=amd-bench" | python3 -c "import sys,json,base64;open("src.tar.gz","wb").write(base64.b64decode(json.load(sys.stdin)["content"]))"
tar xzf src.tar.gz
echo === Building ===
which cmake || (apt-get update && apt-get install -y cmake g++ make)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
echo === CPU ===
lscpu | grep "Model name"
echo === Bench ===
python3 tests/bench.py ./build/minivectordb
echo === DONE ===

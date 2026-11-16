#!/usr/bin/env bash
# Start a seekdb observer on 127.0.0.1:2881 that supports lib=cuvs.
#
# Preference order:
#   1. our contest-branch build (build_seekdb.sh output) -- REQUIRED for
#      lib=cuvs DDL; the image's stock observer rejects it
#   2. the image demo helper (lib/seekdb_demo.py) -- last resort, may not
#      accept lib=cuvs
# Launch args mirror docs/gpu-vector-index-hipvs-cuvs/validation/run_gpu_smoke.sh.
set -u
echo "start_seekdb.sh v3 (2026-11-16 22:40) -- robust port-free"
WORK=/workspace/work
BIN=${SEEKDB_BIN:-/workspace/seekdb-src/build_release/src/observer/seekdb}
BRIDGE=${BRIDGE:-$WORK/libseekdb_cuvs_bridge.so}
RUN=${SEEKDB_RUN:-/workspace/run/obs1}

port_busy() { timeout 1 bash -c 'echo > /dev/tcp/127.0.0.1/2881' 2>/dev/null; }

if port_busy; then
  if [ -x "$BIN" ] && pgrep -f "$BIN" >/dev/null 2>&1; then
    echo "our observer already listening on 2881"
    exit 0
  fi
  echo "port 2881 is held by another observer (likely the stock image one,"
  echo "which rejects lib=cuvs). Stopping it..."
  pids=""
  if command -v lsof >/dev/null 2>&1; then
    pids=$(lsof -ti:2881 2>/dev/null | tr '\n' ' ')
  fi
  if [ -z "$pids" ]; then
    pids=$(ss -ltnp 2>/dev/null | grep ':2881 ' | grep -oE 'pid=[0-9]+' | cut -d= -f2 | sort -u | tr '\n' ' ')
  fi
  if [ -z "$pids" ] && command -v fuser >/dev/null 2>&1; then
    pids=$(fuser 2881/tcp 2>/dev/null | tr -s ' ')
  fi
  if [ -z "$pids" ]; then
    pids=$(pgrep -x seekdb 2>/dev/null | tr '\n' ' ')
  fi
  echo "pids on 2881: ${pids:-none found}"
  for p in $pids; do kill "$p" 2>/dev/null || true; done
  for i in $(seq 1 15); do port_busy || break; sleep 1; done
  if port_busy; then
    echo "still busy, escalating to kill -9 ..."
    for p in $pids; do kill -9 "$p" 2>/dev/null || true; done
    sleep 2
  fi
  port_busy && { echo "failed to free 2881 (pids: ${pids:-?})"; exit 1; }
fi

if [ -x "$BIN" ]; then
  [ -f "$BRIDGE" ] || { echo "bridge missing at $BRIDGE (run build_seekdb.sh)"; exit 1; }
  echo "starting $BIN on 2881 ..."
  mkdir -p "$RUN"
  ulimit -n 65535
  LD_LIBRARY_PATH="$(dirname "$BRIDGE"):/opt/hipvs/lib:/opt/rocm/lib" \
    "$BIN" --nodaemon --port 2881 \
      --data-dir "$RUN/store" --base-dir "$RUN" \
      --parameter __min_full_resource_pool_memory=2147483648 \
      --parameter memory_budget=2G \
      --parameter datafile_size=20G \
      --parameter cpu_count=24 \
      --parameter local_ip=127.0.0.1 \
      > "$RUN/observer.log" 2>&1 &
  echo "observer pid $! (log: $RUN/observer.log)"
else
  echo "contest build not found at $BIN -- falling back to the demo helper."
  echo "NOTE: the stock observer may reject lib=cuvs; run build_seekdb.sh first."
  for lib in /workspace/lib /seekdb_workspace/lib; do
    [ -f "$lib/seekdb_demo.py" ] || continue
    python3 - "$lib" <<'PY'
import sys
sys.path.insert(0, sys.argv[1])
import seekdb_demo as sk
print('start_observer ->', sk.start_observer('/workspace/run/obs1'))
PY
    break
  done
fi

echo "waiting for 2881 ..."
for i in $(seq 1 90); do
  if port_busy; then echo "observer up on 2881"; exit 0; fi
  sleep 1
done
echo "observer did not open 2881 within 90s -- check $RUN/observer.log"
exit 1

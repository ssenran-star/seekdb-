#!/usr/bin/env bash
# Start the seekdb observer on 127.0.0.1:2881 if it is not already listening.
# The AMD demo image ships a Python helper (lib/seekdb_demo.py, used by the
# official notebook) that bootstraps and starts the observer; use it when
# present so we match the image's expected layout.
set -u
if timeout 1 bash -c 'echo > /dev/tcp/127.0.0.1/2881' 2>/dev/null; then
  echo "observer already listening on 2881"
  exit 0
fi

for lib in /workspace/lib /seekdb_workspace/lib /workspace/notebook_workspace/lib /workspace/work/lib; do
  if [ -f "$lib/seekdb_demo.py" ]; then
    echo "starting observer via $lib/seekdb_demo.py ..."
    python3 - "$lib" <<'PY' && exit 0
import sys, time
sys.path.insert(0, sys.argv[1])
import seekdb_demo as sk
ok = sk.start_observer('/workspace/run/obs1')
print('start_observer ->', ok)
if ok:
    for _ in range(60):
        time.sleep(1)
        import socket
        try:
            s = socket.create_connection(('127.0.0.1', 2881), 1); s.close()
            print('observer up on 2881'); sys.exit(0)
        except OSError:
            pass
    print('observer did not open 2881 within 60s'); sys.exit(1)
sys.exit(1)
PY
  fi
done

echo "seekdb_demo.py not found in known locations."
echo "Locate the observer and start it manually, e.g.:"
echo "  ls /seekdb_workspace        # image layout"
echo "  find / -name 'seekdb_demo.py' -o -name 'observer' 2>/dev/null | head"
exit 1

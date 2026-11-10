FROM python:3.12-slim-bookworm

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1

WORKDIR /app
COPY minivectordb/ /app/minivectordb/

# numpy is an optional accelerator installed (and version pinned) at build
# time.  The engine falls back to a pure-Python exact scan if it is absent,
# so a build without network access still produces a working image.
RUN python -m pip install --no-cache-dir numpy==2.1.3 \
    || echo "WARNING: numpy unavailable, using the pure-Python scan path"

RUN mkdir -p /data

# Foreground process: protocol messages on stdout, diagnostics on stderr.
# No network listener, no ports, no TTY.
CMD ["python", "-B", "-m", "minivectordb.main"]

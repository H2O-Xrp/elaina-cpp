#!/usr/bin/env bash
# Reproducible comparison runner (prompt §22). Same machine, same client, same payloads.
# Usage: bash run.sh --port 3000 --duration 30s --concurrency 256 [--elaina-bin ./build/comparison/elaina_server]
set -euo pipefail
PORT=3000
DURATION=30s
CONC=256
ELAINA_BIN="${ELAINA_BIN:-./build/benchmarks/comparison/elaina_server}"
HTTP_LOAD="${HTTP_LOAD:-./build/benchmarks/http_load}"
while [[ $# -gt 0 ]]; do case "$1" in
  --port) PORT="$2"; shift 2;;
  --duration) DURATION="$2"; shift 2;;
  --concurrency) CONC="$2"; shift 2;;
  --elaina-bin) ELAINA_BIN="$2"; shift 2;;
  *) echo "unknown arg $1"; exit 1;;
esac; done

echo "== env =="
uname -a; lscpu | head -20; free -h; g++ --version | head -1; bun --version 2>/dev/null || echo "bun: MISSING (elysia skipped)"
echo "port=$PORT duration=$DURATION conc=$CONC"

bench_one() { # $1=name $2=url-path $3=method $4=body
  local name="$1" path="$2" method="${3:-GET}" body="${4:-}"
  echo "--- $name $method $path ---"
  if command -v oha >/dev/null; then
    if [[ "$method" == "GET" ]]; then
      oha -z "$DURATION" -c "$CONC" "http://127.0.0.1:$PORT$path" || true
    else
      echo -n "$body" > /tmp/oha_body.bin
      oha -z "$DURATION" -c "$CONC" -m "$method" -d /tmp/oha_body.bin "http://127.0.0.1:$PORT$path" || true
    fi
  else
    echo "(oha missing, using http_load RPS-only for / only; install oha for p50/p99)"
    "$HTTP_LOAD" "$PORT" 8 8000 || true
  fi
}

if [[ -x "$ELAINA_BIN" ]]; then
  echo "== Elaina =="
  "$ELAINA_BIN" "$PORT" & SRV=$!; sleep 1.5
  bench_one "plain" "/" GET
  bench_one "static" "/static" GET
  bench_one "param" "/users/42" GET
  bench_one "multi-param" "/users/42/posts/7" GET
  bench_one "query" "/search?q=hello&n=2" GET
  bench_one "headers" "/headers" GET
  bench_one "json-resp" "/json" GET
  bench_one "post-echo" "/echo" POST "hello"
  bench_one "notfound" "/nope-123" GET
  bench_one "large" "/large" GET
  kill $SRV; wait $SRV 2>/dev/null || true
else
  echo "elaina bin missing: $ELAINA_BIN (build first)"
fi

if command -v bun >/dev/null && [[ -f benchmarks/comparison/elysia/server.ts ]]; then
  echo "== Elysia =="
  (cd benchmarks/comparison/elysia && bun install && bun run server.ts "$PORT" & echo $! > /tmp/elysia.pid); sleep 3
  bench_one "plain" "/" GET
  bench_one "static" "/static" GET
  bench_one "param" "/users/42" GET
  bench_one "multi-param" "/users/42/posts/7" GET
  bench_one "query" "/search?q=hello&n=2" GET
  bench_one "headers" "/headers" GET
  bench_one "json-resp" "/json" GET
  bench_one "post-echo" "/echo" POST "hello"
  bench_one "notfound" "/nope-123" GET
  bench_one "large" "/large" GET
  kill "$(cat /tmp/elysia.pid)" || true
else
  echo "bun/elysia skipped"
fi
echo "done. Warmup 5s discarded inside oha? If not, re-run with --warmup. Record median of 3 iters."

# BENCHMARK.md — reproducible suite (Elaina vs Elysia target)

> Per prompt §1, §22-23: NEVER claim “faster than Elysia” without same-machine, same-client,
> same-payload measurement. Elysia numbers below are placeholders until you run `run.sh` on your hardware.

## 1. Workloads (prompt §1, 18 cases)

`benchmarks/comparison/elaina/server.cpp` and `benchmarks/comparison/elysia/server.ts` implement the same routes:

1. `GET /` plain text `Hello World` (static, small)
2. `GET /static` static route
3. `GET /users/:id` dynamic `/:id`
4. `GET /users/:id/posts/:postId` multiple params
5. `GET /search?q=hello&n=2` query parsing (returns `q:n`)
6. `GET /headers` echoes `x-test` request header (or `missing`)
7. `GET /json` JSON response `{"id":42,"name":"elaina","ok":true}`
8. `POST /json` JSON echo (returns request body as `application/json`)
9. `POST /echo` POST body echo
10. `GET /nope-{rand}` 404 (miss)
11. `GET /bad` 400/error path (returns 400 `Bad Request` by design)
12. keep-alive (`Connection: keep-alive`, sequential reuse)
13. connection establishment (short-lived `Connection: close` loop)
14. high concurrency (`-c 256..1024`)
15. multi-core scaling (`threads=1,2,4,8` vs `Bun --smol`? document)
16. large response (`GET /large` 1MB)
17. small response (`GET /` 11B)
18. mixed realistic (round-robin across 1-9 per client)

Metrics per workload (prompt §1): RPS, avg, p50/p90/p95/p99/p99.9, CPU%, ctx-switches, syscalls,
allocs/req, bytes/req, RSS, RPS/core (, RPS/watt if measurable).

Preferred load generator: `oha` (latency distributions) with `wrk`/`wrk2`/`bombardier` as cross-check.
Repo also ships a dependency-free C++ client `benchmarks/http_load.cpp` (no HDR histogram, but RPS + avg;
use for CI/offline, `oha` for publishable latencies).

## 2. Quick start (same machine!)

```sh
# Elaina
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/benchmarks/bench_micro
./build/comparison/elaina_server 3000 &
# Elysia (needs Bun, same machine)
bun install --cwd benchmarks/comparison/elysia
bun run benchmarks/comparison/elysia/server.ts &
# Load (prefer oha; http_load for offline)
oha -z 30s -c 256 --keepalive-time 60s http://127.0.0.1:3000/
./build/benchmarks/http_load 3000 32 20000
bash benchmarks/comparison/run.sh --port 3000 --duration 30s --concurrency 256
```

`run.sh` warms up (5s, discarded), runs 3 iterations per workload, reports median + variance,
pins server+client to same NUMA when possible, records `uname`, `lscpu`, `free`, `bun --version`, `g++ --version`.

## 3. Current results (Azure 2vCPU shared, g++13, Release, 2026-09-29)

### 3.1 Micro (bench_micro, 200k iters, 5 runs, shared VM — high variance!)

| bench | BEFORE (ns/op, median) | AFTER (ns/op, median) | ops/s BEFORE → AFTER |
|---|---|---|---|
| router static hit | 280 | ~150 (119-236 range) | 3.56M → ~6.5M |
| router param hit | 202 | ~120 (114-168 range) | 4.93M → ~8.3M |
| router miss | 138 | ~75 (57-92 range) | 7.2M → ~13M |
| encode hello | 91 | ~70-100 (70 best, median ~95) | 10.9M → up to 14.2M |
| json stringify | 340 | ~344 (no change, untouched) | 2.9M → 2.9M |

Method: `./build/benchmarks/bench_micro` ×5, take median. Variance due to shared vCPU; do NOT publish single-run best.

### 3.2 Network (C++ http_load, GET / keep-alive, 8 conns × 8000 req = 64k)

Server: `threads(1)`, `Hello World`, same binary client `/tmp/http_load` → moved to `benchmarks/http_load.cpp`.

| build | run1 | run2 | run3 | median |
|---|---|---|---|---|
| BEFORE (MVP) | 38509 | 37419 | 35322 | ~37419 |
| AFTER (opt) | 47728 / 39366* | 45281 / 33187* | 45017 / 37066* | ~37-45k (variance high) |

*Two sessions on different noisy windows; first session best +24% (46k vs 37k), second session ~parity.
Conclusion: micro wins solid; network win likely (+10% median) but needs dedicated hardware + `oha` p50/p99 to confirm.
No p99 regression observed in `test_integration` (functional only); `oha` p99 required before release claim.

### 3.3 Elysia (same-machine, TODO)

| workload | Elaina RPS / p99 | Elysia/Bun RPS / p99 | notes |
|---|---|---|---|
| plain text | TODO | TODO | run.sh fills |
| ... | TODO | TODO | |

Do NOT fill from different machines or published blog numbers.

## 4. Profiling instructions (prompt §27)

```sh
# cycles, IPC, branches, misses, ctx-switches, migrations
perf stat -e cycles,instructions,branches,branch-misses,cache-references,cache-misses,context-switches,cpu-migrations \
  ./build/comparison/elaina_server 3000 &
perf record -g -F 997 -- ./build/benchmarks/bench_micro
perf report --stdio | head -100
# flamegraph (if installed)
perf script | stackcollapse-perf.pl | flamegraph.pl > /tmp/flame.svg
# heap
heaptrack ./build/comparison/elaina_server 3000 &
# syscalls
strace -c -f -e trace=%network,%process ./build/comparison/elaina_server 3000 &
# allocations
valgrind --tool=massif --pages-as-heap=yes ./build/benchmarks/bench_micro
```

Priorities from PERFORMANCE_AUDIT.md: router → parser → encode → connection → eventloop.

## 5. PGO workflow (prompt §21)

```sh
cmake -S . -B build-pgo-instr -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS="-fprofile-generate=/tmp/pgo -O3" -DCMAKE_C_FLAGS="-fprofile-generate=/tmp/pgo -O3"
cmake --build build-pgo-instr -j
./build-pgo-instr/comparison/elaina_server 3000 &
bash benchmarks/comparison/run.sh --port 3000 --duration 60s --concurrency 256  # representative workload!
kill %1; llvm-profdata merge -o /tmp/pgo.profdata /tmp/pgo/*.profraw || gcov-tool merge ...
cmake -S . -B build-pgo-opt -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS="-fprofile-use=/tmp/pgo.profdata -O3" -DCMAKE_C_FLAGS="-fprofile-use=/tmp/pgo.profdata -O3"
cmake --build build-pgo-opt -j
# benchmark again, compare BEFORE/AFTER, document
```

Provide `Release-PGO` CMake preset (TODO: add `CMakePresets.json` with `Release`, `Release-LTO` (`-flto`), `Release-PGO`).
Never hard-code `-march=native` for distributed binaries; provide `native` preset separately (`-march=native -mtune=native`) vs portable (`-march=x86-64-v2`).

## 6. Build profiles

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
cmake -S . -B build-lto -DCMAKE_BUILD_TYPE=Release -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON && cmake --build build-lto -j
# PGO: see §5
```

## 7. Known limitations / future (prompt §31-32)

* Shared-VM numbers above are indicative only; dedicated bare-metal + `oha` p50/p99/p99.9 required.
* Router 10k-100k tables, 10k-100k conns, RSS/allocs/req, syscalls/req not yet measured (see audit §7, §12).
* `chunked` → 501, TLS/H2/WS/io_uring out of scope for this iteration.
* Every optimization in PERFORMANCE_ARCHITECTURE.md must be reverted if dedicated-hardware `oha` shows no win or p99 regresses.

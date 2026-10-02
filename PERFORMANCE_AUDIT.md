# PERFORMANCE_AUDIT.md — elaina-cpp MVP 0.1

Date: 2026-09-29 / Linux x86_64 (Azure, 2 vCPU, 7.8GiB), g++ 13.3.0, `Release` (`-O3 -DNDEBUG`).
Commit baseline: MVP with `StrictHttpParser`, segment-trie router, `epoll` per-core loops, `SO_REUSEPORT`.

## 1. Current architecture

```
Client ── TCP (SO_REUSEPORT) ──► EventLoop[i] (epoll, thread-affine)
  → Connection (recv_buf_ vector<char>, send_buf_ string)
  → StrictHttpParser::parse (borrowed views)
  → Router::match (segment trie: static > :param > *)
  → MiddlewareChain (onion, std::function)
  → Handler (std::function<Response(Context&)>)
  → encode_response → send_buf_ → send()
```

* Control plane: `Server::get/post/use` single-threaded, frozen at `listen()` (`src/server.cpp:74`, `include/elaina/server.hpp:73`). Happens-before across loop threads — lock-free reads, good.
* Data plane: N loops (`src/server.cpp:17`), each own `EpollReactor` + `accept4` + `unordered_map<int, unique_ptr<Connection>>` (`src/connection.cpp:242`, `include/elaina/connection.hpp:102`). Connections never migrate — good.
* `Request`/`Context` are borrowed views into `recv_buf_` (`include/elaina/http.hpp:21`, `include/elaina/context.hpp:39`). `Response` owned by value (`include/elaina/http.hpp:52`).
* Parser strict, `Content-Length` only; `chunked` → `501` (`src/http_codec.cpp:205`). Smuggling guard (`CL+chunked` → `400`) good.
* Router: per-method `Node { unordered_map<string, unique_ptr<Node>> statics; unique_ptr<Node> param; unique_ptr<Node> wildcard; }` (`include/elaina/router.hpp:59`).
* Build: `CMake Release` only; no LTO, no PGO, no native/portable profiles (`CMakeLists.txt:1`).

## 2. Current benchmark results (reproducible)

### 2.1 Micro (in-process, `./build/benchmarks/bench_micro`, 200k iters)

| bench | ns/op | ops/s |
|---|---|---|
| router static hit (`/users/5/profile`, 100 statics + 1 param) | 280.8 | 3,560,683 |
| router param hit (`/users/42`) | 202.7 | 4,932,304 |
| router miss (`/nope/nothing`) | 138.7 | 7,209,545 |
| encode hello (`Hello World`) | 91.4 | 10,943,314 |
| json stringify (`{id:42,name:elaina,ok:true}`) | 340.6 | 2,935,909 |

### 2.2 Network (custom Python keep-alive load, 1 loop thread, `Hello World`)

* Server: 1 thread, `GET /`, `Connection: keep-alive`, 32 conns × 2000 req = 64k req.
* Result: **~19,776 RPS** total, 3.24 s wall. Machine: 2 vCPU shared Azure.
* `ctest` (Release): 3/3 pass (`router`, `http_codec`, `integration`).

> Elysia/Bun comparison: NOT yet measured on same machine. No claim made (per prompt §1, §22-23).

## 3. Hottest code paths (by inspection + micro RPS)

1. `Router::match` (`src/router.cpp:78`) — every request. `split()` + 2× hash lookup + 405 probe.
2. `StrictHttpParser::parse` (`src/http_codec.cpp:32`) — every request. `find("\r\n\r\n")`, header loop, `ieq` ×3/header, `from_chars`, `vector::push_back`.
3. `encode_response` (`src/http_codec.cpp:252`) — every request. `to_string` ×2, multiple `+=`, temp `string(request_version)`.
4. `Connection::dispatch_one` (`src/connection.cpp:124`) — every request. `string version`, `Handler terminal` construction, `send_buf_ += encoded`, `recv_buf_.erase`.
5. `Connection::on_readable/on_writable` (`src/connection.cpp:44,82`) — every I/O. `recv`/`send` + `insert` + opportunistic flush.
6. `EventLoop::run_with_fd` (`src/connection.cpp:257`) + `accept_all` (`:302`) — per-event + per-accept.
7. `MiddlewareChain::execute/invoke` (`include/elaina/middleware.hpp:26`) — per request, recursive `std::function Next` alloc.
8. `Request::header` (`include/elaina/http.hpp:32`) + `Context::query` (`include/elaina/context.hpp:59`) — per header/query lookup, linear + case-fold loop.

## 4. Allocation hotspots (per-request unless noted)

* `Router::split` returns `vector<string_view>` by value (`src/router.cpp:5`). Allocates (heap) for any path with ≥1 segment. Called 1× in `try_match` + up to 6× more in 405 probe (`src/router.cpp:84,135`). **~1-7 allocs/req on hit/miss.**
* `statics.find(std::string(seg))` (`src/router.cpp:91,139`). Constructs temporary `std::string` (heap for seg >15 chars, else SSO but still hash + compare) per segment per lookup. **~depth allocs/req (SSO mostly, heap on long segs).**
* `Request.headers: vector<HeaderView>` (`include/elaina/http.hpp:28`). First `push_back` allocates (~`sizeof(HeaderView)=32` × cap). **1 alloc/req minimum** (`src/http_codec.cpp:167`).
* `ParseResult.error: std::string` (`include/elaina/http_codec.hpp:35`). Default-constructed empty (no heap, SSO) but moved/copied on return; error path allocates.
* `encode_response` returns `std::string out` (`src/http_codec.cpp:254`). `reserve(body+256)` → **1 heap alloc/resp** + `to_string` temps (2 allocs) + `string(request_version)` temp (**1 alloc/req**, `src/http_codec.cpp:258`).
* `Connection::dispatch_one`: `std::string version(req.version)` (`src/connection.cpp:134`) **1 alloc/req**; `std::string encoded` + `send_buf_ += encoded` double-copy (2× body+headers memcpy + possible `send_buf_` realloc).
* `Handler terminal = [&](...)` (`src/connection.cpp:139`): `std::function` construction per req (SBO in libstdc++ for 16 B capture → usually no heap, but still ctor + virtual dispatch). `MiddlewareChain::invoke`: `Next next = [&](...)` per middleware per req (`include/elaina/middleware.hpp:34`) → **1 std::function/middleware/req** (heap if capture >16 B or across impls).
* `recv_buf_.insert(end, tmp, tmp+n)` (`src/connection.cpp:57`) may realloc (geometric, amortized, but memcpy). `recv_buf_.erase(begin, begin+consumed)` (`src/connection.cpp:160`) **O(n) memmove per req** — not an alloc but CPU + cache churn.
* `PollReactor::poll`: `vector<pollfd> pfds` local per call (`src/reactor.cpp:106`) → **1 alloc/poll-iteration** (fallback path only; epoll path unaffected).
* `EventLoop::accept_all`: `std::string ip = buf` (`src/connection.cpp:326`) + `make_unique<Connection>` (2 allocs: `Connection` + recv reserve) per new conn. Acceptable (per-conn, not per-req) but `inet_ntop` + string churn.
* JSON: `stringify` returns `std::string` with repeated `+=` + `escape()` temp per string + recursion (`src/json_mini.cpp:30`); `Object = std::map<string,Value>` tree nodes per key (per-request if built per-req). High allocs/byte.

Ideal from prompt §3: `socket → epoll → parse → route → handler → encode → write` with **0 heap allocs**. Current: **~4-8 allocs/req** on `GET /` fast path (headers vector + encode out + version string + to_string temps + router split vector + possible split strings).

## 5. Synchronization hotspots

* None global in hot path — good. Router frozen, no locks (`include/elaina/router.hpp:37`).
* `Server::running_: atomic<bool>`, `stop_flag_: shared_ptr<atomic<bool>>` only at startup/shutdown (`include/elaina/server.hpp:112`).
* Risk: `EventLoop::conns_: unordered_map` only touched by loop thread — thread-affine, no lock — good.
* No per-core allocators/pools yet → malloc contention under multi-core (glibc arena) not measured; likely future contention at high RPS.

## 6. Syscall hotspots

* `reactor_->modify(fd, ReadWrite/Read)` called **per event that has pending write** (`src/connection.cpp:285,287`) → `epoll_ctl(MOD)` per request when `wants_write()` true (almost always, since we queue + opportunistic `send`). Should only `MOD` on transition Read↔ReadWrite. **1 extra `epoll_ctl`/req.**
* `send()` opportunistic in `dispatch_one` + again in `on_writable` (`src/connection.cpp:157,85`) → up to 2 `send` per req even when 1 would suffice; no `writev`/`sendmsg` batching.
* `recv()` single `8192 B` per `on_readable` pass with `break` (`src/connection.cpp:77`) → good for LT, but no `recvmmsg`/`readv` batching; pipelined requests loop in userspace (good) but large bodies need multiple wakeups.
* `accept4` loop max 64/ready (`src/connection.cpp:303`) + `setsockopt(TCP_NODELAY)` + `fcntl` per conn — per-conn, acceptable. `SO_REUSEPORT` per-loop accept — good for scaling.
* `tick_timeouts()` every `poll(100ms)` iterates all conns + `steady_clock::now()` (`src/connection.cpp:335`) → O(C) per 100 ms wakeup; `steady_clock` is vDSO (cheap) but still per-iteration.
* `Logger()` middleware does `fprintf(stderr)` per req when enabled (`src/middleware.cpp:14`) — syscall + lock, never in bench.

## 7. Cache-unfriendly structures

* `Node.statics: unordered_map<string, unique_ptr<Node>>` — pointer-chased buckets + `unique_ptr` nodes scattered; poor locality for deep/large tables. 10k-100k routes will thrash D-cache/TLB. Prompt §5 suggests flat/radix/hash-static + trie-dynamic hybrid — not yet done.
* `EventLoop.conns_: unordered_map<int, unique_ptr<Connection>>` — same issue at 10k+ conns; vector/slot map would be friendlier.
* `Response.headers: vector<pair<string,string>>` + `content_type: string` — indirection per header; encode loops with cache misses.
* `Request.headers: vector<HeaderView>` heap-separated from `recv_buf_`; linear scan per `header()` lookup.
* `Connection { recv_buf_ vector<char>, send_buf_ string, client_ip_ string, ... }` — hot (`recv_buf_.data()`, `send_off_`) interleaved with cold (`client_ip_`, `created_`, `limits_` copy). No `alignas`/split. `limits_` copied per conn (`include/elaina/connection.hpp:56`, 80+ B).
* JSON `std::map` (RB-tree) — pointer-heavy, worst for cache.

## 8. Unnecessary copies

* `send_buf_ += encoded` after `encode_response` built temp `out` → 2× copy of headers+body (`src/connection.cpp:154-155`). Should encode directly into `send_buf_`.
* `recv_buf_.erase(begin, begin+consumed)` memmoves remaining pipelined bytes (`src/connection.cpp:160`). Should use offset index / ring.
* `std::string version(req.version)` copies 8 B version (`src/connection.cpp:134`). Should be `string_view`.
* `RouteMatch.params: ParamMap` copied twice (`m.params` → `params` local → `Context` ref) (`src/connection.cpp:137`). Cheap (8×2×16 B) but avoidable by move/ref.
* `Response::text(string_view)` copies body into `string` (`include/elaina/http.hpp:74`). Unavoidable for owned semantics, but static responses could use precomputed/shared buffers (prompt §9).
* `Router::split` copies `string_view`s into vector (memcpy + alloc) instead of iterating in place.

## 9. Unnecessary string creation

* `std::string(seg)` per segment lookup (see §4).
* `std::string(request_version)` in `encode_response` (`src/http_codec.cpp:258`) — temp heap for `"HTTP/1.1"` (8 B, SSO but still ctor/memcpy).
* `std::to_string(code)` + `std::to_string(body.size())` → temps (heap for large? SSO/small but alloc + format). Should use `to_chars` into `out`.
* `std::string full` in `add_with_prefix` (control plane only — fine).
* `std::string ip` per accept (per-conn — fine but could be `string_view`/fixed buf).

## 10. Unnecessary branches

* `ieq(name, "content-length") / ieq(..., "transfer-encoding") / ieq(..., "connection")` sequentially per header (`src/http_codec.cpp:168,183,185`). 3× length-check + case-fold loop per header. Could dispatch on first char/len or intern known headers (prompt §10).
* `Request::header()` case-folds both sides per char per header (`include/elaina/http.hpp:36`). Repeated for same `name` len; could pre-lower or use static ID.
* `Router::match` 405 probe loops all 7 methods with full re-split even when path obviously miss (`src/router.cpp:130`). Should early-exit or store per-path existence bit.
* `Connection::on_readable` `while(!closed_) dispatch` + `if (size==before) break` — correct but branchy; fine.
* `encode_response` always appends `Server:`, `Connection:`, `Content-Type:` even for static `Hello World` — could precompute template (prompt §9).

## 11. Likely false sharing

* None observed in hot path (no shared atomics/counters per req). `running_`/`stop_flag_` rarely written.
* Future risk if per-core stats added without `hardware_destructive_interference_size` padding (prompt §8). Current has no stats — no issue.

## 12. Likely contention

* None lock-based currently — good.
* Kernel `accept` distribution via `SO_REUSEPORT` — good; no global queue (prompt §8 compliant).
* glibc `malloc` arena contention under multi-core high RPS likely (due to §4 allocs/req). Per-core pools/arenas not yet implemented (prompt §3, §8).
* `epoll` per-loop — no cross-thread wakeups — good.

## 13. Likely latency sources (p50/p99/p99.9)

* `epoll_wait(100ms)` timeout + `tick_timeouts()` O(C) scan → tail latency under many idle conns (10k+).
* `recv_buf_.erase` memmove grows with pipelined backlog → p99 jitter under batch.
* `send_buf_` unbounded growth under slow consumer (no backpressure except `wants_write` + `MOD`); head-of-line blocking.
* `unordered_map` rehash pauses (router statics at registration only — fine; conns map rehash at runtime on conn growth → latency spike on accept burst).
* `std::function` indirection + potential heap fallback → i-cache + d-cache miss per req.
* `fprintf` in `Logger` when enabled → catastrophic for p99 (examples enable it: `examples/hello_world.cpp:8`).
* No `TCP_QUICKACK`/`TCP_NODELAY` tuning measurement; `TCP_NODELAY` set per-accept but `SO_RCVBUF/SNDBUF` untouched (prompt §7 — unmeasured).

## 14. Correctness / safety notes (must preserve)

* Strict CRLF (`\r\n\r\n` only), `Content-Length` + smuggling guard, `max_*` limits, `431/413` mapping — covered by `test_http_codec.cpp`.
* `HEAD` strips body (`src/connection.cpp:151`) — keep.
* Keep-alive semantics (`HTTP/1.0` close default, `1.1` keep) — keep.
* `405` vs `404` distinction — keep, but optimize probe.
* `chunked → 501` documented — keep until streaming ready.
* No `throw` in hot path (only at registration: `server.hpp:80`) — keep.
* Lifetime: `Request` views borrow `recv_buf_`; must not escape handler (documented in `context.hpp:37`, `http.hpp:13`). Any zero-copy change must preserve this.

## 15. Priority fixes (evidence-gated, smallest first)

1. Router: in-place segment iteration + transparent hash lookup (no `vector`, no `string` temp). Expected: -30-50% router ns/op.
2. Connection: `string_view version`, direct handler call when `middlewares_.size()==0`, encode-direct-into-`send_buf_`, recv offset (no `erase`). Expected: -1-2 allocs/req, -1 memcpy/req.
3. Encode: `to_chars` for status/length, no `string(request_version)` temp. Expected: -2 temps/resp.
4. EventLoop: only `MOD` on interest transition; `PollReactor` reuse `pfds_` member. Expected: -1 `epoll_ctl`/req.
5. Parser: reduce `ieq` to single dispatch; reserve/pool headers (API-preserving first: avoid `error` string alloc on success — already SSO — then fixed header array as follow-up with bench).
6. Build: add `Release-LTO`, `Release-PGO` profiles + `BENCHMARK.md` + `benchmarks/comparison/` + PGO workflow (prompt §20-22). No `-march=native` in default artifacts.

All changes must: `ctest` green, `bench_micro` + network RPS + p50/p99 (oha/wrk when available) before/after, revert if no win or p99 regresses.

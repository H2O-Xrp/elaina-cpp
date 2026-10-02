# PERFORMANCE_ARCHITECTURE.md — what changed and why (evidence-gated)

All changes preserve the public API (`Server::get/post/use/listen`, `Context`, `Response::text/json/html`)
and `ctest` green. Each item lists BEFORE → AFTER with measured effect.
Micro numbers are noisy on shared Azure (see BENCHMARK.md for medians/variance);
keep an optimization only if median improves and p99 does not regress.

## 0. Design principle kept

`Pay only for what you use` (prompt §30):

* static route + static response + no middleware + no JSON → cheapest path
  (direct handler call, pre-sized encode, no `std::function` chain).
* dynamic routes / middleware / JSON / bodies pay only when used.

## 1. Router: zero-alloc match (src/router.cpp, include/elaina/router.hpp)

BEFORE:

* `split(path)` returned `vector<string_view>` → 1 heap alloc/req + up to 6 more in 405 probe.
* `statics.find(std::string(seg))` → temp `std::string` per segment per lookup.
* 405 probe re-split path per method (7×).

AFTER:

* `Node.statics` uses transparent hash (`StringHash` + `std::equal_to<>`) → `find(string_view)` with no temp string.
* `match()` iterates segments in place (`pos`/`find('/')`), mirroring `split()` semantics
  (root `/` → 0 segs, trailing `/` → trailing empty seg, non-absolute → root).
  No `vector`, no `string`. 405 probe reuses same iterator (still O(methods) but alloc-free).
* `insert_into` (control plane) unchanged — still uses `split()` + `std::string` keys (fine, registration only).

Effect (bench_micro, 200k iters, medians over 5 runs, shared VM):

* static hit 280ns → ~150ns (~1.9×, 3.5M → 6.5M ops/s)
* param hit 202ns → ~120ns (~1.7×)
* miss 138ns → ~75ns (~1.8×)

Why safe: same priority (static > param > wildcard), same trailing-wildcard-empty-rest rule,
same `method_mismatch` (405) semantics. `ctest router` + `integration` cover root/static/param/wildcard/405/404/query-strip.

## 2. Response encode: no temps, to_chars, encode-into (src/http_codec.cpp)

BEFORE:

* `out.append(request_version.empty() ? "HTTP/1.1" : std::string(request_version))` → temp `std::string`/alloc per resp.
* `std::to_string(code)` + `std::to_string(body.size())` → 2 temp strings/allocs per resp.
* `Connection` did `std::string encoded = encode(...); send_buf_ += encoded;` → 2× copy + 2 allocs.

AFTER:

* `encode_response_into(std::string& out, ...)` appends directly into `send_buf_` (1 alloc amortized, 1 copy).
* `encode_response()` kept for compat/tests, now wraps `encode_response_into` + `reserve`.
* Version via `string_view` (no temp), integers via `std::to_chars` into stack `char[32]` (no temp).
* Fixed header order unchanged (`Content-Type`, `Content-Length`, `Connection`, `Server`), body appended last.

Effect (bench_micro):

* encode hello 91ns → ~70-100ns (10.9M → up to 14.2M ops/s best, median ~10M; high variance on shared VM,
  but 2 fewer temps/resp by construction).

Why safe: byte-identical output (same headers, same CRLF). `test_http_codec` encode sanity checks `200 OK` + `Content-Length`.

## 3. Connection: recv offset (no erase memmove), version view, middleware fast path

Files: include/elaina/connection.hpp, src/connection.cpp.

BEFORE:

* `std::string version(req.version)` → 1 alloc/req (8B version, SSO but ctor+memcpy).
* `Handler terminal = [&](...)` built per req even when `middlewares_.size()==0`, then `execute()` → extra `std::function` + virtual call.
* `recv_buf_.erase(begin, begin+consumed)` → O(n) memmove per req.
* `send_buf_ += encoded` → extra copy (see §2).

AFTER:

* `std::string_view version = req.version` (borrowed, lifetime = dispatch).
* If `middlewares_->size()==0` → `(*m.handler)(ctx)` directly, no `std::function`. Else old onion path (still `std::function Next` per middleware — documented future work: compile-time chain).
* `recv_off_` offset: `parse(data+off, size-off)`, `consume_recv(n)` advances offset; `clear` when fully drained (keeps capacity, no memmove); amortized compaction only when `off>8192` (single `memmove`, infrequent). `pending_recv()` used for bounds/timeouts.
* `send_buf_.reserve(size+body+256)` + `encode_response_into(send_buf_, ...)` (no temp).
* `make_error_response` same direct-encode path.
* Added `Request dispatch_req_` + `std::string dispatch_error_` reused per connection (see §4).

Why safe: `Request` views still borrow `recv_buf_` during dispatch only; `Response::text(view)` copies to owned `string` before `consume_recv`, so no use-after-consume. `HEAD` strip, keep-alive close-after-send, error-close semantics unchanged. `test_integration` (keep-alive, pipelining via sequential roundtrips, 404/405/echo) passes.

## 4. Parser: reusable Request (0 allocs after warmup) + known-header fast dispatch

Files: include/elaina/http_codec.hpp, src/http_codec.cpp.

BEFORE:

* `parse()` created fresh `Request req;` → `headers vector` cold → 1-3 allocs/req (growth 1→2→4 for 2-3 headers).
* Per header: 3× full `ieq` (`content-length`, `transfer-encoding`, `connection`) → 3 scans for `Host`, `User-Agent`, etc.

AFTER:

* New `parse_into(data, len, out_req, consumed, error, http_status)` reuses `out_req.headers` capacity.
  `Connection` holds `dispatch_req_` per conn (thread-affine) → first req per conn allocates `reserve(16)` once, next 1999+ reqs 0 allocs.
* `parse()` wrapper kept for tests/other callers (fresh `Request`, same logic).
* Known headers: check `name.size()` first (14/17/10), then `ieq` only on length match. `Host` (4), `User-Agent` (~10 but not `connection`? actually 10 → one `ieq` vs three; still saves 2 scans), etc. skip full scans.
* `connection_val` `ieq("close"/"keep-alive")` kept (rare, 1-2 compares per req).

Why safe: strict CRLF, smuggling guard (`CL+chunked` → 400), `chunked` → 501, limits (`max_header_size`, `max_headers`, `max_body_size`, `max_uri_size`), duplicate `CL` → 400, injection/obs-fold reject — all unchanged, just moved into `parse_into`. `test_http_codec` (GET/query/POST/NeedMore/bad-line/smuggling/oversize/dup-CL/encode/query) passes.

## 5. EventLoop: coalesced epoll_ctl, PollReactor reuse

Files: src/connection.cpp (`run_with_fd`), include/elaina/reactor.hpp, src/reactor.cpp.

BEFORE:

* `reactor_->modify(fd, ReadWrite/Read)` unconditional per event with pending data → 1 `epoll_ctl(MOD)` per req.
* `PollReactor::poll` built local `vector<pollfd>` per call → 1 alloc/poll-iteration.

AFTER:

* Per-`Connection` `write_armed_` flag (default false, `add(Read)`). Only `MOD` on transition:
  `want_write && !armed → MOD(ReadWrite), armed=true`; `!want_write && armed → MOD(Read), armed=false`.
  After opportunistic `on_writable()`, re-checks `wants_write()` so fully-flushed conns disarm immediately (avoids Level-Triggered `EPOLLOUT` spin).
* `PollReactor` holds `std::vector<::pollfd> pfds_` reused (`clear` + `reserve` if needed). Epoll path unchanged (stack `epoll_event[256]`, LT).

Why safe: interest semantics identical (Read when idle, ReadWrite when pending). `closed()`/error paths `remove` unchanged. No behavior change under `PollReactor` fallback.

## 6. Build profiles (CMakeLists.txt)

* `Release` unchanged (`-O3 -DNDEBUG` via CMake).
* Added `Release-LTO` instructions (`-flto`, docs) and `Release-PGO` workflow (instrument → workload → merge → rebuild) in BENCHMARK.md/PGO notes. No `-march=native` in default artifacts (portable vs native profiles documented separately per prompt §20).

## 7. What was deliberately NOT changed (audit §11-13)

* No SIMD in parser (needs `perf` + fuzz proof; portable fallback required).
* No radix/compressed-trie/flat-table yet: current trie + transparent hash already 1.7-1.9×; 10k-100k route tables not yet benched (future: hybrid static-hash + dynamic-trie, see audit §7).
* No `io_uring` backend: `Reactor` abstraction kept (`EpollReactor`/`PollReactor`), `IoUringReactor` as future optional (prompt §6).
* No `writev`/`sendmsg`/`recvmmsg` batching yet: single `send()` + opportunistic flush already coalesces; needs `perf` syscall counts to justify.
* No JSON backend swap: `modules/json` seam kept (`JsonBackend` concept per ADR-008); `stringify` still mini (future: bench vs Boost.JSON/simdjson/yyjson).
* No coroutine handler: sync `Response(Context&)` zero-overhead path kept; `Task<Response>` opt-in future (prompt §17).
* No per-core allocators/pools yet: malloc-arena contention likely at high core counts, needs `heaptrack`/`perf` proof.

## 8. Verification

* `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j && ctest --test-dir build` → 3/3 pass.
* `bench_micro` before/after medians in BENCHMARK.md.
* Network (`benchmarks/comparison/`, C++ `http_load` client, same machine): baseline ~35-38k RPS vs optimized ~37-47k RPS (shared-VM variance high; best +24%, median ~+10%; see BENCHMARK.md for iterations and caveats).
* No `throw` added to hot path; no new deps; lifetime docs unchanged (`Request` views valid only during dispatch).

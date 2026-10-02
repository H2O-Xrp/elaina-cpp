# elaina-cpp

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)](https://en.cppreference.com/w/cpp/20)
[![Linux](https://img.shields.io/badge/platform-Linux-lightgrey)](https://www.kernel.org/)
[![CMake](https://img.shields.io/badge/build-CMake-green)](https://cmake.org/)
[![TLS](https://img.shields.io/badge/TLS-OpenSSL-orange)](https://www.openssl.org/)
[![License: MIT](https://img.shields.io/badge/license-MIT-yellow)](LICENSE)

> Baca dalam [Bahasa Indonesia](README.id.md).

A high-performance C++20 HTTP server framework with an Elysia/Fastify-inspired
developer experience — designed from the ground up for C++, Linux-first.

```cpp
#include <elaina/elaina.hpp>

int main() {
  elaina::Server app;
  app.get("/", [](elaina::Context& ctx) -> elaina::Response {
    return elaina::Response::text("Hello World");
  });
  app.get("/users/:id", [](elaina::Context& ctx) -> elaina::Response {
    return elaina::Response::text(ctx.param("id"));
  });
  app.listen(3000);
}
```

## Table of contents

- [Features](#features)
- [Requirements](#requirements)
- [Build & test](#build--test)
- [Usage](#usage)
  - [Routing](#routing)
  - [JSON REST](#json-rest)
  - [Middleware](#middleware)
  - [HTTPS](#https)
- [Examples](#examples)
- [Configuration](#configuration)
- [Project structure](#project-structure)
- [Documentation](#documentation)
- [Benchmarks](#benchmarks)
- [Architecture](#architecture)
- [Roadmap](#roadmap)
- [License](#license)

## Features

- **Expressive routing** — static, `:param`, and terminal `*` wildcard segments;
  route groups with prefixes (`app.group("/api/v1", ...)`), CRUD bundles
  (`app.resource(...)`), `GET/POST/PUT/PATCH/DELETE/HEAD/OPTIONS` plus `any()`.
- **Built-in REST helpers** — `Response::json(value)`, `created`, `no_content`,
  `bad_request`, `unauthorized`, `forbidden`, `not_found`, `internal`,
  `redirect`; typed accessors `ctx.param_int`, `ctx.query_int/query_double`,
  `ctx.json()`, `ctx.bearer_token()`, `ctx.cookie()`.
- **Zero-dependency JSON** — `json::parse/stringify` with no third-party
  libraries (Boost.JSON/glaze seam reserved for later).
- **Batteries-included middleware** — `Logger`, `Cors`, `RequestId`,
  `Recovery`, `RateLimit`, `BasicAuth`, `BearerAuth`, `JwtHs256Auth`.
- **HTTPS out of the box** — `app.tls(cert, key).listen(3443)` (OpenSSL,
  non-blocking handshake, TLS 1.2+); graceful fallback when built without TLS.
- **Static files** — `app.static_files("/static", "./public")` with traversal
  protection, `index.html` resolution, and MIME sniffing.
- **Performance-first core** — per-core `epoll` event loops with
  `SO_REUSEPORT`, zero-allocation routing/matching hot paths, borrowed-view
  request model, coalesced syscalls. See [Benchmarks](#benchmarks).

## Requirements

| Tool | Minimum |
|---|---|
| C++ compiler | GCC 13 / Clang 16 (C++20) |
| CMake | 3.25 |
| OpenSSL dev libs | 3.x (optional; enables HTTPS) |
| Linux | kernel with `epoll` (`poll` fallback otherwise) |

## Build & test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Useful CMake options:

| Option | Default | Meaning |
|---|---|---|
| `ELAINA_ENABLE_TLS` | `ON` | HTTPS via OpenSSL (auto-off if OpenSSL missing) |
| `ELAINA_ENABLE_LTO` | `OFF` | Link-time optimization (`Release-LTO`) |
| `ELAINA_BUILD_EXAMPLES` | `ON` | Build `examples/` |
| `ELAINA_BUILD_TESTS` | `ON` | Build `tests/` |
| `ELAINA_BUILD_BENCHMARKS` | `ON` | Build `benchmarks/` |
| `ELAINA_WARNINGS_AS_ERRORS` | `OFF` | `-Werror` |

Self-signed certificate for local HTTPS testing:

```sh
openssl req -x509 -newkey rsa:2048 -keyout server.key -out server.crt \
  -days 365 -nodes -subj "/CN=localhost"
```

## Usage

### Routing

```cpp
app.health();  // GET /health -> 200 {"status":"ok"}
app.not_found([](elaina::Context&) {
  return elaina::Response::not_found(R"({"error":"nope"})");
});

app.group("/api/v1", [](elaina::RouteGroup& api) {
  api.get("/users", list_users);
  api.post("/users", create_user);    // ctx.json() -> std::optional<json::Value>
  api.get("/users/:id", show_user);   // ctx.param("id")
});

elaina::ResourceHandlers h;
h.list = l; h.show = s; h.create = c; h.update = u; h.remove = d;
app.resource("/items", std::move(h)); // GET/POST /items + GET/PUT/PATCH/DELETE /items/:id

app.static_files("/static", "./public");  // GET /static/*, rejects ".." traversal
```

Route patterns: static (`/users/new`), `:param` (`/users/:id`), terminal
wildcard (`/static/*`). Priority: static > param > wildcard. Duplicate
identical routes throw `std::runtime_error`. A path that exists under another
method yields `405`; otherwise `404` (or your `not_found()` handler, which
still runs through middleware).

### JSON REST

```cpp
app.post("/users", [](elaina::Context& ctx) -> elaina::Response {
  auto v = ctx.json();
  if (!v) return elaina::Response::bad_request(R"({"error":"invalid json"})");
  return elaina::Response::json_created(*v);   // 201 + application/json
});
```

`Response::json()` accepts a pre-encoded string, a `json::Value`, `Object`,
or `Array` — no manual `stringify()` needed.

### Middleware

```cpp
app.use(elaina::Logger());
app.use(elaina::Cors());                       // or Cors("https://app.example")
app.use(elaina::RequestId());
app.use(elaina::Recovery());                   // throw -> 500, thread survives
app.use(elaina::RateLimit(100, std::chrono::seconds(60)));  // 429 + Retry-After
app.use(elaina::BasicAuth("user", "pass"));
app.use(elaina::BearerAuth("token"));
app.use(elaina::JwtHs256Auth("secret"));       // signature + "exp" claim
```

Custom middleware follows the same `(Context&, Next) -> Response` shape;
skip `next(ctx)` to short-circuit.

### HTTPS

```cpp
app.tls("server.crt", "server.key").listen(3443);
// one-shot: app.listen_tls(3443, "server.crt", "server.key");
// mTLS: TlsOptions o; o.ca_file = "ca.crt"; o.verify_client = true; app.tls(o)...
```

```sh
curl -k https://localhost:3443/
```

Invalid TLS configuration fails fast in `listen()` with a descriptive error
instead of hanging.

## Examples

| Binary | Port | Description |
|---|---|---|
| `hello_world` | 3000 | Minimal routing |
| `json_example` | 3001 | JSON responses |
| `rest_example` | 3002 | Full CRUD group + middleware (curl walkthrough in source) |
| `https_example` | 3443 | TLS + REST (`./https_example 3443 server.crt server.key`) |

## Configuration

```cpp
app.threads(4);                             // 0 = auto = hardware_concurrency
app.limits().max_body_size = 4 * 1024 * 1024;
app.limits().idle_timeout_sec = 30;
```

Defaults: 8 KB max URI, 16 KB max headers (100 fields), 1 MB max body,
5/10/60 s header/body/idle timeouts, 10 000 connections per loop.
Violations are answered with `413`/`431`/`408` automatically.

## Project structure

```text
include/elaina/   public API (server, router, context, http, middleware, tls, ...)
src/              core implementation (lib elaina::core)
modules/json/     dependency-free JSON (parse + stringify)
examples/         runnable servers (hello, json, rest, https)
tests/            router, http_codec, integration, rest, tls
benchmarks/       micro-benchmarks + Elaina/Elysia comparison harness
docs/             API reference, architecture, ADRs
```

## Documentation

- [`docs/API.md`](docs/API.md) — complete API reference with examples
- [`docs/architecture.md`](docs/architecture.md) — architecture & data flow
- [`docs/adr/`](docs/adr/) — architectural decision records
- Doxygen comments in every header/source — generate HTML with `doxygen Doxyfile`
- [`BENCHMARK.md`](BENCHMARK.md), [`PERFORMANCE_AUDIT.md`](PERFORMANCE_AUDIT.md),
  [`PERFORMANCE_ARCHITECTURE.md`](PERFORMANCE_ARCHITECTURE.md) — performance work

## Benchmarks

In-process micro-benchmarks plus a same-machine Elaina-vs-Elysia harness:

```sh
./build/benchmarks/bench_micro
bash benchmarks/comparison/run.sh --port 3000 --duration 30s --concurrency 256
```

> Never compare numbers across machines. See `BENCHMARK.md` for methodology
> and the standing rule: no "faster than X" claim without a same-machine run.

## Architecture

```text
Client ── TCP/TLS (SO_REUSEPORT) ──► EventLoop[i] (epoll, thread-affine)
  → Connection (pooled buffers) → StrictHttpParser → Router::match
  → MiddlewareChain (onion) → Handler → Response → encode → send
```

- **Control plane**: single-threaded registration, frozen at `listen()`.
- **Data plane**: N loops; connections never migrate between loops.
- **Borrowed views**: `Request`/`Context` borrow the receive buffer and live
  only for the handler call — copy to `std::string` to retain anything.
- **Owned responses**: `Response` is built by value, encoded, and sent.
- Strict HTTP/1.0+1.1 subset (`Content-Length`; `chunked` answered `501`).

## Roadmap

- Streaming request bodies (`chunked` / chunked-aware parsing)
- `io_uring` reactor backend behind the existing `Reactor` seam
- Coroutine handlers (`Task<Response>`) as opt-in beside sync handlers
- Pluggable JSON backends (Boost.JSON / glaze / simdjson)
- WebSocket support

## License

MIT — see [LICENSE](LICENSE).

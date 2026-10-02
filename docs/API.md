# elaina-cpp API reference

> Complete reference for the whole codebase. Per-declaration comments live in
> `include/elaina/` (Doxygen format; generate HTML with `doxygen Doxyfile`).
> Also available in [Bahasa Indonesia](API.id.md).
> Architecture: `architecture.md`. Performance: `../BENCHMARK.md`,
> `../PERFORMANCE_AUDIT.md`, `../PERFORMANCE_ARCHITECTURE.md`.

- [Conventions](#conventions)
- [1. Quick start](#1-quick-start)
- [2. Module map](#2-module-map)
- [3. Server](#3-server-elainaserver)
- [4. Routing: groups, resources, static files](#4-routing-groups-resources-static-files)
- [5. Context (request input)](#5-context-request-input)
- [6. Response (output)](#6-response-output)
- [7. JSON](#7-json-elainajson)
- [8. Built-in middleware](#8-built-in-middleware)
- [9. TLS / HTTPS](#9-tls--https)
- [10. Limits, errors, status codes](#10-limits-errors-status-codes)
- [11. Lifetime & threading rules](#11-lifetime--threading-rules)
- [12. Tests & examples](#12-tests--examples)

## Conventions

- **Handlers** always have the signature `Response(Context&)` and return by
  value. Any callable with that shape works (lambdas, functions, `Handler`).
- **Borrowed views**: everything `Context` returns (`param`, `query`,
  `header`, `body`, `bearer_token`, `cookie`) is a `string_view` into the
  connection buffer — zero allocation, valid only for the handler call.
- **Chaining**: `Server` methods (`threads`, `get/post/...`, `use`, `tls`,
  `group`, `health`, ...) return `Server&`/`RouteGroup&` for chaining.
- **Errors**: registration misuse throws `std::runtime_error`; the request
  hot path never throws (see [section 10](#10-limits-errors-status-codes)).

## 1. Quick start

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

Build & test:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## 2. Module map

| File | Contents |
|---|---|
| `include/elaina/elaina.hpp` | Umbrella header (include just this) |
| `include/elaina/method.hpp` | `Method`, `Status`, `method_name/parse_method`, `status_reason/code` |
| `include/elaina/error.hpp` | `ErrorKind`, `HttpError`, `Expected<T,E>` (`std::expected` shim) |
| `include/elaina/limits.hpp` | `ServerLimits` (mutate via `Server::limits()`) |
| `include/elaina/http.hpp` | `HeaderView`, `Request` (borrowed), `Response` (owned) + factories |
| `include/elaina/context.hpp` | `ParamMap`, `Context`, `Handler` alias |
| `include/elaina/router.hpp` | Segment-trie `Router`, `RouteMatch` |
| `include/elaina/http_codec.hpp` | `StrictHttpParser` (`parse`/`parse_into`), `encode_response(_into)` |
| `include/elaina/reactor.hpp` | `Reactor`, `EpollReactor`, `PollReactor`, `make_default_reactor()` |
| `include/elaina/middleware.hpp` | `Next`, `Middleware`, `MiddlewareChain` + built-ins |
| `include/elaina/tls.hpp` | `TlsOptions`, `TlsAcceptor` (when `ELAINA_HAS_TLS`) |
| `include/elaina/connection.hpp` | `Connection`, `EventLoop` |
| `include/elaina/server.hpp` | `Server`, `RouteGroup`, `ResourceHandlers` |
| `modules/json/json.hpp` | `json::Value/Object/Array`, `stringify/parse/escape`, `find/get_*` helpers, `obj()` builder |
| `src/*.cpp` | Implementations: `router`, `http_codec`, `middleware`, `reactor`, `connection` (+`EventLoop`), `server`, `json_mini`, `tls` |

## 3. Server (`elaina::Server`)

Control plane (registration, single-threaded before `listen()`) plus
data plane (N event loops, `SO_REUSEPORT`, thread-affine connections).

```cpp
elaina::Server app;
app.threads(4);                       // 0 = auto (default)
app.limits().max_body_size = 4*1024*1024;

app.get("/a", h); app.post("/a", h); app.put("/a", h);
app.patch("/a", h); app.del("/a", h);
app.head("/a", h); app.options("/a", h);
app.any("/ping", h);                  // all 7 methods
app.route(elaina::Method::GET, "/x", h);

app.use(elaina::Logger());
app.use(elaina::Cors());

app.listen(3000);                     // blocks; throws on duplicates/bad TLS
// tests: int port = app.listen_ephemeral(); ... app.stop();
app.route_count(); app.has_tls(); app.tls_options();
```

- Registering an identical duplicate route throws `std::runtime_error`.
- `HEAD` responses have their body stripped automatically.
- A path that exists under another method yields `405`; otherwise `404`
  (or your `not_found()` handler).

## 4. Routing: groups, resources, static files

```cpp
// Prefixed groups (nestable):
app.group("/api/v1", [](elaina::RouteGroup& api) {
  api.get("/users", list); api.post("/users", create);
  api.get("/users/:id", show);
  api.group("/admin", [](elaina::RouteGroup& a) { a.get("/stats", s); });
});

// CRUD bundle (only the handlers you set are registered):
// list: GET base | create: POST base | show: GET base/:id
// update: PUT base/:id | patch: PATCH base/:id | remove: DELETE base/:id
elaina::ResourceHandlers h;
h.list = l; h.show = s; h.create = c; h.update = u; h.remove = d;
app.resource("/items", std::move(h));

// Static files (GET+HEAD, rejects "..", directory -> index.html,
// MIME sniffing, 32 MB cap):
app.static_files("/static", "./public");

// JSON probe + custom 404 (still runs through middleware):
app.health();                       // GET /health -> 200 {"status":"ok"}
app.health("/ping");
app.not_found([](elaina::Context&) {
  return elaina::Response::not_found(R"({"error":"nope"})");
});
```

Route patterns: static (`/users/new`), `:param` (`/users/:id`), terminal
wildcard (`/static/*`). Priority: static > param > wildcard.

## 5. Context (request input)

All borrowed views, zero allocation:

| Access | Example |
|---|---|
| Route param | `ctx.param("id")`, `ctx.param_int("id", 0)` |
| Query | `ctx.query("q")`, `ctx.query_or("q","")`, `ctx.query_int("page",1)`, `ctx.query_double("x",0.0)` |
| Header | `ctx.header("x-test")`, `ctx.header_or("x-test","-")` |
| Body | `ctx.body()` (view), `ctx.json()` → `optional<json::Value>` |
| Content type | `ctx.content_type()`, `ctx.is_json()` |
| Auth/cookie | `ctx.bearer_token()`, `ctx.cookie("sess")` |
| Meta | `ctx.method()`, `ctx.path()`, `ctx.client_ip()`, `ctx.request()` |

```cpp
app.post("/users", [](elaina::Context& ctx) -> elaina::Response {
  auto v = ctx.json();
  if (!v) return elaina::Response::bad_request(R"({"error":"invalid json"})");
  return elaina::Response::json_created(*v);
});
```

> Query strings are not percent-decoded (raw slices). Cookies are
> space-trimmed. `Authorization: Basic ...` belongs to the `BasicAuth`
> middleware, not `Context`.

## 6. Response (output)

```cpp
Response::text("hi");  Response::html("<b>x</b>");
Response::json(R"({"a":1})");
Response::json(json::obj({{"id", 1}}));          // no manual stringify
Response::json(val, Status::Created);
Response::created("..."); Response::json_created(v);
Response::no_content();                          // 204 (e.g. successful DELETE)
Response::bad_request("..."); Response::unauthorized(...);
Response::forbidden(...); Response::not_found(...); Response::internal(...);
Response::redirect("/login");                    // 302 + Location
Response::status_only(Status::NoContent);
Response::json(v).with_header("X-Trace","1");    // chaining
Response::text("x").with_cookie("sess=abc; Path=/; HttpOnly");
```

## 7. JSON (`elaina::json`)

```cpp
#include <elaina/elaina.hpp>  // json comes via http.hpp (or "json/json.hpp")

auto v = elaina::json::parse(body);              // optional<Value>
auto* o = std::get_if<elaina::json::Object>(&v->data);
auto* name = elaina::json::find(*o, "name");     // const Value* / nullptr
elaina::json::get_string(*name);                 // optional<string_view>
elaina::json::get_int / get_number / get_bool;
elaina::json::stringify(v); elaina::json::escape(s);
elaina::json::obj({{"id", 42}, {"tags", elaina::json::Array{}}});
```

Parser rules: exactly one value plus trailing whitespace; max depth 64;
fraction-less numbers become `int64` when they fit (else `double`);
`\uXXXX` with surrogate pairs supported; raw control bytes rejected.

## 8. Built-in middleware

```cpp
app.use(elaina::Logger());                       // stderr log (off for RPS benches)
app.use(elaina::Cors());                         // or Cors("https://app.example")
app.use(elaina::RequestId());                    // X-Request-Id echoed back
app.use(elaina::Recovery());                     // throw -> 500
app.use(elaina::RateLimit(100, std::chrono::seconds(60)));  // 429 + Retry-After
app.use(elaina::BasicAuth("user", "pass"));
app.use(elaina::BasicAuth([](std::string_view u, std::string_view p){ return ...; }));
app.use(elaina::BearerAuth("token"));
app.use(elaina::JwtHs256Auth("secret"));         // signature + "exp" claim
```

All take `(Context&, Next) -> Response`; skip `next(ctx)` to short-circuit.
`JwtHs256Auth` is a gate only (re-parse claims in the handler if needed).
Custom middleware:

```cpp
app.use([](elaina::Context& ctx, elaina::Next next) -> elaina::Response {
  elaina::Response r = next(ctx);
  r.with_header("X-App", "demo");
  return r;
});
```

## 9. TLS / HTTPS

Requires OpenSSL at build time (`-DELAINA_ENABLE_TLS=ON`, default ON when
found). Without OpenSSL the code still compiles; `tls()` throws at runtime.

```cpp
app.tls("server.crt", "server.key").listen(3443);
// or: app.listen_tls(3443, "server.crt", "server.key");
// mTLS: elaina::TlsOptions o{...}; o.ca_file="ca.crt"; o.verify_client=true;
//       app.tls(o).listen(3443);
```

```sh
openssl req -x509 -newkey rsa:2048 -keyout server.key -out server.crt \
  -days 365 -nodes -subj "/CN=localhost"
curl -k https://localhost:3443/
```

Details: non-blocking `SSL_accept` handshake (WANT_READ/WRITE), I/O via
`SSL_read`/`SSL_write`, one `TlsAcceptor` (min TLS 1.2) per loop thread, and
bad configuration fails fast in `listen()` instead of hanging. See
`include/elaina/tls.hpp`, `src/tls.cpp`, `src/connection.cpp`.

## 10. Limits, errors, status codes

```cpp
app.limits().max_body_size = 4*1024*1024;  // before listen()
app.limits().idle_timeout_sec = 30;
// max_uri_size 8KB, max_header_size 16KB, max_headers 100,
// header/body/idle timeouts, max_connections_per_loop 10000
```

Violations are answered with `413`/`431`/`408` automatically. `HttpError` +
`Expected<T,E>` (a `std::expected` shim for C++20) in `error.hpp` cover code
that needs structured errors without throwing. `Status` codes: 200/201/202/
204, 301/302/303/304, 400/401/403/404/405/408/429/413/431/500/501 — see
`method.hpp` (`status_reason()`, `status_code()`).

## 11. Lifetime & threading rules

1. **Borrowed views**: `Request`, `HeaderView`, and every `Context` return
   (`param/query/header/body/bearer_token/cookie`) is valid only for the
   handler call. Copy to `std::string` to retain anything.
   (`Response::text` and friends already copy into the owned body.)
2. **Register vs serve**: all `get/post/.../use/tls/not_found` calls happen
   before `listen()` (single-threaded). After `listen()`, tables are frozen
   and read lock-free by N threads.
3. **Non-blocking handlers**: handlers run on loop threads; blocking I/O
   (e.g. large file reads in `static_files`) stalls that loop — account for
   it under heavy load.
4. **Thread-affine connections**: connections never migrate between loops
   (`SO_REUSEPORT` spreads accepts in the kernel).
5. **Throw-free hot path**: parsing/routing/serving never throws; errors are
   represented by statuses + responses (except API misuse at registration).

## 12. Tests & examples

```sh
ctest --test-dir build --output-on-failure   # router, http_codec, integration, rest, tls
./build/examples/hello_world    # :3000  basics
./build/examples/json_example   # :3001  JSON
./build/examples/rest_example   # :3002  full CRUD group + middleware
./build/examples/https_example  # :3443  TLS + REST (needs server.crt/key)
```

- `tests/test_rest.cpp`: JSON parsing, Response/Context helpers, middleware
  (including JWT with a fixed vector), live group/resource/health/404/static.
- `tests/test_tls.cpp`: real HTTPS via OpenSSL (200 JSON, custom 404,
  garbage on the TLS port doesn't crash the server, bad cert → throw).
  Skipped gracefully without OpenSSL/CLI.
- `benchmarks/bench_micro` + `benchmarks/comparison/run.sh`: see
  `BENCHMARK.md` (no vs-Elysia claims without a same-machine run).

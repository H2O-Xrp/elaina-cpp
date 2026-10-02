# Referensi API elaina-cpp

> Dokumentasi lengkap seluruh kode. Untuk komentar per deklarasi, lihat
> header di `include/elaina/` (format Doxygen; generate HTML via `Doxyfile`).
> Tersedia juga dalam [English](API.md).
> Arsitektur: `architecture.md`. Performa: `../BENCHMARK.md`,
> `../PERFORMANCE_AUDIT.md`, `../PERFORMANCE_ARCHITECTURE.md`.

- [Konvensi](#konvensi)
- [1. Mulai cepat](#1-mulai-cepat)
- [2. Peta modul](#2-peta-modul)
- [3. Server](#3-server-elainaserver)
- [4. Routing: grup, resource, file statis](#4-routing-grup-resource-file-statis)
- [5. Context (input request)](#5-context-input-request)
- [6. Response (output)](#6-response-output)
- [7. JSON](#7-json-elainajson)
- [8. Middleware bawaan](#8-middleware-bawaan)
- [9. TLS / HTTPS](#9-tls--https)
- [10. Limit, error, status](#10-limit-error-status)
- [11. Aturan lifetime & threading](#11-aturan-lifetime--threading)
- [12. Test & contoh](#12-test--contoh)

## Konvensi

- **Handler** selalu berbentuk `Response(Context&)` dan return by value.
  Callable apa pun dengan bentuk itu bisa dipakai (lambda, fungsi, `Handler`).
- **Borrowed views**: semua yang dikembalikan `Context` (`param`, `query`,
  `header`, `body`, `bearer_token`, `cookie`) adalah `string_view` ke buffer
  koneksi — nol alokasi, valid hanya selama pemanggilan handler.
- **Chaining**: metode `Server` (`threads`, `get/post/...`, `use`, `tls`,
  `group`, `health`, ...) mengembalikan `Server&`/`RouteGroup&`.
- **Error**: salah pakai API saat registrasi melempar `std::runtime_error`;
  hot path request tidak pernah throw (lihat
  [bagian 10](#10-limit-error-status)).

## 1. Mulai cepat

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

## 2. Peta modul

| File | Isi |
|---|---|
| `include/elaina/elaina.hpp` | Header utama (include satu ini saja) |
| `include/elaina/method.hpp` | `Method`, `Status`, `method_name/parse_method`, `status_reason/code` |
| `include/elaina/error.hpp` | `ErrorKind`, `HttpError`, `Expected<T,E>` (shim `std::expected`) |
| `include/elaina/limits.hpp` | `ServerLimits` (diubah via `Server::limits()`) |
| `include/elaina/http.hpp` | `HeaderView`, `Request` (pinjam), `Response` (milik) + factory |
| `include/elaina/context.hpp` | `ParamMap`, `Context`, alias `Handler` |
| `include/elaina/router.hpp` | `Router` segment-trie, `RouteMatch` |
| `include/elaina/http_codec.hpp` | `StrictHttpParser` (`parse`/`parse_into`), `encode_response(_into)` |
| `include/elaina/reactor.hpp` | `Reactor`, `EpollReactor`, `PollReactor`, `make_default_reactor()` |
| `include/elaina/middleware.hpp` | `Next`, `Middleware`, `MiddlewareChain` + middleware bawaan |
| `include/elaina/tls.hpp` | `TlsOptions`, `TlsAcceptor` (bila `ELAINA_HAS_TLS`) |
| `include/elaina/connection.hpp` | `Connection`, `EventLoop` |
| `include/elaina/server.hpp` | `Server`, `RouteGroup`, `ResourceHandlers` |
| `modules/json/json.hpp` | `json::Value/Object/Array`, `stringify/parse/escape`, helper `find/get_*`, builder `obj()` |
| `src/*.cpp` | Implementasi: `router`, `http_codec`, `middleware`, `reactor`, `connection` (+`EventLoop`), `server`, `json_mini`, `tls` |

## 3. Server (`elaina::Server`)

Control-plane (registrasi, single-thread sebelum `listen`) + data-plane
(N event loop, `SO_REUSEPORT`, koneksi thread-affine).

```cpp
elaina::Server app;
app.threads(4);                       // 0 = auto (default)
app.limits().max_body_size = 4*1024*1024;

app.get("/a", h); app.post("/a", h); app.put("/a", h);
app.patch("/a", h); app.del("/a", h);
app.head("/a", h); app.options("/a", h);
app.any("/ping", h);                  // semua 7 metode
app.route(elaina::Method::GET, "/x", h);

app.use(elaina::Logger());
app.use(elaina::Cors());

app.listen(3000);                     // blokir; throw bila duplikat/TLS invalid
// test: int port = app.listen_ephemeral(); ... app.stop();
app.route_count(); app.has_tls(); app.tls_options();
```

- Duplikat route identik → `throw std::runtime_error`.
- `HEAD`: body otomatis dihapus framework.
- Path ada tapi metode salah → 405; tidak ada → 404 (atau handler `not_found()`).

## 4. Routing: grup, resource, file statis

```cpp
// Grup ber-prefix (bisa nested):
app.group("/api/v1", [](elaina::RouteGroup& api) {
  api.get("/users", list); api.post("/users", create);
  api.get("/users/:id", show);
  api.group("/admin", [](elaina::RouteGroup& a) { a.get("/stats", s); });
});

// Bundel CRUD (hanya yang terisi didaftarkan):
// list: GET base | create: POST base | show: GET base/:id
// update: PUT base/:id | patch: PATCH base/:id | remove: DELETE base/:id
elaina::ResourceHandlers h;
h.list = l; h.show = s; h.create = c; h.update = u; h.remove = d;
app.resource("/items", std::move(h));

// File statis (GET+HEAD, tolak "..", direktori -> index.html,
// snif MIME, batas 32MB):
app.static_files("/static", "./public");

// Probe JSON + 404 custom (tetap lewat middleware):
app.health();                       // GET /health -> 200 {"status":"ok"}
app.health("/ping");
app.not_found([](elaina::Context&) {
  return elaina::Response::not_found(R"({"error":"nope"})");
});
```

Pola route: statis (`/users/new`), `:param` (`/users/:id`), wildcard
terminal (`/static/*`). Prioritas: statis > param > wildcard.

## 5. Context (input request)

Semua view pinjaman, nol alokasi:

| Akses | Contoh |
|---|---|
| Param route | `ctx.param("id")`, `ctx.param_int("id", 0)` |
| Query | `ctx.query("q")`, `ctx.query_or("q","")`, `ctx.query_int("page",1)`, `ctx.query_double("x",0.0)` |
| Header | `ctx.header("x-test")`, `ctx.header_or("x-test","-")` |
| Body | `ctx.body()` (view), `ctx.json()` → `optional<json::Value>` |
| Tipe konten | `ctx.content_type()`, `ctx.is_json()` |
| Auth/cookie | `ctx.bearer_token()`, `ctx.cookie("sess")` |
| Meta | `ctx.method()`, `ctx.path()`, `ctx.client_ip()`, `ctx.request()` |

```cpp
app.post("/users", [](elaina::Context& ctx) -> elaina::Response {
  auto v = ctx.json();
  if (!v) return elaina::Response::bad_request(R"({"error":"invalid json"})");
  return elaina::Response::json_created(*v);
});
```

> Query tidak di-percent-decode (slice mentah). Cookie di-trim spasi.
> `Authorization: Basic ...` dipakai middleware `BasicAuth`, bukan `Context`.

## 6. Response (output)

```cpp
Response::text("hi");  Response::html("<b>x</b>");
Response::json(R"({"a":1})");
Response::json(json::obj({{"id", 1}}));          // tanpa stringify manual
Response::json(val, Status::Created);
Response::created("..."); Response::json_created(v);
Response::no_content();                          // 204 (mis. DELETE sukses)
Response::bad_request("..."); Response::unauthorized(...);
Response::forbidden(...); Response::not_found(...); Response::internal(...);
Response::redirect("/login");                    // 302 + Location
Response::status_only(Status::NoContent);
Response::json(v).with_header("X-Trace","1");    // chaining
Response::text("x").with_cookie("sess=abc; Path=/; HttpOnly");
```

## 7. JSON (`elaina::json`)

```cpp
#include <elaina/elaina.hpp>  // json ikut via http.hpp (atau "json/json.hpp")

auto v = elaina::json::parse(body);              // optional<Value>
auto* o = std::get_if<elaina::json::Object>(&v->data);
auto* name = elaina::json::find(*o, "name");     // const Value* / nullptr
elaina::json::get_string(*name);                 // optional<string_view>
elaina::json::get_int / get_number / get_bool;
elaina::json::stringify(v); elaina::json::escape(s);
elaina::json::obj({{"id", 42}, {"tags", elaina::json::Array{}}});
```

Aturan parse: satu nilai + whitespace ekor saja; kedalaman maks 64;
angka tanpa fraksi → `int64` bila muat (else `double`); `\uXXXX` +
surrogate pair didukung; control mentah ditolak.

## 8. Middleware bawaan

```cpp
app.use(elaina::Logger());                       // log stderr (matikan saat bench RPS)
app.use(elaina::Cors());                         // atau Cors("https://app.example")
app.use(elaina::RequestId());                    // X-Request-Id gema balik
app.use(elaina::Recovery());                     // throw -> 500
app.use(elaina::RateLimit(100, std::chrono::seconds(60)));  // 429 + Retry-After
app.use(elaina::BasicAuth("user", "pass"));
app.use(elaina::BasicAuth([](std::string_view u, std::string_view p){ return ...; }));
app.use(elaina::BearerAuth("token"));
app.use(elaina::JwtHs256Auth("secret"));         // verifikasi signature + "exp"
```

Semua menerima `(Context&, Next) -> Response`; short-circuit dengan tidak
memanggil `next`. `JwtHs256Auth` hanya gate (klaim dibaca ulang di handler
bila perlu). Custom:

```cpp
app.use([](elaina::Context& ctx, elaina::Next next) -> elaina::Response {
  elaina::Response r = next(ctx);
  r.with_header("X-App", "demo");
  return r;
});
```

## 9. TLS / HTTPS

Butuh OpenSSL saat build (`-DELAINA_ENABLE_TLS=ON`, default ON bila ketemu).
Tanpa OpenSSL kode tetap compile; `tls()` throw saat runtime.

```cpp
app.tls("server.crt", "server.key").listen(3443);
// atau: app.listen_tls(3443, "server.crt", "server.key");
// mTLS: elaina::TlsOptions o{...}; o.ca_file="ca.crt"; o.verify_client=true;
//       app.tls(o).listen(3443);
```

```sh
openssl req -x509 -newkey rsa:2048 -keyout server.key -out server.crt \
  -days 365 -nodes -subj "/CN=localhost"
curl -k https://localhost:3443/
```

Detail: handshake `SSL_accept` non-blocking (WANT_READ/WRITE), I/O via
`SSL_read`/`SSL_write`, `TlsAcceptor` (min TLS 1.2) satu per thread loop,
config salah → throw cepat di `listen()` (tidak hang). Lihat
`include/elaina/tls.hpp`, `src/tls.cpp`, `src/connection.cpp`.

## 10. Limit, error, status

```cpp
app.limits().max_body_size = 4*1024*1024;  // sebelum listen()
app.limits().idle_timeout_sec = 30;
// max_uri_size 8KB, max_header_size 16KB, max_headers 100,
// header/body/idle timeout, max_connections_per_loop 10000
```

Pelanggaran → 413/431/408 otomatis. `HttpError` + `Expected<T,E>`
(shim `std::expected` untuk C++20) tersedia di `error.hpp` untuk kode
yang butuh error terstruktur tanpa throw. Daftar `Status`: 200/201/202/
204, 301/302/303/304, 400/401/403/404/405/408/429/413/431/500/501 —
lihat `method.hpp` (`status_reason()`, `status_code()`).

## 11. Aturan lifetime & threading

1. **View pinjaman**: `Request`, `HeaderView`, dan semua return `Context`
   (`param/query/header/body/bearer_token/cookie`) hanya valid selama
   handler berjalan. Menyalin ke `std::string` bila perlu disimpan.
   (`Response::text` dan kawan-kawan sudah menyalin ke body milik.)
2. **Registrasi vs serve**: semua `get/post/.../use/tls/not_found` sebelum
   `listen()` (single-thread). Setelah `listen`, tabel beku, dibaca
   lock-free oleh N thread.
3. **Handler non-blocking**: berjalan di thread loop; I/O blokir (mis.
   baca file besar di `static_files`) menahan loop itu — perhitungkan
   di beban tinggi.
4. **Koneksi afinitas-thread**: tidak pernah migrasi antar loop
   (`SO_REUSEPORT` membagi accept di kernel).
5. **Hot path tanpa throw**: parse/route/serve tidak melempar; error
   diwakili status + respons (kecuali salah pakai API saat registrasi).

## 12. Test & contoh

```sh
ctest --test-dir build --output-on-failure   # router, http_codec, integration, rest, tls
./build/examples/hello_world    # :3000  dasar
./build/examples/json_example   # :3001  JSON
./build/examples/rest_example   # :3002  CRUD grup penuh + middleware
./build/examples/https_example  # :3443  TLS + REST (perlu server.crt/key)
```

- `tests/test_rest.cpp`: parse JSON, helper Response/Context, middleware
  (termasuk JWT dengan vektor tetap), live group/resource/health/404/static.
- `tests/test_tls.cpp`: HTTPS beneran via OpenSSL (200 JSON, 404 custom,
  garbage di port TLS tidak crash server, cert salah → throw). Skip anggun
  bila tanpa OpenSSL/CLI.
- `benchmarks/bench_micro` + `benchmarks/comparison/run.sh`: lihat
  `BENCHMARK.md` (jangan klaim vs Elysia tanpa run se-mesin).

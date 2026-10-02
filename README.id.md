# elaina-cpp

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)](https://en.cppreference.com/w/cpp/20)
[![Linux](https://img.shields.io/badge/platform-Linux-lightgrey)](https://www.kernel.org/)
[![CMake](https://img.shields.io/badge/build-CMake-green)](https://cmake.org/)
[![TLS](https://img.shields.io/badge/TLS-OpenSSL-orange)](https://www.openssl.org/)
[![License: MIT](https://img.shields.io/badge/license-MIT-yellow)](LICENSE)

> Read in [English](README.md).

Framework server HTTP C++20 berperforma tinggi dengan developer experience
ala Elysia/Fastify — dirancang dari awal untuk C++, Linux-first.

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

## Daftar isi

- [Fitur](#fitur)
- [Kebutuhan](#kebutuhan)
- [Build & test](#build--test)
- [Penggunaan](#penggunaan)
  - [Routing](#routing)
  - [JSON REST](#json-rest)
  - [Middleware](#middleware)
  - [HTTPS](#https)
- [Contoh](#contoh)
- [Konfigurasi](#konfigurasi)
- [Struktur proyek](#struktur-proyek)
- [Dokumentasi](#dokumentasi)
- [Benchmark](#benchmark)
- [Arsitektur](#arsitektur)
- [Roadmap](#roadmap)
- [Lisensi](#lisensi)

## Fitur

- **Routing ekspresif** — segmen statis, `:param`, dan wildcard terminal `*`;
  grup route ber-prefix (`app.group("/api/v1", ...)`), bundel CRUD
  (`app.resource(...)`), `GET/POST/PUT/PATCH/DELETE/HEAD/OPTIONS` plus `any()`.
- **Helper REST bawaan** — `Response::json(value)`, `created`, `no_content`,
  `bad_request`, `unauthorized`, `forbidden`, `not_found`, `internal`,
  `redirect`; akses bertipe `ctx.param_int`, `ctx.query_int/query_double`,
  `ctx.json()`, `ctx.bearer_token()`, `ctx.cookie()`.
- **JSON tanpa dependensi** — `json::parse/stringify` tanpa library pihak
  ketiga (seam Boost.JSON/glaze disiapkan untuk nanti).
- **Middleware bawaan** — `Logger`, `Cors`, `RequestId`, `Recovery`,
  `RateLimit`, `BasicAuth`, `BearerAuth`, `JwtHs256Auth`.
- **HTTPS langsung pakai** — `app.tls(cert, key).listen(3443)` (OpenSSL,
  handshake non-blocking, TLS 1.2+); fallback anggun bila build tanpa TLS.
- **File statis** — `app.static_files("/static", "./public")` dengan proteksi
  traversal, resolusi `index.html`, dan snif MIME.
- **Inti berorientasi performa** — event loop `epoll` per core dengan
  `SO_REUSEPORT`, hot path routing/matching nol-alokasi, model request
  borrowed-view, syscall dicoalesce. Lihat [Benchmark](#benchmark).

## Kebutuhan

| Perangkat | Minimum |
|---|---|
| Compiler C++ | GCC 13 / Clang 16 (C++20) |
| CMake | 3.25 |
| Library dev OpenSSL | 3.x (opsional; mengaktifkan HTTPS) |
| Linux | kernel dengan `epoll` (fallback `poll` bila tak ada) |

## Build & test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Opsi CMake yang berguna:

| Opsi | Default | Arti |
|---|---|---|
| `ELAINA_ENABLE_TLS` | `ON` | HTTPS via OpenSSL (otomatis mati bila OpenSSL tak ada) |
| `ELAINA_ENABLE_LTO` | `OFF` | Link-time optimization (`Release-LTO`) |
| `ELAINA_BUILD_EXAMPLES` | `ON` | Build `examples/` |
| `ELAINA_BUILD_TESTS` | `ON` | Build `tests/` |
| `ELAINA_BUILD_BENCHMARKS` | `ON` | Build `benchmarks/` |
| `ELAINA_WARNINGS_AS_ERRORS` | `OFF` | `-Werror` |

Sertifikat self-signed untuk uji HTTPS lokal:

```sh
openssl req -x509 -newkey rsa:2048 -keyout server.key -out server.crt \
  -days 365 -nodes -subj "/CN=localhost"
```

## Penggunaan

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

app.static_files("/static", "./public");  // GET /static/*, tolak traversal ".."
```

Pola route: statis (`/users/new`), `:param` (`/users/:id`), wildcard
terminal (`/static/*`). Prioritas: statis > param > wildcard. Route identik
yang duplikat melempar `std::runtime_error`. Path yang ada di metode lain
menjadi `405`; selain itu `404` (atau handler `not_found()` Anda, yang tetap
melewati middleware).

### JSON REST

```cpp
app.post("/users", [](elaina::Context& ctx) -> elaina::Response {
  auto v = ctx.json();
  if (!v) return elaina::Response::bad_request(R"({"error":"invalid json"})");
  return elaina::Response::json_created(*v);   // 201 + application/json
});
```

`Response::json()` menerima string jadi, `json::Value`, `Object`, atau
`Array` — tanpa `stringify()` manual.

### Middleware

```cpp
app.use(elaina::Logger());
app.use(elaina::Cors());                       // atau Cors("https://app.example")
app.use(elaina::RequestId());
app.use(elaina::Recovery());                   // throw -> 500, thread selamat
app.use(elaina::RateLimit(100, std::chrono::seconds(60)));  // 429 + Retry-After
app.use(elaina::BasicAuth("user", "pass"));
app.use(elaina::BearerAuth("token"));
app.use(elaina::JwtHs256Auth("secret"));       // signature + klaim "exp"
```

Middleware custom memakai bentuk yang sama `(Context&, Next) -> Response`;
lewati `next(ctx)` untuk short-circuit.

### HTTPS

```cpp
app.tls("server.crt", "server.key").listen(3443);
// one-shot: app.listen_tls(3443, "server.crt", "server.key");
// mTLS: TlsOptions o; o.ca_file = "ca.crt"; o.verify_client = true; app.tls(o)...
```

```sh
curl -k https://localhost:3443/
```

Konfigurasi TLS yang invalid gagal-cepat di `listen()` dengan error deskriptif,
bukan menggantung.

## Contoh

| Biner | Port | Deskripsi |
|---|---|---|
| `hello_world` | 3000 | Routing minimal |
| `json_example` | 3001 | Respons JSON |
| `rest_example` | 3002 | CRUD grup penuh + middleware (panduan curl di source) |
| `https_example` | 3443 | TLS + REST (`./https_example 3443 server.crt server.key`) |

## Konfigurasi

```cpp
app.threads(4);                             // 0 = auto = hardware_concurrency
app.limits().max_body_size = 4 * 1024 * 1024;
app.limits().idle_timeout_sec = 30;
```

Default: URI maks 8 KB, header maks 16 KB (100 field), body maks 1 MB,
timeout header/body/idle 5/10/60 dtk, 10.000 koneksi per loop.
Pelanggaran otomatis dijawab `413`/`431`/`408`.

## Struktur proyek

```text
include/elaina/   API publik (server, router, context, http, middleware, tls, ...)
src/              implementasi inti (lib elaina::core)
modules/json/     JSON tanpa dependensi (parse + stringify)
examples/         server yang bisa dijalankan (hello, json, rest, https)
tests/            router, http_codec, integration, rest, tls
benchmarks/       micro-benchmark + harness perbandingan Elaina/Elysia
docs/             referensi API, arsitektur, ADR
```

## Dokumentasi

- [`docs/API.id.md`](docs/API.id.md) — referensi API lengkap + contoh (Bahasa Indonesia)
- [`docs/API.md`](docs/API.md) — lengkap dalam Bahasa Inggris
- [`docs/architecture.md`](docs/architecture.md) — arsitektur & aliran data
- [`docs/adr/`](docs/adr/) — catatan keputusan arsitektur
- Komentar Doxygen di tiap header/source — generate HTML via `doxygen Doxyfile`
- [`BENCHMARK.md`](BENCHMARK.md), [`PERFORMANCE_AUDIT.md`](PERFORMANCE_AUDIT.md),
  [`PERFORMANCE_ARCHITECTURE.md`](PERFORMANCE_ARCHITECTURE.md) — kerja performa

## Benchmark

Micro-benchmark in-process plus harness Elaina-vs-Elysia se-mesin:

```sh
./build/benchmarks/bench_micro
bash benchmarks/comparison/run.sh --port 3000 --duration 30s --concurrency 256
```

> Jangan bandingkan angka beda mesin. Lihat `BENCHMARK.md` untuk metodologi
> dan aturan tetap: tanpa klaim "lebih cepat dari X" tanpa run se-mesin.

## Arsitektur

```text
Client ── TCP/TLS (SO_REUSEPORT) ──► EventLoop[i] (epoll, thread-affine)
  → Connection (pooled buffers) → StrictHttpParser → Router::match
  → MiddlewareChain (onion) → Handler → Response → encode → send
```

- **Control plane**: registrasi single-thread, dibekukan saat `listen()`.
- **Data plane**: N loop; koneksi tak pernah pindah antar loop.
- **Borrowed views**: `Request`/`Context` meminjam buffer terima dan hidup
  hanya selama pemanggilan handler — salin ke `std::string` untuk menyimpan.
- **Owned responses**: `Response` dibangun by value, di-encode, dikirim.
- Subset HTTP/1.0+1.1 yang ketat (`Content-Length`; `chunked` dijawab `501`).

## Roadmap

- Body request streaming (parsing sadar-`chunked`)
- Backend reactor `io_uring` di balik seam `Reactor` yang ada
- Handler coroutine (`Task<Response>`) sebagai opt-in di samping sync
- Backend JSON pluggable (Boost.JSON / glaze / simdjson)
- Dukungan WebSocket

## Lisensi

MIT — lihat [LICENSE](LICENSE).

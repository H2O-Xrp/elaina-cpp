# Arsitektur

> Tersedia juga dalam [English](architecture.md).
> Alasan di balik desain ini: [`adr/`](adr/). API lengkap: [`API.id.md`](API.id.md).

## Gambaran umum

`elaina-cpp` dibagi menjadi **control plane** (registrasi route
single-thread) dan **data plane** (N thread event-loop melayani trafik).

```text
Client ── TCP/TLS (SO_REUSEPORT) ──► EventLoop[i] (epoll, thread-affine)
  → Connection (pooled buffers) → StrictHttpParser → Router::match
  → MiddlewareChain (onion) → Handler → Response → encode → send
```

## Control plane

- `Server::get/post/put/patch/del/head/options/any/route` mengisi
  segment-trie per metode (`Router`). Route identik yang duplikat melempar.
- `Server::use` menambah ke `MiddlewareChain`; `not_found()` menyimpan
  handler 404 custom; `tls()` menyimpan `TlsOptions`.
- `listen()` memvalidasi konfigurasi TLS (gagal-cepat), membekukan tabel
  (registrasi happens-before serve, sehingga baca lock-free), lalu menelurkan
  N thread loop (`threads(n)`, default `hardware_concurrency`).

## Data plane

- Tiap `EventLoop` memiliki `Reactor` (`EpollReactor`, fallback
  `PollReactor`), socket listen `SO_REUSEPORT` sendiri (kernel membagi
  accept), dan pool koneksi. **Koneksi tak pernah pindah antar loop.**
- Tiap `Connection` memiliki buffer terima/kirim, `Request` pakai-ulang
  (kapasitas vektor header bertahan antar request keep-alive), dan — untuk
  HTTPS — sesi `SSL*` non-blocking (`WANT_READ`/`WANT_WRITE`).
- Jalur panas per request: `recv` → parse → match → middleware/handler →
  encode langsung ke buffer kirim → `send` oportunistik → konsumsi via offset
  (tanpa `memmove` per request). `epoll_ctl` dicoalesce ke transisi interest.

## Model kepemilikan

- `Request`/`HeaderView`/`Context` adalah **view pinjaman** ke buffer terima
  — valid hanya selama pemanggilan handler.
- `Response` **dimiliki by value** (di-move keluar handler, di-encode, dikirim).

## Dukungan protokol

- Subset HTTP/1.0 + HTTP/1.1 yang ketat: request line, header, body
  `Content-Length`, keep-alive. `chunked` eksplisit ditolak `501` (streaming
  adalah roadmap, bukan misbehavior diam-diam).
- Penjaga request-smuggling: duplikat `Content-Length`, `CL + chunked`,
  penolakan injeksi header / obs-fold, `ServerLimits` aman-secara-default.
- TLS 1.2+ via OpenSSL bila diaktifkan (`ELAINA_ENABLE_TLS`); satu
  `TlsAcceptor` per thread loop, tanpa berbagi `SSL_CTX` antar thread.

## Batasan saat ini

- Handler sinkron `Response(Context&)` dan harus non-blocking
  (berjalan di thread loop).
- Middleware berbasis `std::function` (komposisi compile-time menyusul).
- Modul JSON tanpa dependensi; seam backend Boost.JSON/glaze dicadangkan
  (ADR-008).
- `static_files` membaca file secara blocking (cocok untuk pemakaian wajar;
  dokumentasikan sebelum beban berat).

## Roadmap

Body request streaming, backend reactor `io_uring`, handler coroutine opt-in
(`Task<Response>`), backend JSON pluggable, dukungan WebSocket.

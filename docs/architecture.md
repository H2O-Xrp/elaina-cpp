# Architecture (MVP 0.1)

```
Client ── TCP (SO_REUSEPORT) ──► EventLoop[i] (epoll, thread-affine)
  → Connection (pooled buffers) → StrictHttpParser → Router::match
  → MiddlewareChain (onion) → Handler → Response → encode → send
```

* Control plane: `Server::get/post/use` (single-thread) → freeze saat `listen()`.
* Data plane: N loop, koneksi tidak pernah migrasi antar loop.
* `Request`/`Context` adalah borrowed views ke recv buffer; hidup hanya selama dispatch.
* `Response` owned by value (move), di-encode ke send buffer + `send()` oportunistik.

## Batasan MVP

* HTTP/1.1 saja, `Content-Length`; `chunked` ditolak eksplisit 501 (roadmap streaming).
* Middleware `std::function` (optimasi inline composition post-MVP).
* JSON mini tanpa dep; swap ke Boost.JSON/glaze via `JsonBackend` concept.
* TLS/WebSocket/H2/io_uring: post-MVP (lihat `plan.txt` roadmap P7+).

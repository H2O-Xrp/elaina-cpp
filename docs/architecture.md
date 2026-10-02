# Architecture

> Also available in [Bahasa Indonesia](architecture.id.md).
> Decisions behind this design: [`adr/`](adr/). Full API: [`API.md`](API.md).

## Overview

`elaina-cpp` is split into a **control plane** (single-threaded route
registration) and a **data plane** (N event-loop threads serving traffic).

```text
Client ── TCP/TLS (SO_REUSEPORT) ──► EventLoop[i] (epoll, thread-affine)
  → Connection (pooled buffers) → StrictHttpParser → Router::match
  → MiddlewareChain (onion) → Handler → Response → encode → send
```

## Control plane

- `Server::get/post/put/patch/del/head/options/any/route` fill a per-method
  segment-trie (`Router`). Duplicate identical routes throw.
- `Server::use` appends to `MiddlewareChain`; `not_found()` stores an
  optional custom 404 handler; `tls()` stores `TlsOptions`.
- `listen()` validates TLS configuration (fail fast), freezes the tables
  (registration happens-before serving, so reads are lock-free), and spawns
  N loop threads (`threads(n)`, default `hardware_concurrency`).

## Data plane

- Each `EventLoop` owns a `Reactor` (`EpollReactor`, `PollReactor` fallback),
  its own `SO_REUSEPORT` listen socket (the kernel spreads accepts), and a
  connection pool. **Connections never migrate between loops.**
- Each `Connection` owns its receive/send buffers, a reusable `Request`
  (header vector capacity survives across keep-alive requests), and — for
  HTTPS — an `SSL*` session driven non-blocking (`WANT_READ`/`WANT_WRITE`).
- Hot path per request: `recv` → parse → match → middleware/handler → encode
  directly into the send buffer → opportunistic `send` → offset-based consume
  (no per-request `memmove`). `epoll_ctl` is coalesced to interest transitions.

## Ownership model

- `Request`/`HeaderView`/`Context` are **borrowed views** into the receive
  buffer — valid only for the handler call.
- `Response` is **owned by value** (moved out of the handler, encoded, sent).

## Protocol support

- Strict HTTP/1.0 + HTTP/1.1 subset: request line, headers, `Content-Length`
  bodies, keep-alive. `chunked` is explicitly rejected with `501` (streaming
  is roadmap, not silent misbehavior).
- Request-smuggling guards: duplicate `Content-Length`, `CL + chunked`,
  header injection / obs-fold rejection, secure-by-default `ServerLimits`.
- TLS 1.2+ via OpenSSL when enabled (`ELAINA_ENABLE_TLS`); one `TlsAcceptor`
  per loop thread, no cross-thread `SSL_CTX` sharing.

## Current boundaries

- Handlers are synchronous `Response(Context&)` and must be non-blocking
  (they run on loop threads).
- Middleware is `std::function`-based (compile-time composition is future work).
- The JSON module is dependency-free; a Boost.JSON/glaze backend seam is
  reserved (ADR-008).
- `static_files` reads files blocking (fine for moderate use; document before
  heavy-load use).

## Roadmap

Streaming request bodies, `io_uring` reactor backend, opt-in coroutine
handlers (`Task<Response>`), pluggable JSON backends, WebSocket support.

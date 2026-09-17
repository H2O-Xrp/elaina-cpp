# elaina-cpp — high-performance C++ server framework (MVP 0.1)

Developer experience terinspirasi ElysiaJS/Fastify, dirancang dari awal untuk C++.

```cpp
#include <elaina/elaina.hpp>

int main() {
  elaina::Server app;
  app.get("/", [](elaina::Context& ctx) -> elaina::Response {
    return elaina::Response::text("Hello World");
  });
  app.get("/users/:id", [](elaina::Context& ctx) -> elaina::Response {
    return elaina::Response::text(std::string("user:") + std::string(ctx.param("id")));
  });
  app.listen(3000);
}
```

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/examples/hello_world        # :3000
./build/benchmarks/bench_micro
```

## Struktur

* `include/elaina/` — publik API (`server.hpp`, `router.hpp`, `context.hpp`, ...)
* `src/` — implementasi core (compiled lib `elaina::core`)
* `modules/json/` — JSON minimal tanpa dep (seam untuk Boost.JSON/glaze)
* `examples/`, `tests/`, `benchmarks/`, `docs/adr/`

## Keputusan arsitektur (ringkas)

* Baseline **C++20**, Linux-first, `epoll` MVP (`PollReactor` fallback).
* **Event loop per-core + SO_REUSEPORT**, connection thread-affine.
* Handler sync `Response(Context&)` untuk MVP; `Task<Response>` coroutine di Phase 5.
* Parser ketat built-in (`StrictHttpParser`, seam llhttp — ADR-007).
* Router segment-trie: `static > :param > *`, tanpa alokasi saat match ideal.
* Error hybrid `expected`-style, tanpa throw di hot path.
* JSON default modul mini (offline); Boost.JSON/glaze sebagai backend swap.

Lihat `docs/adr/` untuk 12 ADR dan `docs/architecture.md` untuk diagram.

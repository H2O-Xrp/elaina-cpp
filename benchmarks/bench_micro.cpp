// Micro-benchmarks (in-process, no network): router + encode + json.
// Run: ./bench_micro
#include <chrono>
#include <cstdio>

#include <elaina/elaina.hpp>
#include "../modules/json/json.hpp"

template <class F>
long long bench(const char* name, F&& f, int iters = 200000) {
  auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < iters; ++i) f(i);
  auto t1 = std::chrono::steady_clock::now();
  long long us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
  std::printf("%-24s %8lld us total  %8.1f ns/op  %10.0f ops/s\n", name, us,
              (double)us * 1000.0 / iters, (double)iters * 1e6 / (double)us);
  return us;
}

int main() {
  elaina::Router r;
  for (int i = 0; i < 100; ++i) {
    char pat[64];
    std::snprintf(pat, sizeof(pat), "/users/%d/profile", i);
    r.add(elaina::Method::GET, pat, [](elaina::Context&) {
      return elaina::Response::text("x");
    });
  }
  r.add(elaina::Method::GET, "/users/:id", [](elaina::Context&) {
    return elaina::Response::text("x");
  });

  bench("router static hit", [&](int) {
    volatile auto m = r.match(elaina::Method::GET, "/users/5/profile");
    (void)m;
  });
  bench("router param hit", [&](int) {
    volatile auto m = r.match(elaina::Method::GET, "/users/42");
    (void)m;
  });
  bench("router miss", [&](int) {
    volatile auto m = r.match(elaina::Method::GET, "/nope/nothing");
    (void)m;
  });

  elaina::Response hello = elaina::Response::text("Hello World");
  bench("encode hello", [&](int) {
    volatile auto s = elaina::encode_response(hello, true, "HTTP/1.1");
    (void)s;
  });

  elaina::json::Value v = elaina::json::Value(
      elaina::json::obj({{"id", 42}, {"name", "elaina"}, {"ok", true}}));
  bench("json stringify", [&](int) { volatile auto s = elaina::json::stringify(v); (void)s; });

  std::printf("\nFor network bench: build hello_world and run\n"
              "  wrk -t4 -c256 -d30s http://127.0.0.1:3000/\n");
  return 0;
}

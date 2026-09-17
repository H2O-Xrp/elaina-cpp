#include "elaina/middleware.hpp"

#include <cstdio>
#include <ctime>

namespace elaina {

Middleware Logger() {
  return [](Context& ctx, Next next) -> Response {
    auto t0 = std::chrono::steady_clock::now();
    Response res = next(ctx);
    auto t1 = std::chrono::steady_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    std::string_view path = ctx.path();
    std::fprintf(stderr, "%s %.*s -> %d (%lld us)\n",
                 std::string(method_name(ctx.method())).c_str(),
                 (int)path.size(), path.data(), status_code(res.status),
                 (long long)us);
    return res;
  };
}

Middleware Cors(std::string allow_origin) {
  return [origin = std::move(allow_origin)](Context& ctx, Next next) -> Response {
    if (ctx.method() == Method::OPTIONS) {
      Response r = Response::status_only(Status::NoContent);
      r.with_header("Access-Control-Allow-Origin", origin)
          .with_header("Access-Control-Allow-Methods", "GET, POST, PUT, PATCH, DELETE, OPTIONS")
          .with_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
      return r;
    }
    Response res = next(ctx);
    res.with_header("Access-Control-Allow-Origin", origin);
    return res;
  };
}

}  // namespace elaina

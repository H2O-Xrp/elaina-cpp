#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "context.hpp"
#include "http.hpp"

namespace elaina {

// Next callable for onion middleware: invokes the next layer.
using Next = std::function<Response(Context&)>;
// Middleware: may inspect/modify ctx, call next(ctx), or short-circuit.
using Middleware = std::function<Response(Context&, Next)>;

// Executes middleware chain + terminal handler without allocation per hop
// beyond std::function dispatch (MVP). Chain is frozen at listen().
class MiddlewareChain {
 public:
  void add(Middleware m) { middlewares_.push_back(std::move(m)); }
  std::size_t size() const noexcept { return middlewares_.size(); }

  Response execute(Context& ctx, const Handler& terminal) const {
    return invoke(ctx, terminal, 0);
  }

 private:
  Response invoke(Context& ctx, const Handler& terminal, std::size_t i) const {
    if (i >= middlewares_.size()) return terminal(ctx);
    const Middleware& m = middlewares_[i];
    Next next = [&](Context& c) -> Response { return invoke(c, terminal, i + 1); };
    return m(ctx, next);
  }

  std::vector<Middleware> middlewares_;
};

// ---- Built-in middleware (MVP) ----

// Logs: method path -> status in N ms. Sink defaults to stderr.
Middleware Logger();

// Minimal CORS: adds ACAO headers; short-circuits OPTIONS preflight.
Middleware Cors(std::string allow_origin = "*");

}  // namespace elaina

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "context.hpp"
#include "http.hpp"
#include "limits.hpp"
#include "method.hpp"
#include "middleware.hpp"
#include "router.hpp"

namespace elaina {

// Server: control plane (route registration) + data plane (N event loops).
//
//   Server app;
//   app.get("/", [](Context& ctx) -> Response { return Response::text("Hello"); });
//   app.get("/users/:id", [](Context& ctx) -> Response {
//     return Response::text(ctx.param("id"));
//   });
//   app.listen(3000);
//
// Threading: registration is single-threaded before listen(). listen()
// freezes the route table (happens-before) and spawns N loop threads
// (default = hardware_concurrency), each with its own SO_REUSEPORT acceptor
// (ADR-004). Handlers run on loop threads; must be non-blocking in MVP.
class Server {
 public:
  Server();
  ~Server();

  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  ServerLimits& limits() noexcept { return limits_; }
  const ServerLimits& limits() const noexcept { return limits_; }

  // Number of event-loop threads (0 = auto = hardware_concurrency, min 1).
  Server& threads(std::size_t n) {
    thread_count_ = n;
    return *this;
  }

  // ---- Route registration (control plane) ----
  template <class F>
  Server& get(std::string_view pattern, F&& f) {
    return route(Method::GET, pattern, std::forward<F>(f));
  }
  template <class F>
  Server& post(std::string_view pattern, F&& f) {
    return route(Method::POST, pattern, std::forward<F>(f));
  }
  template <class F>
  Server& put(std::string_view pattern, F&& f) {
    return route(Method::PUT, pattern, std::forward<F>(f));
  }
  template <class F>
  Server& patch(std::string_view pattern, F&& f) {
    return route(Method::PATCH, pattern, std::forward<F>(f));
  }
  template <class F>
  Server& del(std::string_view pattern, F&& f) {
    return route(Method::DELETE, pattern, std::forward<F>(f));
  }

  template <class F>
  Server& route(Method method, std::string_view pattern, F&& f) {
    static_assert(std::is_invocable_r_v<Response, F, Context&>,
                  "elaina: handler must be callable as Response(Context&). "
                  "Example: [](Context& ctx) -> Response { return Response::text(\"hi\"); }");
    Handler h = Handler(std::forward<F>(f));
    if (!router_.add(method, pattern, std::move(h))) {
      throw std::runtime_error("elaina: duplicate route registration: " +
                               std::string(pattern));
    }
    return *this;
  }

  Server& use(Middleware m) {
    middlewares_.add(std::move(m));
    return *this;
  }

  // ---- Serving (data plane) ----
  // Blocks until process signal / stop() from another thread.
  void listen(int port);
  // Non-blocking variant used by tests: binds ephemeral port, returns port.
  // Caller must call stop() to join.
  int listen_ephemeral();
  void stop();

  std::size_t route_count() const noexcept { return router_.route_count(); }

 private:
  void run_loops(int port, std::shared_ptr<std::atomic<bool>> stop_flag,
                 bool own_socket, int shared_fd);

  ServerLimits limits_;
  Router router_;
  MiddlewareChain middlewares_;
  std::size_t thread_count_ = 0;
  int shared_listen_fd_ = -1;

  std::vector<std::thread> workers_;
  std::atomic<bool> running_{false};
  std::shared_ptr<std::atomic<bool>> stop_flag_;
};

}  // namespace elaina

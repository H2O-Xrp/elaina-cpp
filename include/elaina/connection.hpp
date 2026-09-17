#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "http.hpp"
#include "http_codec.hpp"
#include "limits.hpp"
#include "middleware.hpp"
#include "reactor.hpp"
#include "router.hpp"

namespace elaina {

class EventLoop;

// Single TCP connection: owns recv/send buffers + parser state.
// Thread-affine: only touched by its EventLoop thread.
class Connection {
 public:
  Connection(int fd, std::string client_ip, const ServerLimits& limits,
             const Router* router, const MiddlewareChain* middlewares);
  ~Connection();

  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  int fd() const noexcept { return fd_; }
  bool closed() const noexcept { return closed_; }
  std::chrono::steady_clock::time_point last_activity() const noexcept {
    return last_activity_;
  }

  // Called by EventLoop when fd is readable/writable.
  // Returns false if connection should be destroyed.
  bool on_readable();
  bool on_writable();
  bool on_tick(std::chrono::steady_clock::time_point now);

  bool wants_write() const noexcept { return !send_buf_.empty() && send_off_ < send_buf_.size(); }

 private:
  bool dispatch_one();
  void make_error_response(Status s, std::string_view msg, bool keep_alive,
                           std::string_view version);
  void close() noexcept;

  int fd_ = -1;
  std::string client_ip_;
  ServerLimits limits_;
  const Router* router_;
  const MiddlewareChain* middlewares_;

  std::vector<char> recv_buf_;
  std::string send_buf_;
  std::size_t send_off_ = 0;
  bool closed_ = false;
  std::chrono::steady_clock::time_point created_;
  std::chrono::steady_clock::time_point last_activity_;
  StrictHttpParser parser_;
};

// One event loop per CPU core (ADR-004): owns Reactor + acceptor +
// connection pool. Connections never migrate between loops.
class EventLoop {
 public:
  using AcceptHandler = std::function<void(int client_fd, std::string ip)>;

  EventLoop(const ServerLimits& limits, const Router* router,
            const MiddlewareChain* middlewares);
  ~EventLoop();

  EventLoop(const EventLoop&) = delete;
  EventLoop& operator=(const EventLoop&) = delete;

  // Creates a SO_REUSEPORT listen socket bound to port. Returns fd or -1.
  static int make_listen_socket(int port, int backlog = 1024);

  // Runs loop on current thread: accept + poll + serve until stop().
  // Each loop creates its own listen socket (kernel distributes accepts).
  void run(int port, std::atomic<bool>& stop);

  // Single-socket variant (used by tests): serve one already-bound socket.
  void run_with_fd(int listen_fd, std::atomic<bool>& stop);

  void set_on_ready(std::function<void(int port)> cb) { on_ready_ = std::move(cb); }

 private:
  bool accept_all(int listen_fd);
  void tick_timeouts();

  ServerLimits limits_;
  const Router* router_;
  const MiddlewareChain* middlewares_;
  std::unique_ptr<Reactor> reactor_;
  std::unordered_map<int, std::unique_ptr<Connection>> conns_;
  std::function<void(int port)> on_ready_;
};

}  // namespace elaina

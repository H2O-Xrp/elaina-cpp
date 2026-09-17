#include "elaina/server.hpp"

#include <chrono>
#include <netinet/in.h>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "elaina/connection.hpp"

namespace elaina {

Server::Server() = default;
Server::~Server() { stop(); }

void Server::run_loops(int port, std::shared_ptr<std::atomic<bool>> stop_flag,
                       bool own_socket, int shared_fd) {
  std::size_t n = thread_count_ == 0
                      ? std::max<std::size_t>(1, std::thread::hardware_concurrency())
                      : thread_count_;
  workers_.reserve(n);
  if (!own_socket) {
    workers_.emplace_back([this, shared_fd, stop_flag]() {
      EventLoop loop(limits_, &router_, &middlewares_);
      loop.run_with_fd(shared_fd, *stop_flag);
    });
    return;
  }
  for (std::size_t i = 0; i < n; ++i) {
    workers_.emplace_back([this, port, stop_flag]() {
      EventLoop loop(limits_, &router_, &middlewares_);
      loop.run(port, *stop_flag);
    });
  }
}

void Server::listen(int port) {
  if (running_.exchange(true)) throw std::runtime_error("elaina: already listening");
  stop_flag_ = std::make_shared<std::atomic<bool>>(false);
  run_loops(port, stop_flag_, true, -1);
  for (auto& t : workers_) t.join();
  workers_.clear();
  running_ = false;
  stop_flag_.reset();
}

int Server::listen_ephemeral() {
  int probe = EventLoop::make_listen_socket(0);
  if (probe < 0) throw std::runtime_error("elaina: cannot bind ephemeral port");
  struct sockaddr_in6 sa {};
  socklen_t sl = sizeof(sa);
  if (::getsockname(probe, (struct sockaddr*)&sa, &sl) != 0) {
    ::close(probe);
    throw std::runtime_error("elaina: getsockname failed");
  }
  int port = ntohs(sa.sin6_port);
  if (running_.exchange(true)) {
    ::close(probe);
    throw std::runtime_error("elaina: already listening");
  }
  stop_flag_ = std::make_shared<std::atomic<bool>>(false);
  shared_listen_fd_ = probe;
  thread_count_ = 1;
  run_loops(port, stop_flag_, false, probe);
  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  return port;
}

void Server::stop() {
  if (stop_flag_) stop_flag_->store(true, std::memory_order_relaxed);
  for (auto& t : workers_) {
    if (t.joinable()) t.join();
  }
  workers_.clear();
  if (shared_listen_fd_ >= 0) {
    ::close(shared_listen_fd_);
    shared_listen_fd_ = -1;
  }
  running_ = false;
}

}  // namespace elaina

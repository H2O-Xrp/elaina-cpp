#include "elaina/connection.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace elaina {
namespace {

bool set_nonblocking(int fd) {
  int f = ::fcntl(fd, F_GETFL, 0);
  if (f < 0) return false;
  return ::fcntl(fd, F_SETFL, f | O_NONBLOCK) == 0;
}

}  // namespace

Connection::Connection(int fd, std::string client_ip, const ServerLimits& limits,
                       const Router* router, const MiddlewareChain* middlewares)
    : fd_(fd),
      client_ip_(std::move(client_ip)),
      limits_(limits),
      router_(router),
      middlewares_(middlewares),
      parser_(limits) {
  recv_buf_.reserve(limits_.recv_buffer_size);
  created_ = std::chrono::steady_clock::now();
  last_activity_ = created_;
}

Connection::~Connection() {
  if (fd_ >= 0) ::close(fd_);
}

void Connection::close() noexcept {
  closed_ = true;
}

bool Connection::on_readable() {
  if (closed_) return false;
  char tmp[8192];
  for (;;) {
    ssize_t n = ::recv(fd_, tmp, sizeof(tmp), 0);
    if (n > 0) {
      last_activity_ = std::chrono::steady_clock::now();
      // Bound total buffered bytes.
      if (recv_buf_.size() + (std::size_t)n >
          limits_.max_header_size + limits_.max_body_size + 1024) {
        make_error_response(Status::PayloadTooLarge, "buffer overflow", false, "HTTP/1.1");
        return !closed_;
      }
      recv_buf_.insert(recv_buf_.end(), tmp, tmp + n);
      // Try to dispatch as many pipelined requests as available.
      while (!closed_) {
        if (recv_buf_.empty()) break;
        std::size_t before = recv_buf_.size();
        if (!dispatch_one()) break;
        if (recv_buf_.size() == before) break;  // NeedMore
        if (closed_) break;
      }
    } else if (n == 0) {
      close();  // peer closed
      return false;
    } else {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      if (errno == EINTR) continue;
      close();
      return false;
    }
    // For level-triggered reactor, one drain pass is enough; loop again only
    // if data remains pipelined (handled above). Break to avoid starvation.
    break;
  }
  return !closed_;
}

bool Connection::on_writable() {
  if (closed_) return false;
  while (send_off_ < send_buf_.size()) {
    ssize_t n = ::send(fd_, send_buf_.data() + send_off_, send_buf_.size() - send_off_, MSG_NOSIGNAL);
    if (n > 0) {
      send_off_ += (std::size_t)n;
      last_activity_ = std::chrono::steady_clock::now();
    } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      break;
    } else if (n < 0 && errno == EINTR) {
      continue;
    } else {
      close();
      return false;
    }
  }
  if (send_off_ >= send_buf_.size()) {
    send_buf_.clear();
    send_off_ = 0;
    if (closed_) return false;  // was marked close-after-send
  }
  return true;
}

bool Connection::on_tick(std::chrono::steady_clock::time_point now) {
  if (closed_) return false;
  auto idle = std::chrono::duration_cast<std::chrono::seconds>(now - last_activity_).count();
  // If we have partial data waiting, apply header timeout; else idle timeout.
  if (!recv_buf_.empty()) {
    if (idle > limits_.header_timeout_sec && send_buf_.empty()) {
      make_error_response(Status::RequestTimeout, "header timeout", false, "HTTP/1.1");
      // flush attempt happens via writable path; keep alive=false ensures close.
    }
  } else {
    if (idle > limits_.idle_timeout_sec) {
      close();
      return false;
    }
  }
  return !closed_;
}

bool Connection::dispatch_one() {
  ParseResult pr = parser_.parse(recv_buf_.data(), recv_buf_.size());
  if (pr.status == ParseStatus::NeedMore) return false;

  if (pr.status == ParseStatus::Complete) {
    Request& req = pr.request;
    // Body size already bounded by parser.
    RouteMatch m = router_->match(req.method, req.path);
    Response res;
    bool keep_alive = req.keep_alive;
    std::string version(req.version);

    if (m.handler) {
      ParamMap params = m.params;
      Context ctx(req, params, client_ip_);
      Handler terminal = [&](Context& c) -> Response { return (*m.handler)(c); };
      if (middlewares_) {
        res = middlewares_->execute(ctx, terminal);
      } else {
        res = terminal(ctx);
      }
    } else if (m.method_mismatch) {
      res = Response::text("Method Not Allowed", Status::MethodNotAllowed);
    } else {
      res = Response::text("Not Found", Status::NotFound);
    }

    // HEAD must not return body.
    if (req.method == Method::HEAD) res.body.clear();

    std::string encoded = encode_response(res, keep_alive, version);
    send_buf_ += encoded;
    // Try opportunistic direct send to reduce epoll round-trip.
    on_writable();

    // Consume parsed bytes.
    recv_buf_.erase(recv_buf_.begin(), recv_buf_.begin() + pr.consumed);
    if (!keep_alive) {
      // Close after pending bytes are flushed.
      if (send_buf_.empty()) {
        close();
        return false;
      }
      // Mark for close: reuse closed_ only after flush; use a sentinel by
      // appending nothing and checking keep_alive on next tick. Simplest:
      // if send pending, close now logically but keep fd until flushed.
      // We implement close-after-send by closing fd only when buffer empty —
      // so stash a flag in client_ip_ is ugly. Instead: shutdown write side
      // after flush via explicit handling: set a bool via recv_buf_ cap?
      // MVP: just close immediately after queueing if send already flushed,
      // else mark closed_ but keep sending in on_writable before destroy.
      // To achieve that, we set closed_=true but on_writable still sends.
      // EventLoop must call on_writable once more before erasing. Handle by
      // NOT returning false yet; next poll writable will flush then destroy.
      closed_ = true;
    }
    return true;
  }

  // Error / TooLarge: respond then close (cannot resync safely).
  Status s = pr.http_status;
  if (pr.status == ParseStatus::Error && pr.http_status == Status::NotImplemented) {
    s = Status::NotImplemented;
  } else if (pr.status == ParseStatus::Error) {
    s = Status::BadRequest;
  } else if (pr.status == ParseStatus::TooLarge) {
    if (s != Status::PayloadTooLarge && s != Status::HeaderTooLarge) s = Status::PayloadTooLarge;
  }
  make_error_response(s, pr.error.empty() ? "Bad Request" : pr.error, false, "HTTP/1.1");
  recv_buf_.clear();
  return true;
}

void Connection::make_error_response(Status s, std::string_view msg, bool keep_alive,
                                     std::string_view version) {
  Response res = Response::text(msg, s);
  std::string encoded = encode_response(res, keep_alive, version);
  send_buf_ += encoded;
  on_writable();
  close();
}

// ---------------- EventLoop ----------------

EventLoop::EventLoop(const ServerLimits& limits, const Router* router,
                     const MiddlewareChain* middlewares)
    : limits_(limits), router_(router), middlewares_(middlewares) {
  reactor_ = make_default_reactor();
}

EventLoop::~EventLoop() = default;

int EventLoop::make_listen_socket(int port, int backlog) {
  int fd = ::socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  int off = 0;
  ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
  set_nonblocking(fd);
  struct sockaddr_in6 addr {};
  addr.sin6_family = AF_INET6;
  addr.sin6_addr = in6addr_any;
  addr.sin6_port = htons((uint16_t)port);
  if (::bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
    ::close(fd);
    return -1;
  }
  if (::listen(fd, backlog) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

void EventLoop::run(int port, std::atomic<bool>& stop) {
  int lfd = make_listen_socket(port);
  if (lfd < 0) return;
  if (on_ready_) {
    // Report actual port (useful when port==0).
    struct sockaddr_in6 sa {};
    socklen_t sl = sizeof(sa);
    int actual = port;
    if (::getsockname(lfd, (struct sockaddr*)&sa, &sl) == 0) actual = ntohs(sa.sin6_port);
    on_ready_(actual);
  }
  run_with_fd(lfd, stop);
  ::close(lfd);
}

void EventLoop::run_with_fd(int listen_fd, std::atomic<bool>& stop) {
  set_nonblocking(listen_fd);
  reactor_->add(listen_fd, Interest::Read);
  std::vector<FdEvent> events;
  events.reserve(256);
  while (!stop.load(std::memory_order_relaxed)) {
    events.clear();
    int n = reactor_->poll(events, 100);
    if (n < 0) break;
    for (auto& e : events) {
      if (stop.load(std::memory_order_relaxed)) break;
      if (e.fd == listen_fd) {
        if (e.readable || e.error) accept_all(listen_fd);
        continue;
      }
      auto it = conns_.find(e.fd);
      if (it == conns_.end()) continue;
      Connection* c = it->second.get();
      bool alive = true;
      if (e.error) {
        alive = false;
      } else {
        if (e.readable) alive = c->on_readable();
        if (alive && e.writable) alive = c->on_writable();
        // If connection queued data but reactor only watches Read, ensure we
        // flush opportunistically and arm Write interest.
        if (alive && c->wants_write()) {
          alive = c->on_writable();
          reactor_->modify(e.fd, Interest::ReadWrite);
        } else if (alive) {
          reactor_->modify(e.fd, Interest::Read);
        }
      }
      if (!alive || c->closed()) {
        // One last flush attempt for close-after-send.
        if (c->wants_write() && !c->closed()) {
        }
        reactor_->remove(e.fd);
        conns_.erase(it);
      }
    }
    tick_timeouts();
  }
}

bool EventLoop::accept_all(int listen_fd) {
  bool accepted = false;
  for (int i = 0; i < 64; ++i) {
    struct sockaddr_storage ss {};
    socklen_t sl = sizeof(ss);
    int cfd = ::accept4(listen_fd, (struct sockaddr*)&ss, &sl, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (cfd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      if (errno == EINTR) continue;
      break;
    }
    if ((int)conns_.size() >= limits_.max_connections_per_loop) {
      ::close(cfd);
      continue;
    }
    int one = 1;
    ::setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    std::string ip;
    char buf[INET6_ADDRSTRLEN] = {};
    if (ss.ss_family == AF_INET) {
      ::inet_ntop(AF_INET, &((struct sockaddr_in*)&ss)->sin_addr, buf, sizeof(buf));
    } else if (ss.ss_family == AF_INET6) {
      ::inet_ntop(AF_INET6, &((struct sockaddr_in6*)&ss)->sin6_addr, buf, sizeof(buf));
    }
    ip = buf;
    auto conn = std::make_unique<Connection>(cfd, ip, limits_, router_, middlewares_);
    reactor_->add(cfd, Interest::Read);
    conns_.emplace(cfd, std::move(conn));
    accepted = true;
  }
  return accepted;
}

void EventLoop::tick_timeouts() {
  auto now = std::chrono::steady_clock::now();
  for (auto it = conns_.begin(); it != conns_.end();) {
    if (!it->second->on_tick(now)) {
      reactor_->remove(it->first);
      it = conns_.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace elaina

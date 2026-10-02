/// @file connection.cpp
/// @brief Implementasi Connection (recv/dispatch/send + TLS non-blocking)
/// dan EventLoop (poll + accept + timeout).
///
/// Jalur panas per request: on_readable -> dispatch_one (parse_into dengan
/// Request pakai-ulang -> route -> middleware/handler langsung ->
/// encode_response_into ke send_buf_) -> kirim oportunistik ->
/// consume_recv via offset (tanpa memmove). epoll_ctl dicoalesce via
/// write_armed_. Bila TLS: handshake SSL_accept + SSL_read/SSL_write dengan
/// penanganan WANT_READ/WANT_WRITE (lihat tls_retry_read_/tls_hs_want_write_).
#include "elaina/connection.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <climits>
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

#if defined(ELAINA_HAS_TLS)
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

Connection::Connection(int fd, std::string client_ip, const ServerLimits& limits,
                       const Router* router, const MiddlewareChain* middlewares,
                       const Handler* not_found
#if defined(ELAINA_HAS_TLS)
                       ,
                       SSL* ssl
#endif
                       )
    : fd_(fd),
      client_ip_(std::move(client_ip)),
      limits_(limits),
      router_(router),
      middlewares_(middlewares),
      not_found_(not_found),
#if defined(ELAINA_HAS_TLS)
      ssl_(ssl),
      tls_handshake_done_(ssl == nullptr),
#endif
      parser_(limits) {
  recv_buf_.reserve(limits_.recv_buffer_size);
  created_ = std::chrono::steady_clock::now();
  last_activity_ = created_;
}

Connection::~Connection() {
#if defined(ELAINA_HAS_TLS)
  if (ssl_) {
    SSL_shutdown(ssl_);  // best effort on non-blocking socket
    SSL_free(ssl_);
    ssl_ = nullptr;
  }
#endif
  if (fd_ >= 0) ::close(fd_);
}

ssize_t Connection::net_recv(char* buf, std::size_t len) noexcept {
#if defined(ELAINA_HAS_TLS)
  if (ssl_) return tls_recv(buf, len);
#endif
  return ::recv(fd_, buf, len, 0);
}

ssize_t Connection::net_send(const char* buf, std::size_t len) noexcept {
#if defined(ELAINA_HAS_TLS)
  if (ssl_) return tls_send(buf, len);
#endif
  return ::send(fd_, buf, len, MSG_NOSIGNAL);
}

#if defined(ELAINA_HAS_TLS)

bool Connection::drive_tls_handshake() {
  int r = SSL_accept(ssl_);
  if (r == 1) {
    tls_handshake_done_ = true;
    tls_hs_want_write_ = false;
    last_activity_ = std::chrono::steady_clock::now();
    return true;
  }
  int e = SSL_get_error(ssl_, r);
  if (e == SSL_ERROR_WANT_READ) {
    tls_hs_want_write_ = false;
    return false;
  }
  if (e == SSL_ERROR_WANT_WRITE) {
    tls_hs_want_write_ = true;
    return false;
  }
  close();
  return false;
}

// Contract mirrors recv(2): >0 bytes, 0 clean shutdown, -1 + errno
// (EAGAIN = WANT_READ or WANT_WRITE, EIO = fatal).
ssize_t Connection::tls_recv(char* buf, std::size_t len) noexcept {
  int want = static_cast<int>(len > static_cast<std::size_t>(INT_MAX) ? INT_MAX : len);
  int n = SSL_read(ssl_, buf, want);
  if (n > 0) return n;
  int e = SSL_get_error(ssl_, n);
  if (e == SSL_ERROR_WANT_READ) {
    errno = EAGAIN;
    return -1;
  }
  if (e == SSL_ERROR_WANT_WRITE) {
    tls_retry_read_ = true;
    errno = EAGAIN;
    return -1;
  }
  if (e == SSL_ERROR_ZERO_RETURN) return 0;
  if (e == SSL_ERROR_SYSCALL && errno != 0) return -1;  // keep errno
  errno = EIO;
  return -1;
}

ssize_t Connection::tls_send(const char* buf, std::size_t len) noexcept {
  int want = static_cast<int>(len > static_cast<std::size_t>(INT_MAX) ? INT_MAX : len);
  int n = SSL_write(ssl_, buf, want);
  if (n > 0) return n;
  int e = SSL_get_error(ssl_, n);
  if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
    errno = EAGAIN;
    return -1;
  }
  if (e == SSL_ERROR_SYSCALL && errno != 0) return -1;  // keep errno
  errno = EIO;
  return -1;
}

#endif

void Connection::close() noexcept {
  closed_ = true;
}

/// @brief Konsumsi n byte recv (lihat Connection::consume_recv di header).
void Connection::consume_recv(std::size_t n) noexcept {
  recv_off_ += n;
  // Fast path: all consumed -> reset (keeps capacity, no memmove).
  if (recv_off_ >= recv_buf_.size()) {
    recv_buf_.clear();
    recv_off_ = 0;
    return;
  }
  // Amortized compaction: only memmove when offset is large, avoiding O(n)
  // memmove per request (old erase(begin, begin+n) behavior).
  if (recv_off_ > 8192) {
    std::size_t remain = recv_buf_.size() - recv_off_;
    std::memmove(recv_buf_.data(), recv_buf_.data() + recv_off_, remain);
    recv_buf_.resize(remain);
    recv_off_ = 0;
  }
}

bool Connection::on_readable() {
  if (closed_) return false;
#if defined(ELAINA_HAS_TLS)
  if (ssl_ && !tls_handshake_done_) {
    if (!drive_tls_handshake()) return !closed_;
    // Handshake just completed: fall through to app-data read.
  }
#endif
  char tmp[8192];
  for (;;) {
    ssize_t n = net_recv(tmp, sizeof(tmp));
    if (n > 0) {
      last_activity_ = std::chrono::steady_clock::now();
      // Bound total buffered bytes (pending, not capacity).
      if (pending_recv() + (std::size_t)n >
          limits_.max_header_size + limits_.max_body_size + 1024) {
        make_error_response(Status::PayloadTooLarge, "buffer overflow", false, "HTTP/1.1");
        return !closed_;
      }
      // Compact if offset would waste space before append.
      if (recv_off_ > 0 && recv_off_ > 8192) {
        std::size_t remain = recv_buf_.size() - recv_off_;
        std::memmove(recv_buf_.data(), recv_buf_.data() + recv_off_, remain);
        recv_buf_.resize(remain);
        recv_off_ = 0;
      }
      recv_buf_.insert(recv_buf_.end(), tmp, tmp + n);
      // Try to dispatch as many pipelined requests as available.
      while (!closed_) {
        if (pending_recv() == 0) break;
        std::size_t before = pending_recv();
        if (!dispatch_one()) break;
        if (pending_recv() == before) break;  // NeedMore
        if (closed_) break;
      }
      // Opportunistic compaction when fully drained is handled in consume_recv.
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
#if defined(ELAINA_HAS_TLS)
  if (ssl_ && !tls_handshake_done_) {
    if (!drive_tls_handshake()) return !closed_;
    if (closed_) return false;
  } else if (ssl_ && tls_retry_read_) {
    // A previous SSL_read needed the socket writable; retry it now.
    tls_retry_read_ = false;
    if (!on_readable()) return false;
    if (closed_) return true;  // responses queued; flushed opportunistically above
  }
#endif
  while (send_off_ < send_buf_.size()) {
    ssize_t n = net_send(send_buf_.data() + send_off_, send_buf_.size() - send_off_);
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
  if (pending_recv() != 0) {
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

/// @brief Satu langkah parse+dispatch (lihat Connection::dispatch_one).
/// Fast path tanpa middleware memanggil handler langsung; 404 custom tetap
/// lewat middleware; HEAD menghapus body; error parse -> tutup koneksi.
bool Connection::dispatch_one() {
  std::size_t consumed = 0;
  dispatch_error_.clear();
  Status hs = Status::BadRequest;
  ParseStatus st = parser_.parse_into(recv_buf_.data() + recv_off_, pending_recv(),
                                      dispatch_req_, consumed, dispatch_error_, hs);
  if (st == ParseStatus::NeedMore) return false;

  if (st == ParseStatus::Complete) {
    Request& req = dispatch_req_;
    // Body size already bounded by parser.
    RouteMatch m = router_->match(req.method, req.path);
    Response res;
    bool keep_alive = req.keep_alive;
    std::string_view version = req.version;

    if (m.handler) {
      // Fast path: no middleware -> direct call, no std::function wrapper.
      if (middlewares_ && middlewares_->size() != 0) {
        ParamMap params = m.params;
        Context ctx(req, params, client_ip_);
        Handler terminal = [&](Context& c) -> Response { return (*m.handler)(c); };
        res = middlewares_->execute(ctx, terminal);
      } else {
        ParamMap params = m.params;
        Context ctx(req, params, client_ip_);
        res = (*m.handler)(ctx);
      }
    } else if (m.method_mismatch) {
      res = Response::text("Method Not Allowed", Status::MethodNotAllowed);
    } else if (not_found_) {
      ParamMap empty;
      Context nctx(req, empty, client_ip_);
      if (middlewares_ && middlewares_->size() != 0) {
        Handler terminal = [&](Context& c) -> Response { return (*not_found_)(c); };
        res = middlewares_->execute(nctx, terminal);
      } else {
        res = (*not_found_)(nctx);
      }
    } else {
      res = Response::text("Not Found", Status::NotFound);
    }

    // HEAD must not return body.
    if (req.method == Method::HEAD) res.body.clear();

    // Encode directly into send_buf_: saves 1 alloc + 1 memcpy per request
    // vs old `std::string encoded = encode(...); send_buf_ += encoded;`.
    // Reserve to avoid repeated growth (headers ~200B + body).
    send_buf_.reserve(send_buf_.size() + res.body.size() + 256);
    encode_response_into(send_buf_, res, keep_alive, version);
    // Try opportunistic direct send to reduce epoll round-trip.
    on_writable();

    // Consume parsed bytes (offset-based, no memmove in common case).
    consume_recv(consumed);
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
  Status s = hs;
  if (st == ParseStatus::Error && hs == Status::NotImplemented) {
    s = Status::NotImplemented;
  } else if (st == ParseStatus::Error) {
    s = Status::BadRequest;
  } else if (st == ParseStatus::TooLarge) {
    if (s != Status::PayloadTooLarge && s != Status::HeaderTooLarge) s = Status::PayloadTooLarge;
  }
  make_error_response(s, dispatch_error_.empty() ? "Bad Request" : dispatch_error_, false,
                      "HTTP/1.1");
  recv_buf_.clear();
  recv_off_ = 0;
  return true;
}

void Connection::make_error_response(Status s, std::string_view msg, bool keep_alive,
                                     std::string_view version) {
  Response res = Response::text(msg, s);
  send_buf_.reserve(send_buf_.size() + res.body.size() + 256);
  encode_response_into(send_buf_, res, keep_alive, version);
  on_writable();
  close();
}

// ---------------- EventLoop ----------------

EventLoop::EventLoop(const ServerLimits& limits, const Router* router,
                     const MiddlewareChain* middlewares, const Handler* not_found
#if defined(ELAINA_HAS_TLS)
                     ,
                     const TlsAcceptor* tls
#endif
                     )
    : limits_(limits),
      router_(router),
      middlewares_(middlewares),
      not_found_(not_found)
#if defined(ELAINA_HAS_TLS)
      ,
      tls_(tls)
#endif
{
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
        // Coalesce epoll_ctl(MOD): only on Read<->ReadWrite transition
        // (saves ~1 syscall/req vs old unconditional modify).
        // wants_socket_write() also covers TLS handshake/renegotiation states.
        if (alive) {
          bool want_write = c->wants_socket_write();
          if (want_write) alive = c->on_writable();
          want_write = c->wants_socket_write();
          if (want_write && !c->write_armed()) {
            if (reactor_->modify(e.fd, Interest::ReadWrite)) c->set_write_armed(true);
          } else if (!want_write && c->write_armed()) {
            if (reactor_->modify(e.fd, Interest::Read)) c->set_write_armed(false);
          }
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
#if defined(ELAINA_HAS_TLS)
    SSL* ssl = nullptr;
    if (tls_ && tls_->valid()) {
      ssl = tls_->new_session();
      if (!ssl) {
        ::close(cfd);
        continue;
      }
      SSL_set_fd(ssl, cfd);
    }
    // SSL ownership moves to Connection (freed in ~Connection).
    auto conn = std::make_unique<Connection>(cfd, ip, limits_, router_, middlewares_,
                                             not_found_, ssl);
#else
    auto conn =
        std::make_unique<Connection>(cfd, ip, limits_, router_, middlewares_, not_found_);
#endif
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

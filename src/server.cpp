/// @file server.cpp
/// @brief Implementasi Server: RouteGroup, resource/static/health,
/// run_loops (N thread + TlsAcceptor per thread), listen/listen_ephemeral/stop.
///
/// ensure_tls_ok(): gagal-cepat bila konfigurasi TLS invalid agar listen()
/// tidak menggantung. Static-file: safe_join anti traversal + snif MIME.
#include "elaina/server.hpp"

#include <chrono>
#include <fstream>
#include <netinet/in.h>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "elaina/connection.hpp"

namespace elaina {

Server::Server() = default;
Server::~Server() { stop(); }

// ---- RouteGroup ----

RouteGroup::RouteGroup(Server& srv, std::string prefix)
    : srv_(&srv), prefix_(std::move(prefix)) {}

std::string RouteGroup::full(std::string_view pattern) const {
  return detail::join_prefix(prefix_, pattern);
}

// ---- REST conveniences ----

Server& Server::resource(std::string_view base, ResourceHandlers h) {
  std::string b = detail::normalize_prefix(base);
  std::string item = (b == "/") ? "/:id" : b + "/:id";
  if (h.list) route(Method::GET, b, std::move(*h.list));
  if (h.create) route(Method::POST, b, std::move(*h.create));
  if (h.show) route(Method::GET, item, std::move(*h.show));
  if (h.update) route(Method::PUT, item, std::move(*h.update));
  if (h.patch) route(Method::PATCH, item, std::move(*h.patch));
  if (h.remove) route(Method::DELETE, item, std::move(*h.remove));
  return *this;
}

namespace {

std::string_view mime_for(std::string_view path) noexcept {
  auto dot = path.rfind('.');
  if (dot == std::string_view::npos) return "application/octet-stream";
  std::string_view ext = path.substr(dot);
  if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
  if (ext == ".css") return "text/css; charset=utf-8";
  if (ext == ".js" || ext == ".mjs") return "text/javascript; charset=utf-8";
  if (ext == ".json") return "application/json";
  if (ext == ".txt") return "text/plain; charset=utf-8";
  if (ext == ".xml") return "application/xml";
  if (ext == ".svg") return "image/svg+xml";
  if (ext == ".png") return "image/png";
  if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
  if (ext == ".gif") return "image/gif";
  if (ext == ".webp") return "image/webp";
  if (ext == ".ico") return "image/x-icon";
  if (ext == ".woff") return "font/woff";
  if (ext == ".woff2") return "font/woff2";
  if (ext == ".ttf") return "font/ttf";
  if (ext == ".wasm") return "application/wasm";
  if (ext == ".pdf") return "application/pdf";
  return "application/octet-stream";
}

// Join dir + url-relative path lexically. Returns false on traversal attempt.
bool safe_join(std::string_view dir, std::string_view rel, std::string& out) {
  out.assign(dir.data(), dir.size());
  std::size_t start = 0;
  auto flush_comp = [&](std::string_view comp) -> bool {
    if (comp.empty() || comp == ".") return true;
    if (comp == "..") return false;
    for (char c : comp) {
      if (c == '\\' || c == '\0') return false;
    }
    if (!out.empty() && out.back() != '/') out.push_back('/');
    out.append(comp.data(), comp.size());
    return true;
  };
  while (start <= rel.size()) {
    std::size_t slash = rel.find('/', start);
    if (slash == std::string_view::npos) slash = rel.size();
    if (!flush_comp(rel.substr(start, slash - start))) return false;
    start = slash + 1;
  }
  return true;
}

}  // namespace

Server& Server::static_files(std::string_view url_prefix, std::string_view dir) {
  std::string prefix = detail::normalize_prefix(url_prefix);
  std::string root(dir);
  while (root.size() > 1 && root.back() == '/') root.pop_back();
  auto serve = [root, prefix](Context& ctx) -> Response {
    std::string_view path = ctx.path();
    std::string_view rel =
        path.size() >= prefix.size() ? path.substr(prefix.size()) : std::string_view{};
    if (!rel.empty() && rel.front() == '/') rel.remove_prefix(1);
    std::string fs;
    if (!safe_join(root, rel, fs)) return Response::forbidden("Forbidden");
    if (rel.empty() || path.back() == '/') {
      if (!fs.empty() && fs.back() != '/') fs.push_back('/');
      fs += "index.html";
    }
    std::ifstream f(fs, std::ios::binary);
    if (!f) return Response::not_found("Not Found");
    f.seekg(0, std::ios::end);
    std::streamoff sz = f.tellg();
    if (sz < 0 || sz > 32 * 1024 * 1024) return Response::status_only(Status::PayloadTooLarge);
    f.seekg(0, std::ios::beg);
    std::string body(static_cast<std::size_t>(sz), '\0');
    if (!f.read(body.data(), sz)) return Response::internal("Read error");
    Response r;
    r.status = Status::Ok;
    r.body = std::move(body);
    r.content_type = std::string(mime_for(fs));
    return r;
  };
  get(prefix + "/*", serve);
  head(prefix + "/*", std::move(serve));
  return *this;
}

Server& Server::health(std::string_view path) {
  return get(path, [](Context&) -> Response {
    return Response::json(json::Value(json::obj({{"status", "ok"}})));
  });
}

namespace {
// Fail fast on bad TLS config instead of hanging in listen().
void ensure_tls_ok(const TlsOptions& opts) {
  if (!opts.valid()) return;
#if defined(ELAINA_HAS_TLS)
  TlsAcceptor probe(opts);
  if (!probe.valid()) throw std::runtime_error("elaina: TLS: " + probe.error());
#else
  throw std::runtime_error("elaina: TLS configured but built without OpenSSL "
                           "(cmake -DELAINA_ENABLE_TLS=ON with OpenSSL installed)");
#endif
}
}  // namespace

void Server::run_loops(int port, std::shared_ptr<std::atomic<bool>> stop_flag,
                       bool own_socket, int shared_fd) {
  std::size_t n = thread_count_ == 0
                      ? std::max<std::size_t>(1, std::thread::hardware_concurrency())
                      : thread_count_;
  workers_.reserve(n);
  const Handler* nf = not_found_handler();
  TlsOptions tls = tls_opts_;  // copy: workers only read it
  if (!own_socket) {
    workers_.emplace_back([this, shared_fd, stop_flag, nf, tls]() {
#if defined(ELAINA_HAS_TLS)
      std::unique_ptr<TlsAcceptor> acc;
      const TlsAcceptor* acc_ptr = nullptr;
      if (tls.valid()) {
        acc = std::make_unique<TlsAcceptor>(tls);
        if (!acc->valid()) return;
        acc_ptr = acc.get();
      }
      EventLoop loop(limits_, &router_, &middlewares_, nf, acc_ptr);
#else
      (void)tls;
      EventLoop loop(limits_, &router_, &middlewares_, nf);
#endif
      loop.run_with_fd(shared_fd, *stop_flag);
    });
    return;
  }
  for (std::size_t i = 0; i < n; ++i) {
    workers_.emplace_back([this, port, stop_flag, nf, tls]() {
#if defined(ELAINA_HAS_TLS)
      std::unique_ptr<TlsAcceptor> acc;
      const TlsAcceptor* acc_ptr = nullptr;
      if (tls.valid()) {
        acc = std::make_unique<TlsAcceptor>(tls);
        if (!acc->valid()) return;
        acc_ptr = acc.get();
      }
      EventLoop loop(limits_, &router_, &middlewares_, nf, acc_ptr);
#else
      (void)tls;
      EventLoop loop(limits_, &router_, &middlewares_, nf);
#endif
      loop.run(port, *stop_flag);
    });
  }
}

void Server::listen(int port) {
  if (running_.exchange(true)) throw std::runtime_error("elaina: already listening");
  try {
    ensure_tls_ok(tls_opts_);
  } catch (...) {
    running_ = false;
    throw;
  }
  stop_flag_ = std::make_shared<std::atomic<bool>>(false);
  run_loops(port, stop_flag_, true, -1);
  for (auto& t : workers_) t.join();
  workers_.clear();
  running_ = false;
  stop_flag_.reset();
}

int Server::listen_ephemeral() {
  ensure_tls_ok(tls_opts_);
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

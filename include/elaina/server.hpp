/// @file server.hpp
/// @brief Server: control-plane registrasi + data-plane N event loop.
///
/// Gaya pakai:
/// @code
///   elaina::Server app;
///   app.get("/", [](elaina::Context& ctx) -> elaina::Response {
///     return elaina::Response::text("Hello");
///   });
///   app.group("/api/v1", [](elaina::RouteGroup& api) {
///     api.get("/users/:id", show_user);
///   });
///   app.listen(3000);  // atau app.tls(cert, key).listen(3443);
/// @endcode
///
/// Threading: registrasi single-thread sebelum listen(). listen()
/// membekukan tabel route (happens-before) lalu menelurkan N thread loop
/// (default = hardware_concurrency), masing-masing dengan acceptor
/// SO_REUSEPORT sendiri. Handler berjalan di thread loop; harus non-blocking.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
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
#include "tls.hpp"

namespace elaina {

namespace detail {
// "/api/" + "users" -> "/api/users"; "" + "/x" -> "/x".
/// @brief Gabung prefix grup + pola (pastikan ada '/' pemisah).
/// @return Path absolut hasil gabungan.
inline std::string join_prefix(std::string_view prefix, std::string_view pattern) {
  std::string out;
  out.reserve(prefix.size() + pattern.size() + 1);
  out.append(prefix.data(), prefix.size());
  if (!pattern.empty() && pattern.front() != '/') out.push_back('/');
  out.append(pattern.data(), pattern.size());
  if (out.empty() || out.front() != '/') out.insert(out.begin(), '/');
  return out;
}
// "" -> "/"; "/api/" -> "/api"; "api" -> "/api".
/// @brief Normalisasi prefix grup: absolut + tanpa trailing '/' (kecuali root).
inline std::string normalize_prefix(std::string_view p) {
  if (p.empty()) return "/";
  std::string out(p);
  if (out.front() != '/') out.insert(out.begin(), '/');
  while (out.size() > 1 && out.back() == '/') out.pop_back();
  return out;
}
}  // namespace detail

class Server;

// Route group: same handler style, patterns prefixed automatically.
//   app.group("/api/v1", [](elaina::RouteGroup& api) {
//     api.get("/users", list_users);
//     api.get("/users/:id", show_user);
//     api.post("/users", create_user);
//   });
/// @brief Grup route ber-prefix: gaya handler sama, pola otomatis berprefix.
///
/// Didapat via Server::group() (atau nested via group()). Semua verb
/// tersedia: get/post/put/patch/del/head/options/any.
class RouteGroup {
 public:
  /// @brief Bungkus Server + prefix (sudah dinormalisasi pemanggil).
  RouteGroup(Server& srv, std::string prefix);

  /// @brief Daftarkan GET prefix+pola.
  template <class F>
  RouteGroup& get(std::string_view pattern, F&& f) {
    return route(Method::GET, pattern, std::forward<F>(f));
  }
  /// @brief Daftarkan POST prefix+pola.
  template <class F>
  RouteGroup& post(std::string_view pattern, F&& f) {
    return route(Method::POST, pattern, std::forward<F>(f));
  }
  /// @brief Daftarkan PUT prefix+pola.
  template <class F>
  RouteGroup& put(std::string_view pattern, F&& f) {
    return route(Method::PUT, pattern, std::forward<F>(f));
  }
  /// @brief Daftarkan PATCH prefix+pola.
  template <class F>
  RouteGroup& patch(std::string_view pattern, F&& f) {
    return route(Method::PATCH, pattern, std::forward<F>(f));
  }
  /// @brief Daftarkan DELETE prefix+pola.
  template <class F>
  RouteGroup& del(std::string_view pattern, F&& f) {
    return route(Method::DELETE, pattern, std::forward<F>(f));
  }
  /// @brief Daftarkan HEAD prefix+pola.
  template <class F>
  RouteGroup& head(std::string_view pattern, F&& f) {
    return route(Method::HEAD, pattern, std::forward<F>(f));
  }
  /// @brief Daftarkan OPTIONS prefix+pola.
  template <class F>
  RouteGroup& options(std::string_view pattern, F&& f) {
    return route(Method::OPTIONS, pattern, std::forward<F>(f));
  }
  /// @brief Daftarkan satu handler untuk SEMUA metode pada prefix+pola.
  template <class F>
  RouteGroup& any(std::string_view pattern, F&& f);

  // Nested group: prefix + sub prefix.
  //   api.group("/admin", [](RouteGroup& a) { a.get("/stats", ...); });
  /// @brief Sub-grup nested: prefix + sub.
  /// @param sub Sub-prefix, mis. "/admin" di dalam "/api".
  /// @param fn Dipanggil dengan grup anak.
  template <class F>
  RouteGroup& group(std::string_view sub, F&& fn);

  /// @brief Prefix grup ini.
  const std::string& prefix() const noexcept { return prefix_; }

 private:
  /// @brief Route satu metode (dipakai semua verb).
  template <class F>
  RouteGroup& route(Method method, std::string_view pattern, F&& f);

  /// @brief prefix_ + pattern.
  std::string full(std::string_view pattern) const;

  Server* srv_;        ///< Server pemilik (bukan milik).
  std::string prefix_; ///< Prefix ternormalisasi.
};

// CRUD handlers for Server::resource(). Only the set ones are registered:
//   GET base          -> list     POST base         -> create
//   GET base/:id      -> show     PUT   base/:id    -> update
//                                    PATCH base/:id    -> patch
//                                    DELETE base/:id   -> remove
//   app.resource("/items", {.list = list_items, .show = show_item, ...});
/// @brief Bundel handler CRUD untuk Server::resource().
///
/// Hanya field yang terisi yang didaftarkan:
/// - list: GET base; create: POST base
/// - show: GET base/:id; update: PUT base/:id
/// - patch: PATCH base/:id; remove: DELETE base/:id
/// @code
///   elaina::ResourceHandlers h;
///   h.list = list_items; h.show = show_item;
///   app.resource("/items", std::move(h));
/// @endcode
struct ResourceHandlers {
  std::optional<Handler> list;    ///< GET base.
  std::optional<Handler> show;    ///< GET base/:id.
  std::optional<Handler> create;  ///< POST base.
  std::optional<Handler> update;  ///< PUT base/:id.
  std::optional<Handler> patch;   ///< PATCH base/:id.
  std::optional<Handler> remove;  ///< DELETE base/:id.
};

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
/// @brief Server HTTP: registrasi route (control-plane) + N loop (data-plane).
class Server {
 public:
  /// @brief Konstruktor default (belum listen).
  Server();
  /// @brief stop() bila masih jalan.
  ~Server();

  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  /// @brief Batas server (ubah sebelum listen()).
  ServerLimits& limits() noexcept { return limits_; }
  /// @brief Batas server (baca).
  const ServerLimits& limits() const noexcept { return limits_; }

  // Number of event-loop threads (0 = auto = hardware_concurrency, min 1).
  /// @brief Jumlah thread event-loop (0 = auto = hardware_concurrency).
  /// @return *this (chaining).
  Server& threads(std::size_t n) {
    thread_count_ = n;
    return *this;
  }

  // ---- Route registration (control plane) ----
  /// @brief Daftarkan GET. throw bila duplikat.
  template <class F>
  Server& get(std::string_view pattern, F&& f) {
    return route(Method::GET, pattern, std::forward<F>(f));
  }
  /// @brief Daftarkan POST. throw bila duplikat.
  template <class F>
  Server& post(std::string_view pattern, F&& f) {
    return route(Method::POST, pattern, std::forward<F>(f));
  }
  /// @brief Daftarkan PUT. throw bila duplikat.
  template <class F>
  Server& put(std::string_view pattern, F&& f) {
    return route(Method::PUT, pattern, std::forward<F>(f));
  }
  /// @brief Daftarkan PATCH. throw bila duplikat.
  template <class F>
  Server& patch(std::string_view pattern, F&& f) {
    return route(Method::PATCH, pattern, std::forward<F>(f));
  }
  /// @brief Daftarkan DELETE. throw bila duplikat.
  template <class F>
  Server& del(std::string_view pattern, F&& f) {
    return route(Method::DELETE, pattern, std::forward<F>(f));
  }
  /// @brief Daftarkan HEAD. throw bila duplikat.
  template <class F>
  Server& head(std::string_view pattern, F&& f) {
    return route(Method::HEAD, pattern, std::forward<F>(f));
  }
  /// @brief Daftarkan OPTIONS. throw bila duplikat.
  template <class F>
  Server& options(std::string_view pattern, F&& f) {
    return route(Method::OPTIONS, pattern, std::forward<F>(f));
  }
  // Register one handler for every method (GET/POST/PUT/PATCH/DELETE/HEAD/OPTIONS).
  /// @brief Daftarkan satu handler untuk SEMUA metode. throw bila duplikat.
  template <class F>
  Server& any(std::string_view pattern, F&& f) {
    static_assert(std::is_invocable_r_v<Response, F, Context&>,
                  "elaina: handler must be callable as Response(Context&).");
    Handler h = Handler(std::forward<F>(f));
    for (std::size_t m = 0; m < kMethodCount; ++m) {
      if (!router_.add(static_cast<Method>(m), pattern, Handler(h))) {
        throw std::runtime_error("elaina: duplicate route registration: " +
                                 std::string(pattern));
      }
    }
    return *this;
  }

  /// @brief Daftarkan metode eksplisit. Handler harus Response(Context&).
  /// @throw std::runtime_error bila route duplikat.
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

  /// @brief Tambah middleware (dieksekusi urutan registrasi).
  Server& use(Middleware m) {
    middlewares_.add(std::move(m));
    return *this;
  }

  // ---- REST conveniences (same handler style) ----

  // Prefix group. See RouteGroup above.
  /// @brief Grup ber-prefix (lihat RouteGroup).
  /// @param prefix Prefix, mis. "/api/v1". @param fn Dipanggil dengan grup.
  template <class F>
  Server& group(std::string_view prefix, F&& fn) {
    RouteGroup g(*this, detail::normalize_prefix(prefix));
    fn(g);
    return *this;
  }

  // CRUD bundle. See ResourceHandlers above.
  /// @brief Bundel CRUD (lihat ResourceHandlers). Hanya yang terisi didaftarkan.
  Server& resource(std::string_view base, ResourceHandlers h);

  // Serve files under `dir` at URL prefix (GET only, traversal-safe).
  //   app.static_files("/static", "./public");  // GET /static/app.js
  /// @brief Sajikan file statis: GET+HEAD prefix/* dari direktori.
  /// @param url_prefix Prefix URL. @param dir Direktori root.
  /// @note Anti traversal (".." ditolak 403); direktori -> index.html;
  ///   MIME disnif dari ekstensi; maks 32MB; baca file blocking (MVP).
  Server& static_files(std::string_view url_prefix, std::string_view dir);

  // JSON health probe: GET path -> 200 {"status":"ok"}.
  /// @brief Probe kesehatan JSON: GET path -> 200 {"status":"ok"}.
  Server& health(std::string_view path = "/health");

  // Custom JSON-friendly 404 instead of the default plain-text one.
  //   app.not_found([](Context&) { return Response::not_found("nope"); });
  /// @brief Handler 404 custom (tetap lewat middleware).
  /// @note Tanpa ini, 404 default teks polos "Not Found".
  template <class F>
  Server& not_found(F&& f) {
    static_assert(std::is_invocable_r_v<Response, F, Context&>,
                  "elaina: handler must be callable as Response(Context&).");
    not_found_ = Handler(std::forward<F>(f));
    return *this;
  }
  /// @brief Handler 404 custom (null bila tak diset). Dipakai EventLoop.
  const Handler* not_found_handler() const noexcept {
    return not_found_ ? &*not_found_ : nullptr;
  }

  // ---- TLS (same chaining style; needs OpenSSL at build time) ----
  //   app.tls("server.crt", "server.key").listen(3443);
  /// @brief Aktifkan TLS dari opsi (chaining). throw bila sedang running.
  Server& tls(TlsOptions opts) {
    if (running_.load()) throw std::runtime_error("elaina: cannot set TLS while running");
    tls_opts_ = std::move(opts);
    return *this;
  }
  /// @brief Aktifkan TLS dari file cert+key (chaining).
  Server& tls(std::string cert_file, std::string key_file) {
    TlsOptions o;
    o.cert_file = std::move(cert_file);
    o.key_file = std::move(key_file);
    return tls(std::move(o));
  }
  /// @brief true bila TLS dikonfigurasi.
  bool has_tls() const noexcept { return tls_opts_.valid(); }
  /// @brief Opsi TLS saat ini.
  const TlsOptions& tls_options() const noexcept { return tls_opts_; }

  // One-shot HTTPS: equivalent to tls(...).listen(port).
  /// @brief HTTPS one-shot: setara tls(opts).listen(port).
  void listen_tls(int port, TlsOptions opts) {
    tls(std::move(opts));
    listen(port);
  }
  /// @brief HTTPS one-shot dari file cert+key.
  void listen_tls(int port, std::string cert_file, std::string key_file) {
    TlsOptions o;
    o.cert_file = std::move(cert_file);
    o.key_file = std::move(key_file);
    listen_tls(port, std::move(o));
  }

  // ---- Serving (data plane) ----
  // Blocks until process signal / stop() from another thread.
  // Uses TLS when tls() was configured, plain HTTP otherwise.
  /// @brief Layani di port (blokir). Pakai TLS bila dikonfigurasi.
  /// @throw std::runtime_error bila sudah running / sudah listen / TLS invalid.
  void listen(int port);
  // Non-blocking variant used by tests: binds ephemeral port, returns port.
  // Caller must call stop() to join.
  /// @brief Varian non-blokir untuk test: bind port efemeral, kembalikan port.
  /// @return Nomor port aktual. Wajib stop() untuk join.
  int listen_ephemeral();
  /// @brief Minta berhenti + join semua thread loop. Idempoten.
  void stop();

  /// @brief Jumlah route terdaftar.
  std::size_t route_count() const noexcept { return router_.route_count(); }

 private:
  /// @brief Telurkan N thread loop (TLS acceptor per thread bila dikonfigurasi).
  void run_loops(int port, std::shared_ptr<std::atomic<bool>> stop_flag,
                 bool own_socket, int shared_fd);

  ServerLimits limits_;              ///< Batas (salinan).
  Router router_;                    ///< Tabel route (beku saat listen).
  MiddlewareChain middlewares_;      ///< Rantai middleware (beku saat listen).
  TlsOptions tls_opts_;              ///< Opsi TLS (kosong = polos).
  std::optional<Handler> not_found_; ///< Handler 404 custom.
  std::size_t thread_count_ = 0;     ///< 0 = auto.
  int shared_listen_fd_ = -1;        ///< Socket efemeral (test).

  std::vector<std::thread> workers_;              ///< Thread loop.
  std::atomic<bool> running_{false};              ///< Sedang listen?
  std::shared_ptr<std::atomic<bool>> stop_flag_;  ///< Flag berhenti bersama.
};

// ---- RouteGroup template bodies (Server is complete here) ----

/// @brief any(): daftarkan ke semua metode (lihat RouteGroup::any).
template <class F>
RouteGroup& RouteGroup::any(std::string_view pattern, F&& f) {
  static_assert(std::is_invocable_r_v<Response, F, Context&>,
                "elaina: handler must be callable as Response(Context&).");
  std::string full_pat = full(pattern);
  Handler h = Handler(std::forward<F>(f));
  for (std::size_t m = 0; m < kMethodCount; ++m)
    srv_->route(static_cast<Method>(m), full_pat, Handler(h));
  return *this;
}

/// @brief group(): sub-grup nested (lihat RouteGroup::group).
template <class F>
RouteGroup& RouteGroup::group(std::string_view sub, F&& fn) {
  // "/api" + "/admin" -> "/api/admin".
  RouteGroup nested(*srv_, detail::join_prefix(prefix_, detail::normalize_prefix(sub)));
  fn(nested);
  return *this;
}

/// @brief route(): inti semua verb grup.
template <class F>
RouteGroup& RouteGroup::route(Method method, std::string_view pattern, F&& f) {
  srv_->route(method, full(pattern), std::forward<F>(f));
  return *this;
}

}  // namespace elaina

/// @file middleware.hpp
/// @brief Rantai middleware onion + middleware bawaan.
///
/// Middleware bisa mengintip/mengubah ctx, memanggil next(ctx), atau
/// short-circuit (tanpa memanggil next). Tanpa middleware, handler
/// dipanggil langsung (tanpa overhead rantai).
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
/// @brief Callable lapisan berikut dalam rantai onion.
using Next = std::function<Response(Context&)>;
// Middleware: may inspect/modify ctx, call next(ctx), or short-circuit.
/// @brief Tipe middleware: (Context&, Next) -> Response.
using Middleware = std::function<Response(Context&, Next)>;

// Executes middleware chain + terminal handler without allocation per hop
// beyond std::function dispatch (MVP). Chain is frozen at listen().
/// @brief Rantai middleware + handler terminal.
///
/// Dieksekusi rekursif per hop (dispatch std::function). Chain dibekukan
/// saat listen(). Bila kosong, Connection memanggil handler langsung.
class MiddlewareChain {
 public:
  /// @brief Tambah middleware (control-plane, sebelum listen()).
  void add(Middleware m) { middlewares_.push_back(std::move(m)); }
  /// @brief Jumlah middleware.
  std::size_t size() const noexcept { return middlewares_.size(); }

  /// @brief Jalankan rantai + handler terminal.
  /// @param ctx Context request. @param terminal Handler akhir.
  /// @return Respons (dari middleware short-circuit atau handler).
  Response execute(Context& ctx, const Handler& terminal) const {
    return invoke(ctx, terminal, 0);
  }

 private:
  /// @brief Rekursi per-hop.
  Response invoke(Context& ctx, const Handler& terminal, std::size_t i) const {
    if (i >= middlewares_.size()) return terminal(ctx);
    const Middleware& m = middlewares_[i];
    Next next = [&](Context& c) -> Response { return invoke(c, terminal, i + 1); };
    return m(ctx, next);
  }

  std::vector<Middleware> middlewares_;  ///< Lapisan (urutan registrasi).
};

// ---- Built-in middleware (same app.use(...) style) ----

// Logs: method path -> status in N ms. Sink defaults to stderr.
/// @brief Log tiap request ke stderr: "METODE path -> status (N us)".
/// @warning Ada syscall tulis per request; jangan dipakai saat benchmark RPS.
Middleware Logger();

// Minimal CORS: adds ACAO headers; short-circuits OPTIONS preflight.
/// @brief CORS minimal: tambah header ACAO; short-circuit preflight OPTIONS.
/// @param allow_origin Nilai Access-Control-Allow-Origin (default "*").
Middleware Cors(std::string allow_origin = "*");

// Request ID: reuses incoming header or generates one ("rid-<hex>").
// Always echoed back on the response.
/// @brief Request ID: pakai ulang header masuk atau generate ("rid-<hex>").
/// Selalu digemakan kembali di respons.
/// @param header Nama header (default "X-Request-Id").
Middleware RequestId(std::string header = "X-Request-Id");

// Exception guard: a throwing handler/middleware becomes 500 instead of
// killing the loop thread.
/// @brief Penjaga exception: handler yang throw menjadi 500, bukan crash thread.
Middleware Recovery();

// Per-IP fixed-window rate limit. Exceeded -> 429 + Retry-After.
// MVP: in-memory, per-process (each loop thread tracks its own conns;
// counters are shared behind a mutex). Not for distributed setups.
/// @brief Rate limit fixed-window per IP.
/// @param max_requests Maks request per window. @param window Lebar window.
/// @return 429 + Retry-After bila terlampaui.
/// @note In-memory per proses; bukan untuk setup terdistribusi.
Middleware RateLimit(int max_requests, std::chrono::seconds window);

// HTTP Basic auth (constant-time compare). Fail -> 401 + WWW-Authenticate.
/// @brief Auth Basic username/password (perbandingan constant-time).
Middleware BasicAuth(std::string username, std::string password);
/// @brief Auth Basic dengan validator custom.
/// @param verify Predikat (user, pass). @param realm Nilai realm 401.
Middleware BasicAuth(std::function<bool(std::string_view user, std::string_view pass)> verify,
                     std::string realm = "elaina");

// Bearer token auth. Fail -> 401 + WWW-Authenticate: Bearer.
/// @brief Auth Bearer token eksak (constant-time).
Middleware BearerAuth(std::string token);
/// @brief Auth Bearer dengan validator custom.
Middleware BearerAuth(std::function<bool(std::string_view token)> verify);

// JWT HS256 auth: verifies signature + "exp" claim (if present).
// Fail -> 401. No extra deps (self-contained SHA-256/HMAC).
// Stash nothing on Context (gate only); re-parse claims in handler if needed.
/// @brief Auth JWT HS256: verifikasi signature + klaim "exp" bila ada.
/// @param secret Kunci HMAC. Gagal -> 401.
/// @note Gate saja (tidak menempel klaim ke Context); tanpa dep tambahan.
Middleware JwtHs256Auth(std::string secret);

}  // namespace elaina

/// @file context.hpp
/// @brief Fasad per-request: ParamMap, Context, dan alias Handler.
///
/// Semua akses berupa view pinjaman (tanpa alokasi): param route, query,
/// header, body, cookie, dan token. Jangan menyimpan view melewati
/// return handler.
#pragma once

#include <array>
#include <charconv>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "http.hpp"
#include "json/json.hpp"

namespace elaina {

/// @brief Peta parameter route tanpa alokasi.
///
/// View nama/nilai meminjam buffer recv / path. Kapasitas 8 cukup untuk
/// pola seperti /a/:b/c/:d. Kelebihan kapasitas membuat match gagal
/// (terdokumentasi, bukan crash).
struct ParamMap {
  /// @brief Maks pasangan nama=nilai.
  static constexpr std::size_t kMax = 8;
  std::array<std::string_view, kMax> names{};   ///< Nama parameter.
  std::array<std::string_view, kMax> values{};  ///< Nilai parameter.
  std::size_t size = 0;  ///< Jumlah terisi.

  /// @brief Kosongkan (dipakai ulang antar request).
  void clear() noexcept { size = 0; }
  /// @brief Tambah pasangan.
  /// @return false bila penuh.
  bool add(std::string_view n, std::string_view v) noexcept {
    if (size >= kMax) return false;
    names[size] = n;
    values[size] = v;
    ++size;
    return true;
  }
  /// @brief Cari nilai berdasar nama.
  /// @return View nilai atau nullopt.
  std::optional<std::string_view> get(std::string_view n) const noexcept {
    for (std::size_t i = 0; i < size; ++i) {
      if (names[i] == n) return values[i];
    }
    return std::nullopt;
  }
};

// Per-request facade. Borrowed; must not escape handler.
// Owns nothing except views + deadline info.
/// @brief Fasad request untuk handler: pinjam, tanpa alokasi.
///
/// Hidup hanya selama dispatch() ke handler. Contoh:
/// @code
///   app.get("/users/:id", [](elaina::Context& ctx) -> elaina::Response {
///     auto v = ctx.json();  // std::optional<json::Value>
///     return elaina::Response::text(ctx.param("id"));
///   });
/// @endcode
class Context {
 public:
  /// @brief Bungkus Request + param hasil match.
  /// @param req Request pinjaman. @param params Param hasil routing.
  /// @param client_ip IP klien (untuk log/rate-limit).
  Context(const Request& req, ParamMap& params, std::string_view client_ip = {})
      : req_(req), params_(params), client_ip_(client_ip) {}

  /// @brief Request mentah.
  const Request& request() const noexcept { return req_; }
  /// @brief Metode request.
  Method method() const noexcept { return req_.method; }
  /// @brief Path tanpa query.
  std::string_view path() const noexcept { return req_.path; }
  /// @brief Body mentah (view pinjaman).
  std::string_view body() const noexcept { return req_.body; }
  /// @brief IP klien.
  std::string_view client_ip() const noexcept { return client_ip_; }

  // /users/:id -> ctx.param("id")
  /// @brief Nilai parameter route.
  /// @param name Nama tanpa ':', mis. "id" untuk "/users/:id".
  /// @param fallback Dikembalikan bila tidak ada.
  std::string_view param(std::string_view name,
                         std::string_view fallback = {}) const noexcept {
    auto v = params_.get(name);
    return v ? *v : fallback;
  }

  // ?q=hello&n=2 -> ctx.query("q")
  // Percent-decoding is NOT done in MVP (returns raw slice); documented.
  /// @brief Nilai query string.
  /// @param key Kunci query.
  /// @return View nilai atau nullopt.
  /// @note Percent-decoding BELUM dilakukan (kembalikan slice mentah).
  std::optional<std::string_view> query(std::string_view key) const noexcept {
    std::string_view q = req_.query;
    while (!q.empty()) {
      std::size_t amp = q.find('&');
      std::string_view pair = (amp == std::string_view::npos) ? q : q.substr(0, amp);
      std::size_t eq = pair.find('=');
      std::string_view k = (eq == std::string_view::npos) ? pair : pair.substr(0, eq);
      std::string_view v = (eq == std::string_view::npos) ? std::string_view{} : pair.substr(eq + 1);
      if (k == key) return v;
      if (amp == std::string_view::npos) break;
      q = q.substr(amp + 1);
    }
    return std::nullopt;
  }

  /// @brief Query dengan default.
  std::string_view query_or(std::string_view key, std::string_view fallback = {}) const noexcept {
    auto v = query(key);
    return v ? *v : fallback;
  }

  /// @brief Nilai header (case-insensitive) atau nullopt.
  std::optional<std::string_view> header(std::string_view name) const noexcept {
    return req_.header(name);
  }

  /// @brief Header dengan default.
  std::string_view header_or(std::string_view name, std::string_view fallback = {}) const noexcept {
    auto v = req_.header(name);
    return v ? *v : fallback;
  }

  // ---- REST helpers (same zero-alloc view style) ----

  // Parse request body as JSON. Empty/invalid body -> nullopt.
  //   auto v = ctx.json();
  //   if (!v) return Response::bad_request("invalid json");
  /// @brief Parse body sebagai JSON.
  /// @return Nilai JSON atau nullopt bila body kosong/invalid.
  std::optional<json::Value> json() const {
    if (req_.body.empty()) return std::nullopt;
    return json::parse(req_.body);
  }

  /// @brief Content-Type tanpa parameter ("; charset=..."), sudah di-trim.
  std::string_view content_type() const noexcept {
    auto v = req_.header("content-type");
    if (!v) return {};
    std::string_view ct = *v;
    if (auto semi = ct.find(';'); semi != std::string_view::npos)
      ct = ct.substr(0, semi);
    // trim trailing spaces before ';'
    while (!ct.empty() && (ct.back() == ' ' || ct.back() == '\t')) ct.remove_suffix(1);
    return ct;
  }

  /// @brief true bila Content-Type JSON.
  bool is_json() const noexcept {
    std::string_view ct = content_type();
    return ct == "application/json" || ct == "application/problem+json";
  }

  // Typed route param / query access without exceptions.
  //   int page = ctx.query_int("page", 1);
  /// @brief Parameter route sebagai integer (tanpa exception).
  /// @return Nilai atau fallback bila tidak ada/invalid.
  long long param_int(std::string_view name, long long fallback = 0) const noexcept {
    return parse_int(param(name), fallback);
  }
  /// @brief Query sebagai integer (tanpa exception).
  long long query_int(std::string_view key, long long fallback = 0) const noexcept {
    auto v = query(key);
    return parse_int(v ? *v : std::string_view{}, fallback);
  }
  /// @brief Query sebagai double (tanpa exception).
  double query_double(std::string_view key, double fallback = 0.0) const noexcept {
    auto v = query(key);
    if (!v || v->empty()) return fallback;
    // from_chars for double is available on GCC 11+; fallback keeps it simple.
    double d = fallback;
    auto [ptr, ec] = std::from_chars(v->data(), v->data() + v->size(), d);
    if (ec != std::errc{} || ptr != v->data() + v->size()) return fallback;
    return d;
  }

  // "Authorization: Bearer <token>" -> token view, or nullopt.
  /// @brief Token dari header "Authorization: Bearer ...".
  /// @return View token atau nullopt bila tidak ada/format salah.
  std::optional<std::string_view> bearer_token() const noexcept {
    auto h = req_.header("authorization");
    if (!h) return std::nullopt;
    std::string_view v = *h;
    constexpr std::string_view k = "Bearer ";
    if (v.size() <= k.size() || v.substr(0, k.size()) != k) return std::nullopt;
    std::string_view tok = v.substr(k.size());
    while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t')) tok.remove_prefix(1);
    if (tok.empty()) return std::nullopt;
    return tok;
  }

  // "Cookie: a=1; b=2" -> value view for name, or nullopt.
  /// @brief Nilai cookie berdasar nama.
  /// @return View nilai (trim) atau nullopt.
  std::optional<std::string_view> cookie(std::string_view name) const noexcept {
    auto h = req_.header("cookie");
    if (!h) return std::nullopt;
    std::string_view c = *h;
    while (!c.empty()) {
      while (!c.empty() && (c.front() == ' ' || c.front() == '\t' || c.front() == ';'))
        c.remove_prefix(1);
      if (c.empty()) break;
      std::size_t semi = c.find(';');
      std::string_view pair = (semi == std::string_view::npos) ? c : c.substr(0, semi);
      std::size_t eq = pair.find('=');
      std::string_view k = (eq == std::string_view::npos) ? pair : pair.substr(0, eq);
      while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.remove_suffix(1);
      if (k == name) {
        std::string_view val =
            (eq == std::string_view::npos) ? std::string_view{} : pair.substr(eq + 1);
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.remove_prefix(1);
        while (!val.empty() && (val.back() == ' ' || val.back() == '\t')) val.remove_suffix(1);
        return val;
      }
      if (semi == std::string_view::npos) break;
      c = c.substr(semi + 1);
    }
    return std::nullopt;
  }

 private:
  /// @brief Parse integer tanpa exception (fallback bila gagal).
  static long long parse_int(std::string_view s, long long fallback) noexcept {
    if (s.empty()) return fallback;
    long long v = fallback;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || ptr != s.data() + s.size()) return fallback;
    return v;
  }

 private:
  const Request& req_;       ///< Request pinjaman.
  ParamMap& params_;         ///< Param hasil match (referensi).
  std::string_view client_ip_;  ///< IP klien.
};

// Handler returns Response by value (move). MVP uses std::function for
// simplicity; hot-path type-erasure optimization is post-MVP (see ADR).
// Defined here (next to Context) so router.hpp and middleware.hpp share one
// canonical alias without a dependency cycle.
/// @brief Tipe handler: fungsi Response(Context&).
///
/// Semua route handler memakai tanda tangan ini. Definisi di sini agar
/// router.hpp dan middleware.hpp berbagi satu alias tanpa siklus dependensi.
/// @note Jalan cepat tanpa middleware memanggil handler langsung (tanpa
///   overhead std::function tambahan di sisi framework).
using Handler = std::function<Response(Context&)>;

}  // namespace elaina

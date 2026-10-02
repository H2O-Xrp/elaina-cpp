/// @file http.hpp
/// @brief Tipe inti HTTP: HeaderView, Request (pinjam), Response (milik).
///
/// Model kepemilikan:
/// - Request berisi string_view yang meminjam buffer terima koneksi.
///   Hanya valid selama dispatch() handler berjalan — jangan disimpan.
/// - Response dimiliki penuh (owned) dan di-move keluar handler,
///   lalu di-encode ke buffer kirim.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "json/json.hpp"
#include "method.hpp"

namespace elaina {

/// @brief Satu header HTTP sebagai view pinjaman.
///
/// @warning Meminjam buffer recv koneksi. Hanya valid selama dispatch().
///   Jangan disimpan melewati return handler.
struct HeaderView {
  std::string_view name;   ///< Nama field, mis. "Content-Type".
  std::string_view value;  ///< Nilai field (sudah di-trim).
};

/// @brief Request yang sudah di-parse: head + body sebagai view pinjaman.
///
/// Semua view meminjam buffer recv Connection. Lihat Context untuk akses
/// yang nyaman (param/query/header/body).
struct Request {
  Method method = Method::GET;  ///< Metode hasil parse.
  std::string_view method_raw;  ///< Teks metode mentah, mis. "GET".
  std::string_view target;      ///< Target mentah, mis. "/users/42?x=1".
  std::string_view path;        ///< Path tanpa query, mis. "/users/42".
  std::string_view query;       ///< Query tanpa '?', bisa kosong.
  std::string_view version;     ///< Versi, mis. "HTTP/1.1".
  std::vector<HeaderView> headers;  ///< Header (kapasitas dipakai ulang per koneksi).
  std::string_view body;        ///< Body pinjaman, bisa kosong.
  bool keep_alive = true;       ///< true bila koneksi dipertahankan.

  /// @brief Cari header tanpa peduli kapital (case-insensitive).
  /// @param name Nama header, mis. "content-type".
  /// @return Nilai header atau nullopt bila tidak ada.
  /// @note Linear O(H); cukup untuk H kecil (batas max_headers).
  std::optional<std::string_view> header(std::string_view name) const noexcept {
    for (auto& h : headers) {
      if (h.name.size() == name.size()) {
        bool eq = true;
        for (std::size_t i = 0; i < name.size(); ++i) {
          char a = h.name[i];
          char b = name[i];
          if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + 32);
          if (b >= 'A' && b <= 'Z') b = static_cast<char>(b + 32);
          if (a != b) { eq = false; break; }
        }
        if (eq) return h.value;
      }
    }
    return std::nullopt;
  }
};

// Owned HTTP response produced by handler (move-only for clarity, but
// copyable for std::function friendliness in MVP).
/// @brief Respons HTTP milik penuh yang dibuat handler.
///
/// Dibuat lewat factory (text/html/json/...) lalu opsional di-chaining:
/// @code
///   return elaina::Response::json(elaina::json::obj({{"id", 1}}))
///       .with_header("X-Trace", "1");
/// @endcode
struct Response {
  Status status = Status::Ok;  ///< Kode status.
  std::vector<std::pair<std::string, std::string>> headers;  ///< Header tambahan.
  std::string body;            ///< Body milik penuh.
  std::string content_type = "text/plain; charset=utf-8";  ///< Content-Type.

  /// @brief Konstruktor default: 200 text kosong.
  Response() = default;
  /// @brief Konstruktor status + body.
  /// @param s Status. @param b Body (di-move).
  Response(Status s, std::string b) : status(s), body(std::move(b)) {}

  /// @brief Ubah status (chaining).
  Response& with_status(Status s) {
    status = s;
    return *this;
  }
  /// @brief Tambah header (chaining).
  Response& with_header(std::string k, std::string v) {
    headers.emplace_back(std::move(k), std::move(v));
    return *this;
  }
  /// @brief Ubah Content-Type (chaining).
  Response& with_content_type(std::string ct) {
    content_type = std::move(ct);
    return *this;
  }

  /// @brief Respons teks polos.
  static Response text(std::string_view s, Status st = Status::Ok) {
    Response r;
    r.status = st;
    r.body.assign(s.data(), s.size());
    r.content_type = "text/plain; charset=utf-8";
    return r;
  }
  /// @brief Respons HTML.
  static Response html(std::string_view s, Status st = Status::Ok) {
    Response r;
    r.status = st;
    r.body.assign(s.data(), s.size());
    r.content_type = "text/html; charset=utf-8";
    return r;
  }
  /// @brief Respons JSON dari string yang sudah valid.
  static Response json(std::string_view s, Status st = Status::Ok) {
    Response r;
    r.status = st;
    r.body.assign(s.data(), s.size());
    r.content_type = "application/json";
    return r;
  }
  // String overloads first (exact match beats the Value conversion below).
  /// @brief Respons JSON dari std::string (cocok eksak, hindari ambiguitas Value).
  static Response json(const std::string& s, Status st = Status::Ok) {
    return json(std::string_view(s), st);
  }
  /// @brief Respons JSON dari literal C-string.
  static Response json(const char* s, Status st = Status::Ok) {
    return json(std::string_view(s), st);
  }
  // REST convenience: serialize directly, no manual stringify().
  //   return elaina::Response::json(elaina::json::obj({{"id", 1}}));
  /// @brief Respons JSON langsung dari nilai (tanpa stringify manual).
  static Response json(const json::Value& v, Status st = Status::Ok) {
    return json(json::stringify(v), st);
  }
  /// @brief Respons JSON langsung dari objek.
  static Response json(const json::Object& o, Status st = Status::Ok) {
    return json(json::Value(o), st);
  }
  /// @brief Respons JSON langsung dari array.
  static Response json(const json::Array& a, Status st = Status::Ok) {
    return json(json::Value(a), st);
  }
  /// @brief Respons tanpa body, hanya status (mis. 204/304).
  static Response status_only(Status st) {
    Response r;
    r.status = st;
    return r;
  }

  // ---- REST status shortcuts (same call style as text/json) ----
  /// @brief 201 Created teks.
  static Response created(std::string_view s, std::string_view ct = "text/plain; charset=utf-8") {
    Response r;
    r.status = Status::Created;
    r.body.assign(s.data(), s.size());
    r.content_type = std::string(ct);
    return r;
  }
  /// @brief 201 Created JSON.
  static Response json_created(const json::Value& v) { return json(v, Status::Created); }
  /// @brief 204 No Content.
  static Response no_content() { return status_only(Status::NoContent); }
  /// @brief 400 Bad Request (validasi/JSON gagal).
  static Response bad_request(std::string_view msg = "Bad Request") {
    return text(msg, Status::BadRequest);
  }
  /// @brief 401 Unauthorized.
  static Response unauthorized(std::string_view msg = "Unauthorized") {
    return text(msg, Status::Unauthorized);
  }
  /// @brief 403 Forbidden.
  static Response forbidden(std::string_view msg = "Forbidden") {
    return text(msg, Status::Forbidden);
  }
  /// @brief 404 Not Found.
  static Response not_found(std::string_view msg = "Not Found") {
    return text(msg, Status::NotFound);
  }
  /// @brief 500 Internal Server Error.
  static Response internal(std::string_view msg = "Internal Server Error") {
    return text(msg, Status::InternalError);
  }
  /// @brief Redirect (default 302 Found) dengan header Location.
  /// @param url Tujuan. @param st Status 3xx (Found/SeeOther/MovedPermanently).
  static Response redirect(std::string_view url, Status st = Status::Found) {
    Response r = status_only(st);
    r.with_header("Location", std::string(url));
    return r;
  }

  /// @brief Tambah cookie via header Set-Cookie (bisa dipanggil berulang).
  /// @param cookie Nilai cookie lengkap, mis. "sess=abc; Path=/; HttpOnly".
  Response& with_cookie(std::string cookie) {
    headers.emplace_back("Set-Cookie", std::move(cookie));
    return *this;
  }
};

}  // namespace elaina

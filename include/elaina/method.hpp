/// @file method.hpp
/// @brief Metode HTTP dan kode status HTTP.
///
/// Berisi dua enum inti framework:
/// - elaina::Method — metode request yang didukung router.
/// - elaina::Status — kode status respons beserta teks alasan (reason phrase).
#pragma once

#include <string_view>

namespace elaina {

/// @brief Metode HTTP yang didukung router.
///
/// @note Hanya metode di sini yang bisa di-routing. Metode lain
///   ditolak parser dengan 400/501.
enum class Method : unsigned char {
  GET,     ///< Ambil representasi resource.
  POST,    ///< Buat resource / kirim body untuk diproses.
  PUT,     ///< Ganti seluruh resource.
  PATCH,   ///< Ubah sebagian resource.
  DELETE,  ///< Hapus resource.
  HEAD,    ///< Seperti GET tanpa body respons.
  OPTIONS, ///< Preflight CORS / capability check.
};

/// @brief Nama kanonis metode (mis. "GET").
///
/// @param m Metode yang ditanya.
/// @return string_view statis, selalu valid.
inline std::string_view method_name(Method m) noexcept {
  switch (m) {
    case Method::GET: return "GET";
    case Method::POST: return "POST";
    case Method::PUT: return "PUT";
    case Method::PATCH: return "PATCH";
    case Method::DELETE: return "DELETE";
    case Method::HEAD: return "HEAD";
    case Method::OPTIONS: return "OPTIONS";
  }
  return "GET";
}

/// @brief Parse nama metode dari request line.
///
/// @param s Potongan metode, mis. "GET".
/// @param[out] out Diisi bila nama dikenal.
/// @return true bila dikenal, false bila tidak (pemanggil merespons 400/501).
inline bool parse_method(std::string_view s, Method& out) noexcept {
  if (s == "GET") { out = Method::GET; return true; }
  if (s == "POST") { out = Method::POST; return true; }
  if (s == "PUT") { out = Method::PUT; return true; }
  if (s == "PATCH") { out = Method::PATCH; return true; }
  if (s == "DELETE") { out = Method::DELETE; return true; }
  if (s == "HEAD") { out = Method::HEAD; return true; }
  if (s == "OPTIONS") { out = Method::OPTIONS; return true; }
  return false;
}

/// @brief Jumlah metode (ukuran array per-metode di Router).
inline constexpr std::size_t kMethodCount = 7;

/// @brief Indeks array untuk sebuah metode (0 .. kMethodCount-1).
/// @param m Metode.
/// @return Indeks yang cocok untuk roots_[index] di Router.
inline std::size_t method_index(Method m) noexcept {
  return static_cast<std::size_t>(m);
}

// HTTP status codes used by framework.
/// @brief Kode status HTTP yang dipakai framework.
///
/// Mencakup 2xx sukses, 3xx redirect, 4xx kesalahan klien,
/// dan 5xx kesalahan server yang umum dipakai REST.
enum class Status : int {
  Ok = 200,              ///< Sukses (default).
  Created = 201,         ///< Resource berhasil dibuat.
  Accepted = 202,        ///< Diterima untuk diproses.
  NoContent = 204,       ///< Sukses tanpa body (mis. DELETE).
  MovedPermanently = 301,///< Pindah permanen (lihat Location).
  Found = 302,           ///< Redirect sementara (default redirect()).
  SeeOther = 303,        ///< Lihat resource lain.
  NotModified = 304,     ///< Tidak berubah (cache).
  BadRequest = 400,      ///< Request malformed / validasi gagal.
  Unauthorized = 401,    ///< Butuh autentikasi.
  Forbidden = 403,       ///< Terautentikasi tapi tidak boleh.
  NotFound = 404,        ///< Path tidak ada.
  MethodNotAllowed = 405,///< Path ada, metode salah.
  RequestTimeout = 408,  ///< Header/body terlalu lama datang.
  TooManyRequests = 429, ///< Rate limit terlampaui (lihat Retry-After).
  PayloadTooLarge = 413, ///< Body melebihi max_body_size.
  HeaderTooLarge = 431,  ///< Header melebihi batas.
  InternalError = 500,   ///< Exception / kesalahan server.
  NotImplemented = 501,  ///< Fitur belum didukung (mis. chunked).
};

/// @brief Teks alasan (reason phrase) untuk status, mis. "Not Found".
/// @param s Status.
/// @return string_view statis, selalu valid.
inline std::string_view status_reason(Status s) noexcept {
  switch (s) {
    case Status::Ok: return "OK";
    case Status::Created: return "Created";
    case Status::Accepted: return "Accepted";
    case Status::NoContent: return "No Content";
    case Status::MovedPermanently: return "Moved Permanently";
    case Status::Found: return "Found";
    case Status::SeeOther: return "See Other";
    case Status::NotModified: return "Not Modified";
    case Status::BadRequest: return "Bad Request";
    case Status::Unauthorized: return "Unauthorized";
    case Status::Forbidden: return "Forbidden";
    case Status::NotFound: return "Not Found";
    case Status::MethodNotAllowed: return "Method Not Allowed";
    case Status::RequestTimeout: return "Request Timeout";
    case Status::TooManyRequests: return "Too Many Requests";
    case Status::PayloadTooLarge: return "Payload Too Large";
    case Status::HeaderTooLarge: return "Request Header Fields Too Large";
    case Status::InternalError: return "Internal Server Error";
    case Status::NotImplemented: return "Not Implemented";
  }
  return "Unknown";
}

/// @brief Angka kode status, mis. 404.
/// @param s Status.
/// @return Kode numerik.
inline int status_code(Status s) noexcept { return static_cast<int>(s); }

}  // namespace elaina

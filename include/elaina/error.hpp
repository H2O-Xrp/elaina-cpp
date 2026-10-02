/// @file error.hpp
/// @brief Model error framework: kategori + shim expected<T,E>.
///
/// Prinsip: hot path (parse/route/serve) tidak boleh throw.
/// Fungsi yang bisa gagal mengembalikan Expected<T, HttpError> ala
/// std::expected — memakai std::expected asli bila tersedia (C++23),
/// atau shim kecil yang kompatibel untuk C++20.
#pragma once

#include <string>
#include <string_view>
#include <variant>

#if defined(__cpp_lib_expected)
#include <expected>
#endif

#include "method.hpp"

namespace elaina {

/// @brief Kategori error framework.
///
/// Dipakai bersama Status HTTP di HttpError agar pemanggil bisa
/// membedakan penyebab tanpa parsing string.
enum class ErrorKind {
  None,            ///< Tidak ada error.
  Parse,           ///< HTTP malformed.
  RouteNotFound,   ///< Tidak ada route (404).
  MethodNotAllowed,///< Path ada, metode salah (405).
  PayloadTooLarge, ///< Body kebesaran (413).
  HeaderTooLarge,  ///< Header kebesaran (431).
  Timeout,         ///< Header/body/idle timeout.
  Network,         ///< I/O socket gagal.
  App,             ///< Aplikasi mengembalikan status error.
  Cancelled,       ///< Operasi dibatalkan.
};

/// @brief Error HTTP terstruktur: kategori + status + pesan.
///
/// Dibuat lewat factory (not_found(), bad_request(), ...) agar konsisten.
struct HttpError {
  ErrorKind kind = ErrorKind::None;        ///< Kategori.
  Status status = Status::InternalError;   ///< Status HTTP yang cocok.
  std::string message;                     ///< Pesan untuk log/body.

  /// @brief 404 Not Found.
  static HttpError not_found(std::string msg = "Not Found") {
    return {ErrorKind::RouteNotFound, Status::NotFound, std::move(msg)};
  }
  /// @brief 400 Bad Request (parse/validasi gagal).
  static HttpError bad_request(std::string msg = "Bad Request") {
    return {ErrorKind::Parse, Status::BadRequest, std::move(msg)};
  }
  /// @brief 413 Payload Too Large.
  static HttpError payload_too_large(std::string msg = "Payload Too Large") {
    return {ErrorKind::PayloadTooLarge, Status::PayloadTooLarge, std::move(msg)};
  }
  /// @brief 431 Header Too Large.
  static HttpError header_too_large(std::string msg = "Header Too Large") {
    return {ErrorKind::HeaderTooLarge, Status::HeaderTooLarge, std::move(msg)};
  }
  /// @brief 408 Request Timeout.
  static HttpError timeout(std::string msg = "Request Timeout") {
    return {ErrorKind::Timeout, Status::RequestTimeout, std::move(msg)};
  }
  /// @brief Error level aplikasi dengan status bebas.
  static HttpError app(Status s, std::string msg) {
    return {ErrorKind::App, s, std::move(msg)};
  }
};

// Minimal expected<T,E> shim: uses std::expected when available (C++23),
// otherwise a tiny compatible subset for C++20.
#if defined(__cpp_lib_expected)
/// @brief Hasil yang bisa sukses (T) atau gagal (E). Alias std::expected bila ada.
template <class T, class E>
using Expected = std::expected<T, E>;
#else
/// @brief Hasil yang bisa sukses (T) atau gagal (E). Shim C++20.
///
/// Subset API yang kompatibel dengan std::expected:
/// has_value()/operator bool, value(), error(), operator*/->.
/// Dibuat via Expected::ok(v) atau Expected::fail(e).
template <class T, class E>
class Expected {
 public:
  /// @brief Konstruksi sukses implisit dari nilai.
  Expected(T v) : data_(std::move(v)), ok_(true) {}
  /// @brief Konstruksi gagal (dipakai make_unexpected).
  Expected(E e, bool) : err_(std::move(e)), ok_(false) {}

  /// @brief Bungkus nilai sukses.
  static Expected ok(T v) { return Expected(std::move(v)); }
  /// @brief Bungkus error.
  static Expected fail(E e) {
    Expected r;
    r.data_ = T{};
    r.err_ = std::move(e);
    r.ok_ = false;
    return r;
  }

  /// @brief true bila berisi nilai.
  bool has_value() const noexcept { return ok_; }
  /// @brief true bila berisi nilai.
  explicit operator bool() const noexcept { return ok_; }
  /// @brief Akses nilai (hanya bila has_value()).
  T& value() & { return data_; }
  /// @brief Akses nilai (hanya bila has_value()).
  const T& value() const& { return data_; }
  /// @brief Akses error (hanya bila !has_value()).
  E& error() & { return err_; }
  /// @brief Akses error (hanya bila !has_value()).
  const E& error() const& { return err_; }
  /// @brief Akses nilai.
  T& operator*() & { return data_; }
  /// @brief Akses nilai.
  const T& operator*() const& { return data_; }
  /// @brief Akses member nilai.
  T* operator->() { return &data_; }
  /// @brief Akses member nilai.
  const T* operator->() const { return &data_; }

 private:
  Expected() = default;
  T data_{};
  E err_{};
  bool ok_ = true;
};

// Helper to construct unexpected without <expected> header.
/// @brief Tag pembungkus error untuk make_unexpected().
template <class E>
struct FailTag {
  E err;  ///< Error yang dibungkus.
};
/// @brief Buat FailTag dari error.
/// @param e Error.
/// @return Tag pembungkus.
template <class E>
FailTag<E> fail(E e) {
  return FailTag<E>{std::move(e)};
}
/// @brief Buat Expected gagal dari error.
/// @param e Error.
/// @return Expected dalam keadaan gagal.
template <class T, class E>
Expected<T, E> make_unexpected(E e) {
  return Expected<T, E>::fail(std::move(e));
}
#endif

}  // namespace elaina

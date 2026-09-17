#pragma once

#include <string>
#include <string_view>
#include <variant>

#if defined(__cpp_lib_expected)
#include <expected>
#endif

#include "method.hpp"

namespace elaina {

// Framework error categories (MVP hybrid model: expected-style, no throw on hot path).
enum class ErrorKind {
  None,
  Parse,        // malformed HTTP
  RouteNotFound,
  MethodNotAllowed,
  PayloadTooLarge,
  HeaderTooLarge,
  Timeout,
  Network,
  App,          // application returned error status
  Cancelled,
};

struct HttpError {
  ErrorKind kind = ErrorKind::None;
  Status status = Status::InternalError;
  std::string message;

  static HttpError not_found(std::string msg = "Not Found") {
    return {ErrorKind::RouteNotFound, Status::NotFound, std::move(msg)};
  }
  static HttpError bad_request(std::string msg = "Bad Request") {
    return {ErrorKind::Parse, Status::BadRequest, std::move(msg)};
  }
  static HttpError payload_too_large(std::string msg = "Payload Too Large") {
    return {ErrorKind::PayloadTooLarge, Status::PayloadTooLarge, std::move(msg)};
  }
  static HttpError header_too_large(std::string msg = "Header Too Large") {
    return {ErrorKind::HeaderTooLarge, Status::HeaderTooLarge, std::move(msg)};
  }
  static HttpError timeout(std::string msg = "Request Timeout") {
    return {ErrorKind::Timeout, Status::RequestTimeout, std::move(msg)};
  }
  static HttpError app(Status s, std::string msg) {
    return {ErrorKind::App, s, std::move(msg)};
  }
};

// Minimal expected<T,E> shim: uses std::expected when available (C++23),
// otherwise a tiny compatible subset for C++20.
#if defined(__cpp_lib_expected)
template <class T, class E>
using Expected = std::expected<T, E>;
#else
template <class T, class E>
class Expected {
 public:
  Expected(T v) : data_(std::move(v)), ok_(true) {}
  Expected(E e, bool) : err_(std::move(e)), ok_(false) {}

  static Expected ok(T v) { return Expected(std::move(v)); }
  static Expected fail(E e) {
    Expected r;
    r.data_ = T{};
    r.err_ = std::move(e);
    r.ok_ = false;
    return r;
  }

  bool has_value() const noexcept { return ok_; }
  explicit operator bool() const noexcept { return ok_; }
  T& value() & { return data_; }
  const T& value() const& { return data_; }
  E& error() & { return err_; }
  const E& error() const& { return err_; }
  T& operator*() & { return data_; }
  const T& operator*() const& { return data_; }
  T* operator->() { return &data_; }
  const T* operator->() const { return &data_; }

 private:
  Expected() = default;
  T data_{};
  E err_{};
  bool ok_ = true;
};

// Helper to construct unexpected without <expected> header.
template <class E>
struct FailTag {
  E err;
};
template <class E>
FailTag<E> fail(E e) {
  return FailTag<E>{std::move(e)};
}
template <class T, class E>
Expected<T, E> make_unexpected(E e) {
  return Expected<T, E>::fail(std::move(e));
}
#endif

}  // namespace elaina

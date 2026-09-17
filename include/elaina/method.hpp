#pragma once

#include <string_view>

namespace elaina {

// HTTP methods supported by MVP router.
enum class Method : unsigned char {
  GET,
  POST,
  PUT,
  PATCH,
  DELETE,
  HEAD,
  OPTIONS,
};

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

inline constexpr std::size_t kMethodCount = 7;

inline std::size_t method_index(Method m) noexcept {
  return static_cast<std::size_t>(m);
}

// HTTP status codes used by framework.
enum class Status : int {
  Ok = 200,
  Created = 201,
  NoContent = 204,
  BadRequest = 400,
  Unauthorized = 401,
  Forbidden = 403,
  NotFound = 404,
  MethodNotAllowed = 405,
  RequestTimeout = 408,
  PayloadTooLarge = 413,
  HeaderTooLarge = 431,
  InternalError = 500,
  NotImplemented = 501,
};

inline std::string_view status_reason(Status s) noexcept {
  switch (s) {
    case Status::Ok: return "OK";
    case Status::Created: return "Created";
    case Status::NoContent: return "No Content";
    case Status::BadRequest: return "Bad Request";
    case Status::Unauthorized: return "Unauthorized";
    case Status::Forbidden: return "Forbidden";
    case Status::NotFound: return "Not Found";
    case Status::MethodNotAllowed: return "Method Not Allowed";
    case Status::RequestTimeout: return "Request Timeout";
    case Status::PayloadTooLarge: return "Payload Too Large";
    case Status::HeaderTooLarge: return "Request Header Fields Too Large";
    case Status::InternalError: return "Internal Server Error";
    case Status::NotImplemented: return "Not Implemented";
  }
  return "Unknown";
}

inline int status_code(Status s) noexcept { return static_cast<int>(s); }

}  // namespace elaina

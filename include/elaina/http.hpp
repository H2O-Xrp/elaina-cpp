#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "method.hpp"

namespace elaina {

// A single HTTP header as borrowed views into the connection recv buffer.
// Valid only for the duration of dispatch(). Do not store.
struct HeaderView {
  std::string_view name;
  std::string_view value;
};

// Parsed request head + body. Views borrow from Connection recv buffer.
struct Request {
  Method method = Method::GET;
  std::string_view method_raw;
  std::string_view target;   // raw target e.g. /users/42?x=1
  std::string_view path;     // without query
  std::string_view query;    // without '?', may be empty
  std::string_view version;  // e.g. HTTP/1.1
  std::vector<HeaderView> headers;
  std::string_view body;     // may be empty; borrowed
  bool keep_alive = true;

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
struct Response {
  Status status = Status::Ok;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  std::string content_type = "text/plain; charset=utf-8";

  Response() = default;
  Response(Status s, std::string b) : status(s), body(std::move(b)) {}

  Response& with_status(Status s) {
    status = s;
    return *this;
  }
  Response& with_header(std::string k, std::string v) {
    headers.emplace_back(std::move(k), std::move(v));
    return *this;
  }
  Response& with_content_type(std::string ct) {
    content_type = std::move(ct);
    return *this;
  }

  static Response text(std::string_view s, Status st = Status::Ok) {
    Response r;
    r.status = st;
    r.body.assign(s.data(), s.size());
    r.content_type = "text/plain; charset=utf-8";
    return r;
  }
  static Response html(std::string_view s, Status st = Status::Ok) {
    Response r;
    r.status = st;
    r.body.assign(s.data(), s.size());
    r.content_type = "text/html; charset=utf-8";
    return r;
  }
  static Response json(std::string_view s, Status st = Status::Ok) {
    Response r;
    r.status = st;
    r.body.assign(s.data(), s.size());
    r.content_type = "application/json";
    return r;
  }
  static Response status_only(Status st) {
    Response r;
    r.status = st;
    return r;
  }
};

}  // namespace elaina

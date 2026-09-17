#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>

#include "http.hpp"

namespace elaina {

// Route params extracted without allocation: views into recv buffer / path.
// Capacity 8 is enough for MVP (e.g. /a/:b/c/:d). Overflow -> match fails.
struct ParamMap {
  static constexpr std::size_t kMax = 8;
  std::array<std::string_view, kMax> names{};
  std::array<std::string_view, kMax> values{};
  std::size_t size = 0;

  void clear() noexcept { size = 0; }
  bool add(std::string_view n, std::string_view v) noexcept {
    if (size >= kMax) return false;
    names[size] = n;
    values[size] = v;
    ++size;
    return true;
  }
  std::optional<std::string_view> get(std::string_view n) const noexcept {
    for (std::size_t i = 0; i < size; ++i) {
      if (names[i] == n) return values[i];
    }
    return std::nullopt;
  }
};

// Per-request facade. Borrowed; must not escape handler.
// Owns nothing except views + deadline info.
class Context {
 public:
  Context(const Request& req, ParamMap& params, std::string_view client_ip = {})
      : req_(req), params_(params), client_ip_(client_ip) {}

  const Request& request() const noexcept { return req_; }
  Method method() const noexcept { return req_.method; }
  std::string_view path() const noexcept { return req_.path; }
  std::string_view body() const noexcept { return req_.body; }
  std::string_view client_ip() const noexcept { return client_ip_; }

  // /users/:id -> ctx.param("id")
  std::string_view param(std::string_view name,
                         std::string_view fallback = {}) const noexcept {
    auto v = params_.get(name);
    return v ? *v : fallback;
  }

  // ?q=hello&n=2 -> ctx.query("q")
  // Percent-decoding is NOT done in MVP (returns raw slice); documented.
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

  std::string_view query_or(std::string_view key, std::string_view fallback = {}) const noexcept {
    auto v = query(key);
    return v ? *v : fallback;
  }

  std::optional<std::string_view> header(std::string_view name) const noexcept {
    return req_.header(name);
  }

  std::string_view header_or(std::string_view name, std::string_view fallback = {}) const noexcept {
    auto v = req_.header(name);
    return v ? *v : fallback;
  }

 private:
  const Request& req_;
  ParamMap& params_;
  std::string_view client_ip_;
};

// Handler returns Response by value (move). MVP uses std::function for
// simplicity; hot-path type-erasure optimization is post-MVP (see ADR).
// Defined here (next to Context) so router.hpp and middleware.hpp share one
// canonical alias without a dependency cycle.
using Handler = std::function<Response(Context&)>;

}  // namespace elaina

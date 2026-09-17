#pragma once

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "context.hpp"
#include "http.hpp"
#include "method.hpp"

namespace elaina {

class Context;
struct Request;

namespace detail {
// Compile-time handler check with a short error message (DX requirement).
template <class F>
concept HandlerFn = requires(F f, Context& c) {
  { f(c) } -> std::convertible_to<Response>;
};
}  // namespace detail

struct RouteMatch {
  const Handler* handler = nullptr;
  ParamMap params;
  bool method_mismatch = false;  // path exists but method differs -> 405
};

// Segment-trie (radix-per-segment) router.
// Split by '/': static children map, single :param child, single *wildcard.
// Priority: static > param > wildcard. Frozen implicitly after listen()
// (no mutation during serve => lock-free read per loop thread; Server
// guarantees registration happens-before listen).
class Router {
 public:
  Router() = default;
  Router(const Router&) = delete;
  Router& operator=(const Router&) = delete;

  // Returns false on conflicting registration (e.g. two different :names
  // at same position is allowed; duplicate identical route is rejected).
  bool add(Method method, std::string_view pattern, Handler handler);

  // Convenience for route groups: prefix + pattern.
  bool add_with_prefix(Method method, std::string_view prefix,
                       std::string_view pattern, Handler handler);

  RouteMatch match(Method method, std::string_view path) const noexcept;

  std::size_t route_count() const noexcept { return route_count_; }

 private:
  struct Node {
    std::unordered_map<std::string, std::unique_ptr<Node>> statics;
    std::unique_ptr<Node> param;
    std::string param_name;
    std::unique_ptr<Node> wildcard;  // '*' catch-all (must be last segment)
    const Handler* handler = nullptr;  // owned by handlers_ storage
  };

  static std::vector<std::string_view> split(std::string_view path) noexcept;
  static bool is_param(std::string_view seg) noexcept {
    return seg.size() >= 2 && seg[0] == ':';
  }
  static bool is_wildcard(std::string_view seg) noexcept { return seg == "*"; }

  bool insert_into(Node& root, std::string_view pattern, const Handler* hp);

  std::array<std::unique_ptr<Node>, kMethodCount> roots_;
  // Owns handler callables so Node pointers stay stable.
  std::vector<std::unique_ptr<Handler>> handlers_;
  std::size_t route_count_ = 0;
};

}  // namespace elaina

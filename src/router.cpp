#include "elaina/router.hpp"

namespace elaina {

std::vector<std::string_view> Router::split(std::string_view path) noexcept {
  std::vector<std::string_view> segs;
  if (path.empty() || path[0] != '/') return segs;
  std::size_t i = 1;
  // Special-case root "/" -> empty segment list.
  if (i >= path.size()) return segs;
  while (i <= path.size()) {
    std::size_t j = path.find('/', i);
    if (j == std::string_view::npos) j = path.size();
    segs.push_back(path.substr(i, j - i));
    i = j + 1;
  }
  return segs;
}

bool Router::add(Method method, std::string_view pattern, Handler handler) {
  if (pattern.empty() || pattern[0] != '/') return false;
  auto idx = method_index(method);
  if (!roots_[idx]) roots_[idx] = std::make_unique<Node>();
  auto hp = std::make_unique<Handler>(std::move(handler));
  const Handler* raw = hp.get();
  if (!insert_into(*roots_[idx], pattern, raw)) return false;
  handlers_.push_back(std::move(hp));
  ++route_count_;
  return true;
}

bool Router::add_with_prefix(Method method, std::string_view prefix,
                             std::string_view pattern, Handler handler) {
  std::string full;
  full.reserve(prefix.size() + pattern.size());
  full.append(prefix.data(), prefix.size());
  full.append(pattern.data(), pattern.size());
  return add(method, full, std::move(handler));
}

bool Router::insert_into(Node& root, std::string_view pattern, const Handler* hp) {
  auto segs = split(pattern);
  Node* cur = &root;
  for (std::size_t i = 0; i < segs.size(); ++i) {
    auto seg = segs[i];
    if (is_wildcard(seg)) {
      // Wildcard must be terminal.
      if (i + 1 != segs.size()) return false;
      if (!cur->wildcard) cur->wildcard = std::make_unique<Node>();
      cur = cur->wildcard.get();
    } else if (is_param(seg)) {
      std::string_view name = seg.substr(1);
      if (name.empty()) return false;
      if (!cur->param) {
        cur->param = std::make_unique<Node>();
        cur->param_name = std::string(name);
      }
      // Conflicting param names at same position: keep first (documented).
      cur = cur->param.get();
    } else {
      std::string key(seg);
      auto it = cur->statics.find(key);
      if (it == cur->statics.end()) {
        auto nn = std::make_unique<Node>();
        Node* raw = nn.get();
        cur->statics.emplace(std::move(key), std::move(nn));
        cur = raw;
      } else {
        cur = it->second.get();
      }
    }
  }
  if (cur->handler != nullptr) return false;  // duplicate
  cur->handler = hp;
  return true;
}

RouteMatch Router::match(Method method, std::string_view path) const noexcept {
  RouteMatch out;
  // Strip query if caller forgot.
  if (auto q = path.find('?'); q != std::string_view::npos) path = path.substr(0, q);

  auto try_match = [&](const Node* root) -> bool {
    if (!root) return false;
    auto segs = split(path);
    const Node* cur = root;
    ParamMap params;
    for (std::size_t i = 0; i < segs.size(); ++i) {
      auto seg = segs[i];
      // 1. static
      auto it = cur->statics.find(std::string(seg));
      if (it != cur->statics.end()) {
        cur = it->second.get();
        continue;
      }
      // 2. param
      if (cur->param) {
        if (!params.add(cur->param_name, seg)) return false;
        cur = cur->param.get();
        continue;
      }
      // 3. wildcard (terminal catch-all)
      if (cur->wildcard) {
        cur = cur->wildcard.get();
        // Wildcard consumes the rest.
        out.handler = cur->handler;
        out.params = params;
        return out.handler != nullptr;
      }
      return false;
    }
    // Exact consumption. Also allow trailing wildcard matching empty rest.
    if (cur->handler) {
      out.handler = cur->handler;
      out.params = params;
      return true;
    }
    if (cur->wildcard && cur->wildcard->handler) {
      out.handler = cur->wildcard->handler;
      out.params = params;
      return true;
    }
    return false;
  };

  auto idx = method_index(method);
  if (try_match(roots_[idx].get())) return out;

  // Check if path exists under another method -> 405.
  for (std::size_t m = 0; m < kMethodCount; ++m) {
    if (m == idx || !roots_[m]) continue;
    RouteMatch probe;
    // Reuse logic: temporarily match with that root.
    const Node* root = roots_[m].get();
    auto segs = split(path);
    const Node* cur = root;
    bool ok = true;
    for (auto seg : segs) {
      auto it = cur->statics.find(std::string(seg));
      if (it != cur->statics.end()) {
        cur = it->second.get();
        continue;
      }
      if (cur->param) {
        cur = cur->param.get();
        continue;
      }
      if (cur->wildcard) {
        cur = cur->wildcard.get();
        break;
      }
      ok = false;
      break;
    }
    if (ok && cur && (cur->handler || (cur->wildcard && cur->wildcard->handler))) {
      out.method_mismatch = true;
      return out;
    }
  }
  return out;
}

}  // namespace elaina

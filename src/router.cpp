/// @file router.cpp
/// @brief Implementasi Router segment-trie.
///
/// - split()/insert_into(): control-plane (registrasi, boleh alokasi).
/// - match(): data-plane nol-alokasi — iterasi segmen di tempat +
///   lookup hash transparan; probe 405 memakai iterator yang sama.
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

  // Zero-alloc segment iteration: yields each '/'-separated segment without
  // building a vector. Mirrors split() semantics (root "/" -> 0 segs,
  // trailing "/" -> trailing empty seg).
  auto match_root = [&](const Node* root, ParamMap* params_out) noexcept -> const Node* {
    if (!root) return nullptr;
    if (path.empty() || path[0] != '/') {
      // Same as split(): non-absolute -> empty segs -> root itself.
      return root;
    }
    if (path.size() == 1) return root;  // "/"
    const Node* cur = root;
    std::size_t pos = 1;
    while (pos <= path.size()) {
      std::size_t j = path.find('/', pos);
      if (j == std::string_view::npos) j = path.size();
      std::string_view seg = path.substr(pos, j - pos);
      // 1. static (heterogeneous lookup: no std::string temp).
      auto it = cur->statics.find(seg);
      if (it != cur->statics.end()) {
        cur = it->second.get();
        pos = j + 1;
        if (pos > path.size()) break;
        // If we consumed exactly up to end, loop ends; trailing "/" case
        // produces one more empty seg on next iteration (pos==size).
        if (j == path.size()) break;
        continue;
      }
      // 2. param
      if (cur->param) {
        if (params_out) {
          if (!params_out->add(cur->param_name, seg)) return nullptr;
        }
        cur = cur->param.get();
        pos = j + 1;
        if (j == path.size()) break;
        if (pos > path.size()) break;
        continue;
      }
      // 3. wildcard (terminal catch-all): consumes the rest.
      if (cur->wildcard) {
        return cur->wildcard.get();
      }
      return nullptr;
    }
    return cur;
  };

  auto idx = method_index(method);
  {
    ParamMap params;
    const Node* cur = match_root(roots_[idx].get(), &params);
    if (cur) {
      if (cur->handler) {
        out.handler = cur->handler;
        out.params = params;
        return out;
      }
      // Trailing wildcard matching empty rest.
      if (cur->wildcard && cur->wildcard->handler) {
        out.handler = cur->wildcard->handler;
        out.params = params;
        return out;
      }
      // Wildcard consumed rest: match_root returned wildcard node directly.
      // If that node has a handler, it's a hit (handles /static/a/b/c).
      // Note: match_root returns wildcard node when wildcard branch taken,
      // which may itself lack handler but have no further traversal — treat
      // same as above: only handler counts.
    } else {
      // match_root returned nullptr on param overflow or miss. But wildcard
      // consumption returns non-null, so nullptr here means miss. However
      // wildcard-mid-path case already returned wildcard node; check if that
      // path was actually a wildcard hit with handler — handled above.
      // Fall through to 405 probe.
    }
    // Distinguish wildcard-mid-path hit vs exact hit: if cur is non-null and
    // came from wildcard consumption, its handler check above already covers
    // it. No extra work needed.
    if (out.handler) return out;
    // If primary root matched a node with handler we returned. Otherwise probe 405.
    // Fast exit: if cur != nullptr and (handler or wildcard handler) we would
    // have returned, so reaching here means miss for this method.
  }

  // Check if path exists under another method -> 405.
  // Reuses zero-alloc iteration; no params needed.
  for (std::size_t m = 0; m < kMethodCount; ++m) {
    if (m == idx || !roots_[m]) continue;
    const Node* cur = match_root(roots_[m].get(), nullptr);
    if (cur && (cur->handler || (cur->wildcard && cur->wildcard->handler))) {
      out.method_mismatch = true;
      return out;
    }
  }
  return out;
}

}  // namespace elaina

/// @file middleware.cpp
/// @brief Implementasi middleware bawaan + util internal (namespace detail).
///
/// - Logger/Cors: observabilitas dasar.
/// - RequestId/Recovery: ketahanan operasional.
/// - RateLimit: fixed-window per IP (mutex + prune oportunistik).
/// - BasicAuth/BearerAuth/JwtHs256Auth: autentikasi (constant-time compare;
///   JWT memakai SHA-256/HMAC mandiri agar tanpa dep tambahan).
/// Util detail: b64_decode (basic+url), const_time_eq, sha256,
/// hmac_sha256, b64url_encode.
#include "elaina/middleware.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <unordered_map>
#include <variant>

namespace elaina {
namespace detail {

// ---- base64/base64url decode (no deps) ----

inline int b64_val(char c, bool url) noexcept {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == (url ? '-' : '+')) return 62;
  if (c == (url ? '_' : '/')) return 63;
  if (!url && c == '=') return -2;  // padding
  return -1;
}

// Returns false on invalid input. `url` selects base64url (no padding needed).
inline bool b64_decode(std::string_view in, std::string& out, bool url) {
  out.clear();
  out.reserve(in.size() * 3 / 4);
  int acc = 0, bits = 0;
  for (char c : in) {
    if (url && (c == '=')) break;  // tolerate padding
    int v = b64_val(c, url);
    if (v == -2) break;  // '=' padding: rest must be padding/end
    if (v < 0) return false;
    acc = (acc << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((acc >> bits) & 0xFF));
    }
  }
  return true;
}

inline bool const_time_eq(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  unsigned diff = 0;
  for (std::size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned>(a[i] ^ b[i]);
  return diff == 0;
}

// ---- SHA-256 (FIPS 180-4, self-contained so JwtAuth needs no extra dep) ----

inline std::array<std::uint32_t, 8> sha256(std::string_view msg) {
  static constexpr std::uint32_t K[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
      0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
      0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
      0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
      0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
      0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
      0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
      0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
      0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
  auto rotr = [](std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };

  std::uint64_t bitlen = static_cast<std::uint64_t>(msg.size()) * 8;
  std::string padded(msg.data(), msg.size());
  padded.push_back(static_cast<char>(0x80));
  while (padded.size() % 64 != 56) padded.push_back('\0');
  for (int i = 7; i >= 0; --i)
    padded.push_back(static_cast<char>((bitlen >> (i * 8)) & 0xFF));

  std::array<std::uint32_t, 8> h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  const auto* p = reinterpret_cast<const unsigned char*>(padded.data());
  for (std::size_t off = 0; off < padded.size(); off += 64) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
      w[i] = (static_cast<std::uint32_t>(p[off + i * 4]) << 24) |
             (static_cast<std::uint32_t>(p[off + i * 4 + 1]) << 16) |
             (static_cast<std::uint32_t>(p[off + i * 4 + 2]) << 8) |
             static_cast<std::uint32_t>(p[off + i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
      std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    auto [a, b, c, d, e, f, g, hh] = h;
    for (int i = 0; i < 64; ++i) {
      std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      std::uint32_t ch = (e & f) ^ (~e & g);
      std::uint32_t t1 = hh + S1 + ch + K[i] + w[i];
      std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      std::uint32_t t2 = S0 + maj;
      hh = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
  }
  return h;
}

inline std::string hmac_sha256(std::string_view key, std::string_view msg) {
  std::string k(key);
  if (k.size() > 64) {
    auto h = sha256(k);
    k.clear();
    for (auto w : h) {
      k.push_back(static_cast<char>((w >> 24) & 0xFF));
      k.push_back(static_cast<char>((w >> 16) & 0xFF));
      k.push_back(static_cast<char>((w >> 8) & 0xFF));
      k.push_back(static_cast<char>(w & 0xFF));
    }
  }
  k.resize(64, '\0');
  std::string okey = k, ikey = k;
  for (int i = 0; i < 64; ++i) {
    okey[i] ^= 0x5c;
    ikey[i] ^= 0x36;
  }
  std::string inner = ikey + std::string(msg);
  auto ih = sha256(inner);
  std::string ih_bytes;
  for (auto w : ih) {
    ih_bytes.push_back(static_cast<char>((w >> 24) & 0xFF));
    ih_bytes.push_back(static_cast<char>((w >> 16) & 0xFF));
    ih_bytes.push_back(static_cast<char>((w >> 8) & 0xFF));
    ih_bytes.push_back(static_cast<char>(w & 0xFF));
  }
  std::string outer = okey + ih_bytes;
  auto oh = sha256(outer);
  std::string out;
  for (auto w : oh) {
    out.push_back(static_cast<char>((w >> 24) & 0xFF));
    out.push_back(static_cast<char>((w >> 16) & 0xFF));
    out.push_back(static_cast<char>((w >> 8) & 0xFF));
    out.push_back(static_cast<char>(w & 0xFF));
  }
  return out;
}

// base64url (no pad) encode for signature comparison.
inline std::string b64url_encode(const std::string& raw) {
  static constexpr char k[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  out.reserve((raw.size() * 4 + 2) / 3);
  for (std::size_t i = 0; i < raw.size(); i += 3) {
    unsigned n = static_cast<unsigned char>(raw[i]) << 16;
    int rem = static_cast<int>(raw.size() - i);
    if (rem > 1) n |= static_cast<unsigned char>(raw[i + 1]) << 8;
    if (rem > 2) n |= static_cast<unsigned char>(raw[i + 2]);
    out.push_back(k[(n >> 18) & 63]);
    out.push_back(k[(n >> 12) & 63]);
    if (rem > 1) out.push_back(k[(n >> 6) & 63]);
    if (rem > 2) out.push_back(k[n & 63]);
  }
  return out;
}

}  // namespace detail

Middleware Logger() {
  return [](Context& ctx, Next next) -> Response {
    auto t0 = std::chrono::steady_clock::now();
    Response res = next(ctx);
    auto t1 = std::chrono::steady_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    std::string_view path = ctx.path();
    std::fprintf(stderr, "%s %.*s -> %d (%lld us)\n",
                 std::string(method_name(ctx.method())).c_str(),
                 (int)path.size(), path.data(), status_code(res.status),
                 (long long)us);
    return res;
  };
}

Middleware Cors(std::string allow_origin) {
  return [origin = std::move(allow_origin)](Context& ctx, Next next) -> Response {
    if (ctx.method() == Method::OPTIONS) {
      Response r = Response::status_only(Status::NoContent);
      r.with_header("Access-Control-Allow-Origin", origin)
          .with_header("Access-Control-Allow-Methods", "GET, POST, PUT, PATCH, DELETE, OPTIONS")
          .with_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
      return r;
    }
    Response res = next(ctx);
    res.with_header("Access-Control-Allow-Origin", origin);
    return res;
  };
}

Middleware RequestId(std::string header) {
  static std::atomic<std::uint64_t> seq{0};
  return [h = std::move(header)](Context& ctx, Next next) -> Response {
    std::string id;
    auto incoming = ctx.header(h);
    if (incoming && !incoming->empty()) {
      id.assign(incoming->data(), incoming->size());
    } else {
      auto now = std::chrono::steady_clock::now().time_since_epoch().count();
      std::uint64_t n = seq.fetch_add(1, std::memory_order_relaxed);
      char buf[48];
      std::snprintf(buf, sizeof(buf), "rid-%llx-%llx", (unsigned long long)now,
                    (unsigned long long)n);
      id = buf;
    }
    Response res = next(ctx);
    res.with_header(h, id);
    return res;
  };
}

Middleware Recovery() {
  return [](Context& ctx, Next next) -> Response {
    try {
      return next(ctx);
    } catch (const std::exception& e) {
      std::string msg = "Internal Server Error: ";
      msg += e.what();
      return Response::internal(msg);
    } catch (...) {
      return Response::internal();
    }
  };
}

Middleware RateLimit(int max_requests, std::chrono::seconds window) {
  struct Entry {
    int count = 0;
    std::chrono::steady_clock::time_point start{};
  };
  struct State {
    std::mutex mu;
    std::unordered_map<std::string, Entry> hits;
  };
  auto st = std::make_shared<State>();
  return [st, max_requests, window](Context& ctx, Next next) -> Response {
    std::string ip(ctx.client_ip());
    auto now = std::chrono::steady_clock::now();
    int count = 0;
    {
      std::lock_guard<std::mutex> lk(st->mu);
      // Opportunistic prune when the table gets big.
      if (st->hits.size() > 8192) {
        for (auto it = st->hits.begin(); it != st->hits.end();) {
          if (now - it->second.start > window * 2)
            it = st->hits.erase(it);
          else
            ++it;
        }
      }
      Entry& e = st->hits[ip];
      if (e.count == 0 || now - e.start >= window) {
        e.count = 1;
        e.start = now;
        count = 1;
      } else {
        count = ++e.count;
      }
    }
    if (count > max_requests) {
      Response r = Response::text("Too Many Requests", Status::TooManyRequests);
      r.with_header("Retry-After", std::to_string(window.count()));
      return r;
    }
    return next(ctx);
  };
}

Middleware BasicAuth(std::string username, std::string password) {
  return BasicAuth(
      [u = std::move(username), p = std::move(password)](std::string_view user,
                                                         std::string_view pass) {
        return detail::const_time_eq(user, u) && detail::const_time_eq(pass, p);
      });
}

Middleware BasicAuth(std::function<bool(std::string_view user, std::string_view pass)> verify,
                     std::string realm) {
  return [verify = std::move(verify), realm = std::move(realm)](Context& ctx,
                                                                Next next) -> Response {
    auto h = ctx.header("authorization");
    std::string_view creds;
    if (h && h->substr(0, 6) == "Basic ") creds = h->substr(6);
    std::string decoded;
    bool ok = false;
    if (!creds.empty() && detail::b64_decode(creds, decoded, false)) {
      auto colon = decoded.find(':');
      if (colon != std::string::npos) {
        std::string_view user(decoded.data(), colon);
        std::string_view pass(decoded.data() + colon + 1, decoded.size() - colon - 1);
        ok = verify(user, pass);
      }
    }
    if (!ok) {
      Response r = Response::unauthorized("Unauthorized");
      r.with_header("WWW-Authenticate", "Basic realm=\"" + realm + "\"");
      return r;
    }
    return next(ctx);
  };
}

Middleware BearerAuth(std::string token) {
  return BearerAuth([t = std::move(token)](std::string_view got) {
    return detail::const_time_eq(got, t);
  });
}

Middleware BearerAuth(std::function<bool(std::string_view token)> verify) {
  return [verify = std::move(verify)](Context& ctx, Next next) -> Response {
    auto tok = ctx.bearer_token();
    if (!tok || !verify(*tok)) {
      Response r = Response::unauthorized("Unauthorized");
      r.with_header("WWW-Authenticate", "Bearer");
      return r;
    }
    return next(ctx);
  };
}

Middleware JwtHs256Auth(std::string secret) {
  return [secret = std::move(secret)](Context& ctx, Next next) -> Response {
    auto fail = []() {
      Response r = Response::unauthorized("Unauthorized");
      r.with_header("WWW-Authenticate", "Bearer");
      return r;
    };
    auto tok = ctx.bearer_token();
    if (!tok) return fail();
    std::string_view jwt = *tok;
    auto d1 = jwt.find('.');
    auto d2 = (d1 == std::string_view::npos) ? std::string_view::npos : jwt.find('.', d1 + 1);
    if (d1 == std::string_view::npos || d2 == std::string_view::npos) return fail();
    std::string_view signing_input = jwt.substr(0, d2);
    std::string_view sig_b64 = jwt.substr(d2 + 1);
    std::string mac = detail::hmac_sha256(secret, signing_input);
    std::string expect = detail::b64url_encode(mac);
    if (!detail::const_time_eq(sig_b64, expect)) return fail();
    // "exp" claim: reject expired tokens (if present and numeric).
    std::string payload_json;
    if (!detail::b64_decode(jwt.substr(d1 + 1, d2 - d1 - 1), payload_json, true)) return fail();
    if (auto v = json::parse(payload_json)) {
      if (auto* o = std::get_if<json::Object>(&v->data)) {
        if (auto* exp = json::find(*o, "exp")) {
          if (auto n = json::get_number(*exp)) {
            auto now = std::chrono::system_clock::now().time_since_epoch();
            double now_s =
                static_cast<double>(std::chrono::duration_cast<std::chrono::seconds>(now).count());
            if (*n <= now_s) return fail();
          }
        }
      }
    }
    return next(ctx);
  };
}

}  // namespace elaina

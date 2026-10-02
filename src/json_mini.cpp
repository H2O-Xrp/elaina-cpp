/// @file json_mini.cpp
/// @brief Implementasi MiniJson: escape/stringify + parser recursive-descent.
///
/// Parser (struct Parser lokal): objek/array/string/angka/literal, escape
/// \\uXXXX + surrogate pair, batas kedalaman 64, integer-vs-double otomatis.
/// Lihat json::parse() di modules/json/json.hpp untuk kontrak publik.
#include "../modules/json/json.hpp"

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace elaina::json {
namespace {

// Recursive-descent parser over a string_view cursor.
struct Parser {
  std::string_view s;
  std::size_t pos = 0;
  int depth = 0;
  static constexpr int kMaxDepth = 64;

  bool eof() const noexcept { return pos >= s.size(); }
  char peek() const noexcept { return eof() ? '\0' : s[pos]; }

  void skip_ws() noexcept {
    while (!eof()) {
      char c = s[pos];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
        ++pos;
      else
        break;
    }
  }

  bool consume(char c) noexcept {
    if (peek() == c) {
      ++pos;
      return true;
    }
    return false;
  }

  bool consume_lit(std::string_view lit) noexcept {
    if (s.substr(pos, lit.size()) == lit) {
      pos += lit.size();
      return true;
    }
    return false;
  }

  // \uXXXX -> UTF-8 append. Handles surrogate pairs. Returns false on bad hex.
  static bool append_utf8(std::string& out, unsigned cp) {
    if (cp <= 0x7F) {
      out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      return false;
    }
    return true;
  }

  static bool hex4(std::string_view h, unsigned& out) noexcept {
    if (h.size() < 4) return false;
    unsigned v = 0;
    for (int i = 0; i < 4; ++i) {
      char c = h[i];
      v <<= 4;
      if (c >= '0' && c <= '9')
        v |= static_cast<unsigned>(c - '0');
      else if (c >= 'a' && c <= 'f')
        v |= static_cast<unsigned>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F')
        v |= static_cast<unsigned>(c - 'A' + 10);
      else
        return false;
    }
    out = v;
    return true;
  }

  std::optional<std::string> parse_string() {
    if (!consume('"')) return std::nullopt;
    std::string out;
    while (!eof()) {
      char c = s[pos++];
      if (c == '"') return out;
      if (c == '\\') {
        if (eof()) return std::nullopt;
        char e = s[pos++];
        switch (e) {
          case '"': out.push_back('"'); break;
          case '\\': out.push_back('\\'); break;
          case '/': out.push_back('/'); break;
          case 'b': out.push_back('\b'); break;
          case 'f': out.push_back('\f'); break;
          case 'n': out.push_back('\n'); break;
          case 'r': out.push_back('\r'); break;
          case 't': out.push_back('\t'); break;
          case 'u': {
            if (pos + 4 > s.size()) return std::nullopt;
            unsigned cp = 0;
            if (!hex4(s.substr(pos, 4), cp)) return std::nullopt;
            pos += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF) {
              // high surrogate: expect \uDC00..\uDFFF
              if (pos + 6 > s.size() || s[pos] != '\\' || s[pos + 1] != 'u')
                return std::nullopt;
              unsigned lo = 0;
              if (!hex4(s.substr(pos + 2, 4), lo)) return std::nullopt;
              if (lo < 0xDC00 || lo > 0xDFFF) return std::nullopt;
              pos += 6;
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
              return std::nullopt;  // lone low surrogate
            }
            if (!append_utf8(out, cp)) return std::nullopt;
            break;
          }
          default: return std::nullopt;
        }
      } else if (static_cast<unsigned char>(c) < 0x20) {
        return std::nullopt;  // unescaped control
      } else {
        out.push_back(c);
      }
    }
    return std::nullopt;  // unterminated
  }

  std::optional<Value> parse_value() {
    if (++depth > kMaxDepth) return std::nullopt;
    skip_ws();
    std::optional<Value> v;
    char c = peek();
    if (c == '{') {
      v = parse_object();
    } else if (c == '[') {
      v = parse_array();
    } else if (c == '"') {
      auto str = parse_string();
      if (str) v = Value(std::move(*str));
    } else if (c == 't') {
      if (consume_lit("true")) v = Value(true);
    } else if (c == 'f') {
      if (consume_lit("false")) v = Value(false);
    } else if (c == 'n') {
      if (consume_lit("null")) v = Value(nullptr);
    } else if (c == '-' || (c >= '0' && c <= '9')) {
      v = parse_number();
    }
    --depth;
    return v;
  }

  std::optional<Value> parse_number() {
    std::size_t start = pos;
    if (peek() == '-') ++pos;
    if (eof()) return std::nullopt;
    if (peek() == '0') {
      ++pos;
    } else if (peek() >= '1' && peek() <= '9') {
      while (!eof() && s[pos] >= '0' && s[pos] <= '9') ++pos;
    } else {
      return std::nullopt;
    }
    bool is_double = false;
    if (peek() == '.') {
      is_double = true;
      ++pos;
      if (eof() || s[pos] < '0' || s[pos] > '9') return std::nullopt;
      while (!eof() && s[pos] >= '0' && s[pos] <= '9') ++pos;
    }
    if (peek() == 'e' || peek() == 'E') {
      is_double = true;
      ++pos;
      if (peek() == '+' || peek() == '-') ++pos;
      if (eof() || s[pos] < '0' || s[pos] > '9') return std::nullopt;
      while (!eof() && s[pos] >= '0' && s[pos] <= '9') ++pos;
    }
    std::string_view tok = s.substr(start, pos - start);
    if (!is_double) {
      std::int64_t i = 0;
      auto [ptr, ec] =
          std::from_chars(tok.data(), tok.data() + tok.size(), i);
      if (ec == std::errc{} && ptr == tok.data() + tok.size()) return Value(i);
      // overflow -> fall through to double
    }
    // strtod is locale-dependent for '.', but framework runs in C locale in
    // practice; use it for simplicity (no alloc: needs NUL, so copy small tok).
    // tok is bounded by input size; numbers > 1KB are rejected as malformed.
    if (tok.size() > 1024) return std::nullopt;
    char buf[1024];
    std::memcpy(buf, tok.data(), tok.size());
    buf[tok.size()] = '\0';
    char* end = nullptr;
    double d = std::strtod(buf, &end);
    if (end != buf + tok.size()) return std::nullopt;
    return Value(d);
  }

  std::optional<Value> parse_array() {
    if (!consume('[')) return std::nullopt;
    Array a;
    skip_ws();
    if (consume(']')) return Value(std::move(a));
    while (true) {
      auto v = parse_value();
      if (!v) return std::nullopt;
      a.push_back(std::move(*v));
      skip_ws();
      if (consume(']')) return Value(std::move(a));
      if (!consume(',')) return std::nullopt;
    }
  }

  std::optional<Value> parse_object() {
    if (!consume('{')) return std::nullopt;
    Object o;
    skip_ws();
    if (consume('}')) return Value(std::move(o));
    while (true) {
      skip_ws();
      auto k = parse_string();
      if (!k) return std::nullopt;
      skip_ws();
      if (!consume(':')) return std::nullopt;
      auto v = parse_value();
      if (!v) return std::nullopt;
      o.emplace(std::move(*k), std::move(*v));
      skip_ws();
      if (consume('}')) return Value(std::move(o));
      if (!consume(',')) return std::nullopt;
    }
  }
};

}  // namespace

std::string escape(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 2);
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if ((unsigned char)c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

std::string stringify(const Value& v) {
  struct Visitor {
    std::string operator()(std::nullptr_t) { return "null"; }
    std::string operator()(bool b) { return b ? "true" : "false"; }
    std::string operator()(double d) {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%.17g", d);
      return buf;
    }
    std::string operator()(std::int64_t i) { return std::to_string(i); }
    std::string operator()(const std::string& s) { return "\"" + escape(s) + "\""; }
    std::string operator()(const Array& a) {
      std::string o = "[";
      bool first = true;
      for (auto& e : a) {
        if (!first) o += ",";
        first = false;
        o += stringify(e);
      }
      return o + "]";
    }
    std::string operator()(const Object& o) {
      std::string s = "{";
      bool first = true;
      for (auto& [k, val] : o) {
        if (!first) s += ",";
        first = false;
        s += "\"" + escape(k) + "\":" + stringify(val);
      }
      return s + "}";
    }
  };
  return std::visit(Visitor{}, v.data);
}

std::optional<Value> parse(std::string_view text) {
  Parser p{text, 0, 0};
  auto v = p.parse_value();
  if (!v) return std::nullopt;
  p.skip_ws();
  if (!p.eof()) return std::nullopt;  // trailing garbage
  return v;
}

}  // namespace elaina::json

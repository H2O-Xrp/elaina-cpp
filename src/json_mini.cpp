#include "../modules/json/json.hpp"

#include <cstdio>

namespace elaina::json {

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

}  // namespace elaina::json

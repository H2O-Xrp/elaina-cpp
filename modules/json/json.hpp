#pragma once

// Minimal dependency-free JSON module (MVP).
// Exposes a JsonBackend concept seam so Boost.JSON / glaze / simdjson can be
// plugged in later without touching core (see ADR-008). Default backend here
// is dependency-free (MiniJson) so MVP builds offline; swap to Boost.JSON by
// defining ELAINA_JSON_BOOST and providing the backend.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace elaina::json {

struct Value;
using Object = std::map<std::string, Value, std::less<>>;
using Array = std::vector<Value>;

struct Value {
  using Storage = std::variant<std::nullptr_t, bool, double, std::int64_t,
                               std::string, Array, Object>;
  Storage data = nullptr;

  Value() = default;
  Value(std::nullptr_t) : data(nullptr) {}
  Value(bool b) : data(b) {}
  Value(int i) : data(static_cast<std::int64_t>(i)) {}
  Value(std::int64_t i) : data(i) {}
  Value(double d) : data(d) {}
  Value(const char* s) : data(std::string(s)) {}
  Value(std::string s) : data(std::move(s)) {}
  Value(Array a) : data(std::move(a)) {}
  Value(Object o) : data(std::move(o)) {}

  static Value str(std::string_view s) { return Value(std::string(s)); }
  static Value num(double d) { return Value(d); }
  static Value integer(std::int64_t i) { return Value(i); }
  static Value boolean(bool b) { return Value(b); }
  static Value arr(Array a) { return Value(std::move(a)); }
  static Value obj(Object o) { return Value(std::move(o)); }
};

std::string escape(std::string_view s);
std::string stringify(const Value& v);

// Tiny object builder: json::obj({{"a", 1}, {"b", "x"}})
inline Object obj(std::initializer_list<std::pair<std::string, Value>> init) {
  Object o;
  for (auto& [k, v] : init) o.emplace(k, v);
  return o;
}

}  // namespace elaina::json

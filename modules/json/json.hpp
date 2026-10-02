/// @file json.hpp
/// @brief Modul JSON minimal tanpa dependensi (MiniJson).
///
/// Mengekspos seam konsep JsonBackend agar Boost.JSON / glaze / simdjson
/// bisa dipasang nanti tanpa menyentuh core (lihat ADR-008). Backend
/// default di sini bebas-dep sehingga MVP build offline; ganti ke
/// Boost.JSON dengan mendefinisikan ELAINA_JSON_BOOST dan menyediakan
/// backend-nya.
///
/// Isi: Value (variant null/bool/double/int64/string/Array/Object),
/// stringify(), parse() recursive-descent, helper akses tanpa throw,
/// dan builder kecil obj().
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace elaina::json {

struct Value;
/// @brief Objek JSON: peta kunci-nilai terurut.
using Object = std::map<std::string, Value, std::less<>>;
/// @brief Array JSON.
using Array = std::vector<Value>;

/// @brief Nilai JSON generik (variant).
///
/// Konstruksi implisit dari tiap tipe didukung agar literal inisialisasi
/// ringkas: `obj({{"id", 42}, {"name", "x"}, {"ok", true}})`.
struct Value {
  /// @brief Penyimpanan variant semua tipe JSON.
  using Storage = std::variant<std::nullptr_t, bool, double, std::int64_t,
                               std::string, Array, Object>;
  Storage data = nullptr;  ///< Nilai (default null).

  Value() = default;                          ///< null.
  Value(std::nullptr_t) : data(nullptr) {}    ///< null eksplisit.
  Value(bool b) : data(b) {}                  ///< boolean.
  Value(int i) : data(static_cast<std::int64_t>(i)) {}  ///< int -> int64.
  Value(std::int64_t i) : data(i) {}          ///< integer 64-bit.
  Value(double d) : data(d) {}                ///< floating point.
  Value(const char* s) : data(std::string(s)) {}  ///< string literal.
  Value(std::string s) : data(std::move(s)) {}    ///< string milik.
  Value(Array a) : data(std::move(a)) {}      ///< array.
  Value(Object o) : data(std::move(o)) {}     ///< objek.

  /// @brief Buat string.
  static Value str(std::string_view s) { return Value(std::string(s)); }
  /// @brief Buat angka desimal.
  static Value num(double d) { return Value(d); }
  /// @brief Buat integer.
  static Value integer(std::int64_t i) { return Value(i); }
  /// @brief Buat boolean.
  static Value boolean(bool b) { return Value(b); }
  /// @brief Buat array.
  static Value arr(Array a) { return Value(std::move(a)); }
  /// @brief Buat objek.
  static Value obj(Object o) { return Value(std::move(o)); }
};

/// @brief Escape string untuk output JSON (tanda kutip, backslash, kontrol).
std::string escape(std::string_view s);
/// @brief Serialisasi nilai menjadi teks JSON.
std::string stringify(const Value& v);

// Minimal recursive-descent parser (no deps). Accepts a single JSON value
// with only trailing whitespace allowed. Depth limited (64) to bound stack.
// Numbers without fraction/exponent become int64 when they fit, else double.
/// @brief Parse satu nilai JSON (tanpa dep).
///
/// Menerima satu nilai dengan hanya whitespace di ekor. Kedalaman dibatasi
/// 64 (anti stack-overflow). Angka tanpa fraksi/eksponen menjadi int64 bila
/// muat, bila tidak menjadi double. Escape termasuk \\uXXXX + surrogate pair.
/// @param text Teks JSON.
/// @return Nilai atau nullopt bila malformed (lihat ctx.json() untuk gaya pakai).
std::optional<Value> parse(std::string_view text);

// Lookup helpers for REST handlers (no throw, no alloc).
/// @brief Cari field objek (tanpa throw/alokasi).
/// @return Pointer nilai atau nullptr.
inline const Value* find(const Object& o, std::string_view key) noexcept {
  auto it = o.find(key);
  return it == o.end() ? nullptr : &it->second;
}
/// @brief Ambil string dari Value (view pinjaman, tanpa alokasi).
inline std::optional<std::string_view> get_string(const Value& v) noexcept {
  if (auto* s = std::get_if<std::string>(&v.data)) return std::string_view(*s);
  return std::nullopt;
}
/// @brief Ambil integer (double dibulatkan ke bawah via cast).
inline std::optional<std::int64_t> get_int(const Value& v) noexcept {
  if (auto* i = std::get_if<std::int64_t>(&v.data)) return *i;
  if (auto* d = std::get_if<double>(&v.data)) return static_cast<std::int64_t>(*d);
  return std::nullopt;
}
/// @brief Ambil angka (int di-promote ke double).
inline std::optional<double> get_number(const Value& v) noexcept {
  if (auto* d = std::get_if<double>(&v.data)) return *d;
  if (auto* i = std::get_if<std::int64_t>(&v.data)) return static_cast<double>(*i);
  return std::nullopt;
}
/// @brief Ambil boolean.
inline std::optional<bool> get_bool(const Value& v) noexcept {
  if (auto* b = std::get_if<bool>(&v.data)) return *b;
  return std::nullopt;
}

// Tiny object builder: json::obj({{"a", 1}, {"b", "x"}})
/// @brief Builder objek kecil: `json::obj({{"a", 1}, {"b", "x"}})`.
inline Object obj(std::initializer_list<std::pair<std::string, Value>> init) {
  Object o;
  for (auto& [k, v] : init) o.emplace(k, v);
  return o;
}

}  // namespace elaina::json

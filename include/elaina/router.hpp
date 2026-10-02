/// @file router.hpp
/// @brief Router segment-trie: pencocokan path cepat tanpa alokasi.
///
/// Struktur: tiap level dipisah '/', anak statis di hash-map,
/// satu anak :param, satu wildcard '*' terminal.
/// Prioritas: statis > :param > wildcard.
/// Beku implisit setelah listen() — tanpa mutasi saat serve sehingga
/// pembacaan lock-free di tiap loop thread (Server menjamin registrasi
/// happens-before listen).
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
/// @brief Konsep compile-time: callable Response(Context&).
///
/// Dipakai untuk pesan error registrasi yang pendek dan jelas.
template <class F>
concept HandlerFn = requires(F f, Context& c) {
  { f(c) } -> std::convertible_to<Response>;
};
}  // namespace detail

/// @brief Hasil pencocokan satu request.
/// @see Router::match
struct RouteMatch {
  const Handler* handler = nullptr;  ///< Handler pemilik route (stabil). null bila miss.
  ParamMap params;                   ///< Param :nama yang terekstrak.
  bool method_mismatch = false;      ///< true bila path ada di metode lain (sinyal 405).
};

/// @brief Router segment-trie per-metode.
///
/// Pola: "/users/:id", "/static/*" (wildcard harus segmen terakhir).
/// Match nol-alokasi: iterasi segmen di tempat + lookup hash transparan
/// (tanpa std::string sementara).
class Router {
 public:
  /// @brief Konstruktor default (tabel kosong).
  Router() = default;
  Router(const Router&) = delete;
  Router& operator=(const Router&) = delete;

  // Returns false on conflicting registration (e.g. two different :names
  // at same position is allowed; duplicate identical route is rejected).
  /// @brief Daftarkan handler untuk (metode, pola).
  /// @param method Metode HTTP. @param pattern Pola, mis. "/users/:id".
  /// @param handler Handler (di-move ke penyimpanan milik router).
  /// @return false bila konflik (duplikat route identik / wildcard non-terminal).
  /// @note Nama :param berbeda di posisi sama diperbolehkan (yang pertama menang).
  bool add(Method method, std::string_view pattern, Handler handler);

  // Convenience for route groups: prefix + pattern.
  /// @brief Daftarkan dengan prefix grup: prefix + pattern.
  /// @return false bila konflik (lihat add()).
  bool add_with_prefix(Method method, std::string_view prefix,
                       std::string_view pattern, Handler handler);

  /// @brief Cocokkan (metode, path) ke handler.
  ///
  /// Nol alokasi; aman dipanggil tiap request (noexcept).
  /// Query ("?...") dipangkas otomatis bila ikut terbawa.
  /// @param method Metode request. @param path Path request.
  /// @return RouteMatch: handler+params, atau method_mismatch untuk 405,
  ///   atau kosong untuk 404.
  RouteMatch match(Method method, std::string_view path) const noexcept;

  /// @brief Jumlah route terdaftar (semua metode).
  std::size_t route_count() const noexcept { return route_count_; }

 private:
  /// @brief Hash transparan: find(string_view) tanpa temp std::string.
  struct StringHash {
    using is_transparent = void;  ///< Aktifkan lookup heterogen.
    /// @brief Hash string_view.
    std::size_t operator()(std::string_view s) const noexcept {
      return std::hash<std::string_view>{}(s);
    }
  };
  /// @brief Node trie satu level segmen.
  struct Node {
    std::unordered_map<std::string, std::unique_ptr<Node>, StringHash,
                       std::equal_to<>>
        statics;                            ///< Anak statis per segmen.
    std::unique_ptr<Node> param;            ///< Anak :param tunggal.
    std::string param_name;                 ///< Nama param (tanpa ':').
    std::unique_ptr<Node> wildcard;         ///< Catch-all '*' (harus terminal).
    const Handler* handler = nullptr;       ///< Handler (milik handlers_).
  };

  /// @brief Pecah path jadi segmen (dipakai saat registrasi/control-plane).
  /// @return Daftar view segmen; "/" -> kosong.
  static std::vector<std::string_view> split(std::string_view path) noexcept;
  /// @brief true bila segmen berupa ":nama".
  static bool is_param(std::string_view seg) noexcept {
    return seg.size() >= 2 && seg[0] == ':';
  }
  /// @brief true bila segmen wildcard "*".
  static bool is_wildcard(std::string_view seg) noexcept { return seg == "*"; }

  /// @brief Sisipkan pola ke trie (control-plane, boleh alokasi).
  /// @return false bila duplikat/konflik.
  bool insert_into(Node& root, std::string_view pattern, const Handler* hp);

  std::array<std::unique_ptr<Node>, kMethodCount> roots_;  ///< Akar per metode.
  // Owns handler callables so Node pointers stay stable.
  std::vector<std::unique_ptr<Handler>> handlers_;  ///< Pemilik Handler (pointer stabil).
  std::size_t route_count_ = 0;  ///< Jumlah route.
};

}  // namespace elaina

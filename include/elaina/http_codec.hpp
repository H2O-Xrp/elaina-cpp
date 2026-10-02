/// @file http_codec.hpp
/// @brief Parser HTTP ketat + encoder respons.
///
/// StrictHttpParser adalah parser bawaan yang ketat: hanya HTTP/1.0+1.1,
/// request line, header, body Content-Length, dan semantik keep-alive.
/// Menolak sisanya dengan 400/413/431/501 yang tepat (termasuk penjaga
/// smuggling CL+chunked). Mengekspos seam ala llhttp agar backend
/// LlhttpParser bisa dipasang nanti tanpa menyentuh Server/Connection
/// (lihat ADR-007).
///
/// Subset yang didukung: request line HTTP/1.0 + HTTP/1.1, header,
/// body Content-Length, keep-alive. Chunked DITOLAK eksplisit dengan 501
/// (roadmap: streaming) demi keamanan.
///
/// Semua hasil parse berupa view yang meminjam buffer input.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "http.hpp"
#include "limits.hpp"
#include "method.hpp"

namespace elaina {

/// @brief Status hasil satu upaya parse.
enum class ParseStatus {
  NeedMore,  ///< Butuh byte tambahan (parsial).
  Complete,  ///< Satu request penuh (head+body) ter-parse.
  Error,     ///< Malformed -> respons 400 (atau 501 untuk chunked).
  TooLarge,  ///< Batas terlampaui -> respons 413/431.
};

/// @brief Hasil parse satu request.
/// @see StrictHttpParser::parse
struct ParseResult {
  ParseStatus status = ParseStatus::NeedMore;  ///< Status.
  Request request;             ///< Valid bila Complete; view pinjam buffer.
  std::size_t consumed = 0;    ///< Byte terpakai dari buffer input.
  std::string error;           ///< Valid bila Error/TooLarge.
  Status http_status = Status::BadRequest;  ///< Status HTTP yang cocok.
};

/// @brief Parser HTTP/1 ketat, tanpa alokasi di luar headers/error.
///
/// Thread-safe untuk dipakai bersamaan (stateless selain limits_).
/// Untuk hot path (keep-alive), pakai parse_into() dengan Request yang
/// dipakai ulang agar vektor headers 0-alokasi setelah warmup.
class StrictHttpParser {
 public:
  /// @brief Buat parser dengan batas server.
  explicit StrictHttpParser(const ServerLimits& limits = {}) : limits_(limits) {}

  // Tries to parse one request from [data, data+len).
  // On Complete, `consumed` includes head + body bytes.
  /// @brief Coba parse satu request dari [data, data+len).
  /// @return ParseResult; bila Complete, `consumed` mencakup head+body.
  /// @note Mengalokasikan vektor headers baru tiap panggil; untuk hot path
  ///   pakai parse_into().
  ParseResult parse(const char* data, std::size_t len) const noexcept;

  // Reusable variant (hot path): fills `out_req` reusing its headers vector
  // capacity (0 allocs after warmup per connection). Returns status and sets
  // `consumed`, `error`, `http_status`. `out_req` valid iff Complete.
  /// @brief Varian pakai-ulang untuk hot path.
  ///
  /// Mengisi `out_req` dengan memakai ulang kapasitas vektor headers-nya
  /// (0 alokasi setelah warmup per koneksi). `out_req` valid hanya bila
  /// return Complete.
  /// @param data Buffer input (view dipinjam dari sini).
  /// @param len Panjang buffer.
  /// @param[out] out_req Request hasil (ditulis ulang tiap panggil).
  /// @param[out] consumed Byte terpakai (head+body).
  /// @param[out] error Pesan (hanya saat Error/TooLarge).
  /// @param[out] http_status Status HTTP yang cocok.
  /// @return Status parse.
  ParseStatus parse_into(const char* data, std::size_t len, Request& out_req,
                         std::size_t& consumed, std::string& error,
                         Status& http_status) const noexcept;

 private:
  ServerLimits limits_;  ///< Batas (salinan per parser).
};

// Encodes a Response into a flat buffer ready for send().
// Adds Content-Length, Content-Type, Connection, Date, Server headers.
/// @brief Encode Response jadi buffer datar siap send().
///
/// Menambah Content-Length, Content-Type, Connection, dan Server.
/// @param res Respons. @param keep_alive Nilai header Connection.
/// @param request_version Versi request ("HTTP/1.1"), "" -> default 1.1.
/// @return String buffer lengkap (head+\r\n\r\n+body).
std::string encode_response(const Response& res, bool keep_alive,
                            std::string_view request_version);
// Zero-extra-copy variant: appends directly into `out` (hot path).
// Avoids the temp std::string + second memcpy in Connection.
/// @brief Varian tanpa salinan ekstra: append langsung ke `out` (hot path).
/// @param[out] out Buffer tujuan (di-reserve pemanggil).
/// @note Menghemat 1 alokasi + 1 memcpy per request dibanding encode_response.
void encode_response_into(std::string& out, const Response& res, bool keep_alive,
                          std::string_view request_version);

}  // namespace elaina

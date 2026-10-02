/// @file tls.hpp
/// @brief Dukungan TLS opsional via OpenSSL.
///
/// Gaya chaining sama seperti API lain:
/// @code
///   elaina::Server app;
///   app.get("/", [](elaina::Context& ctx) -> elaina::Response {
///     return elaina::Response::text("secure hello");
///   });
///   app.tls("server.crt", "server.key").listen(3443);
/// @endcode
/// atau one-shot:
/// @code
///   app.listen_tls(3443, "server.crt", "server.key");
/// @endcode
///
/// Ketersediaan: ELAINA_HAS_TLS == 1 bila dibangun dengan OpenSSL
/// (ELAINA_ENABLE_TLS=ON dan OpenSSL ditemukan). Tanpanya, tls()/listen_tls()
/// tetap compile tetapi throw saat runtime.
#pragma once

#include <string>

#if defined(ELAINA_HAS_TLS)
#include <openssl/ssl.h>
#endif

namespace elaina {

/// @brief Opsi TLS server.
struct TlsOptions {
  std::string cert_file;  ///< Rantai sertifikat PEM.
  std::string key_file;   ///< Kunci privat PEM.
  std::string ca_file;    ///< Opsional: CA klien untuk mTLS.
  bool verify_client = false;  ///< true = wajib sertifikat klien.

  /// @brief true bila cert + key terisi.
  bool valid() const noexcept { return !cert_file.empty() && !key_file.empty(); }
};

#if defined(ELAINA_HAS_TLS)
// Per event-loop-thread acceptor: each loop thread owns one TlsAcceptor
// built from the same TlsOptions (no cross-thread SSL_CTX sharing).
/// @brief Konteks TLS per thread event-loop.
///
/// Tiap thread loop memiliki satu TlsAcceptor dari TlsOptions yang sama
/// (tanpa berbagi SSL_CTX antar thread). Minimal TLS 1.2.
class TlsAcceptor {
 public:
  /// @brief Muat cert/key ke SSL_CTX baru. Gagal -> !valid(), lihat error().
  explicit TlsAcceptor(const TlsOptions& opts);
  /// @brief Bebaskan SSL_CTX.
  ~TlsAcceptor();

  TlsAcceptor(const TlsAcceptor&) = delete;
  TlsAcceptor& operator=(const TlsAcceptor&) = delete;

  /// @brief true bila konteks siap dipakai.
  bool valid() const noexcept { return ctx_ != nullptr; }
  /// @brief Pesan kegagalan konstruksi (kosong bila valid).
  const std::string& error() const noexcept { return error_; }

  // New server-side session in accept state (caller must SSL_free it).
  /// @brief Sesi server baru dalam keadaan accept.
  /// @return SSL* milik pemanggil (wajib SSL_free), atau null.
  SSL* new_session() const;

 private:
  SSL_CTX* ctx_ = nullptr;  ///< Konteks OpenSSL milik objek ini.
  std::string error_;       ///< Alasan gagal (bila !valid()).
};
#endif

}  // namespace elaina

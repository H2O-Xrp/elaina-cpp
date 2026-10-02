/// @file tls.cpp
/// @brief Implementasi TlsAcceptor: SSL_CTX server (min TLS 1.2).
///
/// Mode ACCEPT_MOVING_WRITE_BUFFER diset agar buffer kirim boleh pindah
/// antar retry WANT_WRITE non-blocking. File ini dikompilasi hanya bila
/// ELAINA_HAS_TLS (lihat CMakeLists: ELAINA_ENABLE_TLS + OpenSSL).
#include "elaina/tls.hpp"

#if defined(ELAINA_HAS_TLS)

#include <openssl/err.h>
#include <openssl/ssl.h>

namespace elaina {

TlsAcceptor::TlsAcceptor(const TlsOptions& opts) {
  ctx_ = SSL_CTX_new(TLS_server_method());
  if (!ctx_) {
    error_ = "SSL_CTX_new failed";
    return;
  }
  SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
  // Allows our send buffer to move between WANT_WRITE retries.
  SSL_CTX_set_mode(ctx_, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
  if (SSL_CTX_use_certificate_chain_file(ctx_, opts.cert_file.c_str()) != 1) {
    error_ = "cannot load certificate: " + opts.cert_file;
    SSL_CTX_free(ctx_);
    ctx_ = nullptr;
    return;
  }
  if (SSL_CTX_use_PrivateKey_file(ctx_, opts.key_file.c_str(), SSL_FILETYPE_PEM) != 1) {
    error_ = "cannot load private key: " + opts.key_file;
    SSL_CTX_free(ctx_);
    ctx_ = nullptr;
    return;
  }
  if (SSL_CTX_check_private_key(ctx_) != 1) {
    error_ = "private key does not match certificate";
    SSL_CTX_free(ctx_);
    ctx_ = nullptr;
    return;
  }
  if (opts.verify_client) {
    if (!opts.ca_file.empty()) {
      if (SSL_CTX_load_verify_locations(ctx_, opts.ca_file.c_str(), nullptr) != 1) {
        error_ = "cannot load client CA: " + opts.ca_file;
        SSL_CTX_free(ctx_);
        ctx_ = nullptr;
        return;
      }
    }
    SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
  }
}

TlsAcceptor::~TlsAcceptor() {
  if (ctx_) SSL_CTX_free(ctx_);
}

SSL* TlsAcceptor::new_session() const {
  if (!ctx_) return nullptr;
  SSL* ssl = SSL_new(ctx_);
  if (ssl) SSL_set_accept_state(ssl);
  return ssl;
}

}  // namespace elaina

#endif

/// @file limits.hpp
/// @brief Batas keamanan server (secure-by-default).
///
/// Semua batas bisa diubah via Server::limits() sebelum listen():
/// @code
///   elaina::Server app;
///   app.limits().max_body_size = 4 * 1024 * 1024;  // 4MB
///   app.listen(3000);
/// @endcode
#pragma once

#include <cstddef>

namespace elaina {

/// @brief Batas ukuran, jumlah, timeout, dan buffer server.
///
/// Nilai default aman untuk layanan publik; naikkan sesuai kebutuhan.
/// Pelanggaran batas dijawab 413/431/408, bukan crash.
struct ServerLimits {
  std::size_t max_uri_size = 8 * 1024;        ///< Maks panjang target request (8KB).
  std::size_t max_header_size = 16 * 1024;    ///< Maks total blok header (16KB).
  std::size_t max_headers = 100;              ///< Maks jumlah field header.
  std::size_t max_body_size = 1 * 1024 * 1024;  ///< Maks body request (1MB).
  std::size_t max_frame_size = 1 * 1024 * 1024; ///< Maks satu frame/protokol (1MB).
  /// @brief Timeout menunggu header lengkap (detik).
  int header_timeout_sec = 5;
  /// @brief Timeout menunggu body lengkap (detik).
  int body_timeout_sec = 10;
  /// @brief Timeout koneksi idle tanpa aktivitas (detik).
  int idle_timeout_sec = 60;
  /// @brief Maks koneksi per event-loop thread.
  int max_connections_per_loop = 10000;
  /// @brief Kapasitas awal buffer terima per koneksi.
  std::size_t recv_buffer_size = 16 * 1024;
  /// @brief Kapasitas awal buffer kirim per koneksi.
  std::size_t send_buffer_size = 16 * 1024;
};

}  // namespace elaina

#pragma once

#include <cstddef>

namespace elaina {

// Secure-by-default limits. All configurable via Server::limits().
struct ServerLimits {
  std::size_t max_uri_size = 8 * 1024;        // 8KB
  std::size_t max_header_size = 16 * 1024;    // 16KB total
  std::size_t max_headers = 100;
  std::size_t max_body_size = 1 * 1024 * 1024;  // 1MB
  std::size_t max_frame_size = 1 * 1024 * 1024;
  // Timeouts in seconds.
  int header_timeout_sec = 5;
  int body_timeout_sec = 10;
  int idle_timeout_sec = 60;
  int max_connections_per_loop = 10000;
  std::size_t recv_buffer_size = 16 * 1024;
  std::size_t send_buffer_size = 16 * 1024;
};

}  // namespace elaina

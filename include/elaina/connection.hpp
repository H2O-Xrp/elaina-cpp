/// @file connection.hpp
/// @brief Koneksi TCP/TLS dan event loop per core.
///
/// - Connection: satu koneksi TCP (opsional TLS), pemilik buffer recv/send
///   + state parser. Thread-affine: hanya disentuh thread EventLoop-nya.
/// - EventLoop: satu loop per core CPU: Reactor + acceptor + pool koneksi.
///   Koneksi tidak pernah pindah antar loop.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "http.hpp"
#include "http_codec.hpp"
#include "limits.hpp"
#include "middleware.hpp"
#include "reactor.hpp"
#include "router.hpp"
#include "tls.hpp"

namespace elaina {

class EventLoop;

// Single TCP connection: owns recv/send buffers + parser state.
// Thread-affine: only touched by its EventLoop thread.
/// @brief Satu koneksi TCP (atau TLS): pemilik buffer + state parser.
///
/// Siklus: on_readable() (recv->dispatch berulang untuk pipelining) ->
/// dispatch_one() (parse->route->middleware->handler->encode->send oportunistik)
/// -> on_writable() (flush). Konsumsi recv berbasis offset (tanpa memmove
/// per request). Bila TLS, I/O lewat SSL_read/SSL_write dengan handshake
/// non-blocking.
class Connection {
 public:
  /// @brief Bungkus fd klien yang sudah non-blocking.
  /// @param fd Socket milik koneksi ini (ditutup di destruktor).
  /// @param client_ip IP klien (teks, untuk log/rate-limit).
  /// @param limits Salinan batas server.
  /// @param router Tabel route beku (bukan milik).
  /// @param middlewares Rantai beku (bukan milik, boleh null).
  /// @param not_found Handler 404 custom (bukan milik, boleh null).
  /// @param ssl Sesi OpenSSL accept-state (milik koneksi ini; null = polos).
  Connection(int fd, std::string client_ip, const ServerLimits& limits,
             const Router* router, const MiddlewareChain* middlewares,
             const Handler* not_found = nullptr
#if defined(ELAINA_HAS_TLS)
             ,
             SSL* ssl = nullptr
#endif
  );
  /// @brief Tutup fd (+ SSL_shutdown/SSL_free bila TLS).
  ~Connection();

  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  /// @brief File descriptor.
  int fd() const noexcept { return fd_; }
  /// @brief true bila (akan) ditutup.
  bool closed() const noexcept { return closed_; }
  /// @brief Aktivitas terakhir (untuk timeout).
  std::chrono::steady_clock::time_point last_activity() const noexcept {
    return last_activity_;
  }

  // Called by EventLoop when fd is readable/writable.
  // Returns false if connection should be destroyed.
  /// @brief Dipanggil saat fd readable. false = koneksi harus dihancurkan.
  bool on_readable();
  /// @brief Dipanggil saat fd writable / perlu flush. false = hancurkan.
  bool on_writable();
  /// @brief Tick timeout berkala dari EventLoop. false = hancurkan.
  bool on_tick(std::chrono::steady_clock::time_point now);

  /// @brief true bila ada byte respons tertunda.
  bool wants_write() const noexcept { return !send_buf_.empty() && send_off_ < send_buf_.size(); }
  // Write interest for the reactor: pending app bytes OR a TLS state that
  // needs the socket writable (handshake / renegotiation WANT_WRITE).
  /// @brief Minat tulis untuk reactor: byte tertunda ATAU state TLS yang
  /// butuh socket writable (handshake/renegosiasi WANT_WRITE).
  bool wants_socket_write() const noexcept {
    if (wants_write()) return true;
#if defined(ELAINA_HAS_TLS)
    if (ssl_ && !tls_handshake_done_ && tls_hs_want_write_) return true;
    if (ssl_ && tls_handshake_done_ && tls_retry_read_) return true;
#endif
    return false;
  }
  /// @brief Apakah ReadWrite sedang terpasang (koalesensi epoll_ctl).
  bool write_armed() const noexcept { return write_armed_; }
  /// @brief Tandai status arming (dipakai EventLoop).
  void set_write_armed(bool v) noexcept { write_armed_ = v; }
  /// @brief Byte recv tertunda (belum ter-parse).
  std::size_t pending_recv() const noexcept { return recv_buf_.size() - recv_off_; }
  /// @brief true bila koneksi ini TLS.
  bool is_tls() const noexcept {
#if defined(ELAINA_HAS_TLS)
    return ssl_ != nullptr;
#else
    return false;
#endif
  }

 private:
  /// @brief Parse + dispatch satu request (atau NeedMore).
  /// @return true bila sebuah pesan terkonsumsi (lanjut pipelining).
  bool dispatch_one();
  /// @brief Antre respons error lalu tandai tutup.
  void make_error_response(Status s, std::string_view msg, bool keep_alive,
                           std::string_view version);
  /// @brief Tandai tutup (fd ditutup di destruktor / erase loop).
  void close() noexcept;
  /// @brief Konsumsi n byte recv via offset (reset bila habis; kompaksi
  /// diamortisasi hanya bila offset > 8KB).
  void consume_recv(std::size_t n) noexcept;
  /// @brief recv(2) atau SSL_read — kontrak errno sama.
  ssize_t net_recv(char* buf, std::size_t len) noexcept;
  /// @brief send(2) atau SSL_write — kontrak errno sama.
  ssize_t net_send(const char* buf, std::size_t len) noexcept;
#if defined(ELAINA_HAS_TLS)
  // Drives SSL_accept until done or blocked. Returns true when the handshake
  // is complete (app data may flow). False = wait for more I/O or closed.
  /// @brief Jalankan SSL_accept sampai selesai/terblokir.
  /// @return true bila handshake selesai. false = tunggu I/O / tutup.
  bool drive_tls_handshake();
  /// @brief Baca TLS (EAGAIN = WANT_READ, atau WANT_WRITE + flag retry).
  ssize_t tls_recv(char* buf, std::size_t len) noexcept;
  /// @brief Tulis TLS (EAGAIN = WANT_*).
  ssize_t tls_send(const char* buf, std::size_t len) noexcept;
#endif

  int fd_ = -1;                 ///< Socket (milik).
  std::string client_ip_;       ///< IP klien.
  ServerLimits limits_;         ///< Salinan batas.
  const Router* router_;        ///< Tabel route (bukan milik).
  const MiddlewareChain* middlewares_;  ///< Rantai (bukan milik).
  const Handler* not_found_ = nullptr;  ///< 404 custom (bukan milik).

  std::vector<char> recv_buf_;  ///< Buffer terima (dengan offset).
  std::size_t recv_off_ = 0;    ///< Offset konsumsi.
  std::string send_buf_;        ///< Buffer kirim.
  std::size_t send_off_ = 0;    ///< Offset flush.
  bool closed_ = false;         ///< Akan ditutup.
  bool write_armed_ = false;    ///< ReadWrite terpasang?
  // Reused across requests on this connection (thread-affine): headers vector
  // keeps capacity -> 0 allocs after first request (see parse_into).
  Request dispatch_req_;       ///< Request dipakai ulang (hemat alokasi).
  std::string dispatch_error_; ///< Error dipakai ulang.
#if defined(ELAINA_HAS_TLS)
  SSL* ssl_ = nullptr;            ///< Sesi TLS (milik, null = polos).
  bool tls_handshake_done_ = true;///< Handshake selesai?
  bool tls_retry_read_ = false;   ///< SSL_read WANT_WRITE: ulangi saat writable.
  bool tls_hs_want_write_ = false;///< Handshake butuh writable.
#endif
  std::chrono::steady_clock::time_point created_;       ///< Waktu accept.
  std::chrono::steady_clock::time_point last_activity_; ///< Aktivitas terakhir.
  StrictHttpParser parser_;  ///< Parser (limits per koneksi).
};

// One event loop per CPU core (ADR-004): owns Reactor + acceptor +
// connection pool. Connections never migrate between loops.
/// @brief Satu event loop per core: Reactor + acceptor + pool koneksi.
///
/// Tiap loop membuat socket listen sendiri (SO_REUSEPORT; kernel membagi
/// accept). Koneksi tidak pernah migrasi antar loop. Berhenti kooperatif
/// via flag stop().
class EventLoop {
 public:
  /// @brief Handler accept custom (tak dipakai loop bawaan).
  using AcceptHandler = std::function<void(int client_fd, std::string ip)>;

  /// @brief Buat loop (belum jalan).
  /// @param not_found Handler 404 custom (bukan milik).
  /// @param tls Acceptor TLS per-thread (bukan milik, null = polos).
  EventLoop(const ServerLimits& limits, const Router* router,
            const MiddlewareChain* middlewares, const Handler* not_found = nullptr
#if defined(ELAINA_HAS_TLS)
            ,
            const TlsAcceptor* tls = nullptr
#endif
  );
  /// @brief Destruktor default (koneksi ditutup).
  ~EventLoop();

  EventLoop(const EventLoop&) = delete;
  EventLoop& operator=(const EventLoop&) = delete;

  // Creates a SO_REUSEPORT listen socket bound to port. Returns fd or -1.
  /// @brief Buat socket listen SO_REUSEPORT non-blocking.
  /// @param port Port (0 = efemeral). @param backlog Antrian listen.
  /// @return fd atau -1.
  static int make_listen_socket(int port, int backlog = 1024);

  // Runs loop on current thread: accept + poll + serve until stop().
  // Each loop creates its own listen socket (kernel distributes accepts).
  /// @brief Jalankan loop di thread ini sampai stop().
  /// @param port Port (socket per thread via SO_REUSEPORT).
  /// @param[out] stop Flag berhenti kooperatif.
  void run(int port, std::atomic<bool>& stop);

  // Single-socket variant (used by tests): serve one already-bound socket.
  /// @brief Varian satu socket (dipakai test): layani listen_fd milik pemanggil.
  void run_with_fd(int listen_fd, std::atomic<bool>& stop);

  /// @brief Callback port aktual (berguna saat port==0).
  void set_on_ready(std::function<void(int port)> cb) { on_ready_ = std::move(cb); }

 private:
  /// @brief Accept sebanyak-banyaknya (maks 64/wakeup) + TCP_NODELAY.
  bool accept_all(int listen_fd);
  /// @brief Terapkan timeout ke semua koneksi.
  void tick_timeouts();

  ServerLimits limits_;                 ///< Salinan batas.
  const Router* router_;                ///< Tabel route (bukan milik).
  const MiddlewareChain* middlewares_;  ///< Rantai (bukan milik).
  const Handler* not_found_ = nullptr;  ///< 404 custom (bukan milik).
#if defined(ELAINA_HAS_TLS)
  const TlsAcceptor* tls_ = nullptr;  ///< Acceptor TLS (bukan milik).
#endif
  std::unique_ptr<Reactor> reactor_;  ///< Reactor milik loop.
  std::unordered_map<int, std::unique_ptr<Connection>> conns_;  ///< Pool koneksi.
  std::function<void(int port)> on_ready_;  ///< Callback siap.
};

}  // namespace elaina

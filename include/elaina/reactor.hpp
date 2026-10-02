/// @file reactor.hpp
/// @brief Abstraksi reactor event (epoll di Linux, poll sebagai fallback).
///
/// Server bergantung pada konsep ini, bukan langsung pada epoll, sehingga
/// backend io_uring bisa ditambahkan di balik interface yang sama
/// (lihat ADR-003/ADR-012).
#pragma once

#if defined(__linux__)
#define ELAINA_HAS_EPOLL 1  ///< 1 bila epoll tersedia (Linux).
#else
#define ELAINA_HAS_EPOLL 0
#endif

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
#if !ELAINA_HAS_EPOLL
#include <poll.h>
#else
#include <poll.h>
#endif

namespace elaina {

/// @brief Minat event per fd.
enum class Interest : std::uint8_t {
  Read = 1,      ///< Siap baca.
  Write = 2,     ///< Siap tulis.
  ReadWrite = 3  ///< Keduanya.
};

/// @brief Satu event kesiapan fd dari poll().
struct FdEvent {
  int fd = -1;          ///< File descriptor.
  bool readable = false;///< Punya data masuk.
  bool writable = false;///< Bisa tulis tanpa blokir.
  bool error = false;   ///< Error/hangup.
};

// Level-triggered reactor interface.
/// @brief Interface reactor level-triggered.
///
/// Semua implementasi bersifat level-triggered: selama kondisi masih
/// terpenuhi, event terus dilaporkan (aman terhadap event yang terlewat).
class Reactor {
 public:
  virtual ~Reactor() = default;
  /// @brief Daftarkan fd dengan minat awal.
  virtual bool add(int fd, Interest interest) = 0;
  /// @brief Ubah minat fd terdaftar.
  virtual bool modify(int fd, Interest interest) = 0;
  /// @brief Hapus fd (abaikan error; fd mungkin sudah tertutup).
  virtual bool remove(int fd) = 0;
  // Returns number of events; -1 on fatal error.
  /// @brief Tunggu event sampai timeout.
  /// @param[out] out Ditambah event (tidak dikosongkan pemanggil? lihat
  ///   EventLoop: dikosongkan tiap iterasi).
  /// @param timeout_ms Batas tunggu.
  /// @return Jumlah event; -1 bila error fatal.
  virtual int poll(std::vector<FdEvent>& out, int timeout_ms) = 0;
};

#if ELAINA_HAS_EPOLL
/// @brief Reactor epoll (Linux): backend default yang tercepat.
class EpollReactor : public Reactor {
 public:
  /// @brief Buat epoll fd (EPOLL_CLOEXEC).
  EpollReactor();
  /// @brief Tutup epoll fd.
  ~EpollReactor() override;
  EpollReactor(const EpollReactor&) = delete;
  EpollReactor& operator=(const EpollReactor&) = delete;

  bool add(int fd, Interest interest) override;
  bool modify(int fd, Interest interest) override;
  bool remove(int fd) override;
  int poll(std::vector<FdEvent>& out, int timeout_ms) override;
  /// @brief true bila epoll fd valid.
  bool valid() const noexcept { return epfd_ >= 0; }

 private:
  int epfd_ = -1;  ///< epoll fd (milik objek ini).
};
#endif

/// @brief Reactor poll(2): fallback portabel saat epoll tak ada.
class PollReactor : public Reactor {
 public:
  PollReactor() = default;
  bool add(int fd, Interest interest) override;
  bool modify(int fd, Interest interest) override;
  bool remove(int fd) override;
  int poll(std::vector<FdEvent>& out, int timeout_ms) override;

 private:
  /// @brief Entri fd terdaftar.
  struct Entry {
    int fd = -1;
    Interest interest = Interest::Read;
  };
  std::vector<Entry> entries_;  ///< Tabel fd.
#if defined(__linux__)
  // Reused across poll() calls: avoids 1 alloc/poll iteration (old code
  // built a local vector<pollfd> every call).
  /// @brief Buffer pollfd dipakai ulang (hindari 1 alokasi per poll).
  std::vector<::pollfd> pfds_;
#endif
};

// Factory: best available reactor for this platform.
/// @brief Buat reactor terbaik platform ini (epoll bila valid, else poll).
inline std::unique_ptr<Reactor> make_default_reactor() {
#if ELAINA_HAS_EPOLL
  auto r = std::make_unique<EpollReactor>();
  if (r->valid()) return r;
#endif
  return std::make_unique<PollReactor>();
}

}  // namespace elaina

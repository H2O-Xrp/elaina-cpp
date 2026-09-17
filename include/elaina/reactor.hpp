#pragma once

// Reactor abstraction (ADR-003/ADR-012).
// Server depends on this concept, not on epoll directly, so io_uring can be
// added later behind the same interface with zero hot-path virtual cost
// (Connection is templated on the concrete reactor in the full design;
// MVP uses a small virtual FdHandler seam: one indirect call per readiness).

#if defined(__linux__)
#define ELAINA_HAS_EPOLL 1
#else
#define ELAINA_HAS_EPOLL 0
#endif

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace elaina {

enum class Interest : std::uint8_t { Read = 1, Write = 2, ReadWrite = 3 };

struct FdEvent {
  int fd = -1;
  bool readable = false;
  bool writable = false;
  bool error = false;
};

// Level-triggered reactor interface.
class Reactor {
 public:
  virtual ~Reactor() = default;
  virtual bool add(int fd, Interest interest) = 0;
  virtual bool modify(int fd, Interest interest) = 0;
  virtual bool remove(int fd) = 0;
  // Returns number of events; -1 on fatal error.
  virtual int poll(std::vector<FdEvent>& out, int timeout_ms) = 0;
};

#if ELAINA_HAS_EPOLL
class EpollReactor : public Reactor {
 public:
  EpollReactor();
  ~EpollReactor() override;
  EpollReactor(const EpollReactor&) = delete;
  EpollReactor& operator=(const EpollReactor&) = delete;

  bool add(int fd, Interest interest) override;
  bool modify(int fd, Interest interest) override;
  bool remove(int fd) override;
  int poll(std::vector<FdEvent>& out, int timeout_ms) override;
  bool valid() const noexcept { return epfd_ >= 0; }

 private:
  int epfd_ = -1;
};
#endif

class PollReactor : public Reactor {
 public:
  PollReactor() = default;
  bool add(int fd, Interest interest) override;
  bool modify(int fd, Interest interest) override;
  bool remove(int fd) override;
  int poll(std::vector<FdEvent>& out, int timeout_ms) override;

 private:
  struct Entry {
    int fd = -1;
    Interest interest = Interest::Read;
  };
  std::vector<Entry> entries_;
};

// Factory: best available reactor for this platform.
inline std::unique_ptr<Reactor> make_default_reactor() {
#if ELAINA_HAS_EPOLL
  auto r = std::make_unique<EpollReactor>();
  if (r->valid()) return r;
#endif
  return std::make_unique<PollReactor>();
}

}  // namespace elaina

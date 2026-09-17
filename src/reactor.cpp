#include "elaina/reactor.hpp"

#include <cerrno>
#include <cstring>

#if ELAINA_HAS_EPOLL
#include <sys/epoll.h>
#include <unistd.h>
#endif
#include <poll.h>
#include <unistd.h>

namespace elaina {

#if ELAINA_HAS_EPOLL
namespace {
uint32_t to_epoll(Interest i) {
  switch (i) {
    case Interest::Read: return EPOLLIN;
    case Interest::Write: return EPOLLOUT;
    case Interest::ReadWrite: return EPOLLIN | EPOLLOUT;
  }
  return EPOLLIN;
}
}  // namespace

EpollReactor::EpollReactor() {
  epfd_ = ::epoll_create1(EPOLL_CLOEXEC);
}

EpollReactor::~EpollReactor() {
  if (epfd_ >= 0) ::close(epfd_);
}

bool EpollReactor::add(int fd, Interest interest) {
  struct epoll_event ev {};
  ev.events = to_epoll(interest);
  ev.data.fd = fd;
  return ::epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) == 0;
}

bool EpollReactor::modify(int fd, Interest interest) {
  struct epoll_event ev {};
  ev.events = to_epoll(interest);
  ev.data.fd = fd;
  return ::epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) == 0;
}

bool EpollReactor::remove(int fd) {
  // Ignore errors (fd may already be closed).
  ::epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
  return true;
}

int EpollReactor::poll(std::vector<FdEvent>& out, int timeout_ms) {
  constexpr int kMaxEvents = 256;
  struct epoll_event evs[kMaxEvents];
  int n = ::epoll_wait(epfd_, evs, kMaxEvents, timeout_ms);
  if (n < 0) {
    if (errno == EINTR) return 0;
    return -1;
  }
  for (int i = 0; i < n; ++i) {
    FdEvent e;
    e.fd = evs[i].data.fd;
    e.readable = (evs[i].events & EPOLLIN) != 0;
    e.writable = (evs[i].events & EPOLLOUT) != 0;
    e.error = (evs[i].events & (EPOLLERR | EPOLLHUP)) != 0;
    out.push_back(e);
  }
  return n;
}
#endif

bool PollReactor::add(int fd, Interest interest) {
  for (auto& e : entries_) {
    if (e.fd == fd) {
      e.interest = interest;
      return true;
    }
  }
  entries_.push_back({fd, interest});
  return true;
}

bool PollReactor::modify(int fd, Interest interest) { return add(fd, interest); }

bool PollReactor::remove(int fd) {
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    if (entries_[i].fd == fd) {
      entries_[i] = entries_.back();
      entries_.pop_back();
      return true;
    }
  }
  return true;
}

int PollReactor::poll(std::vector<FdEvent>& out, int timeout_ms) {
  if (entries_.empty()) {
    // Sleep to avoid busy loop.
    struct pollfd dummy {};
    ::poll(&dummy, 0, timeout_ms);
    return 0;
  }
  std::vector<struct pollfd> pfds;
  pfds.reserve(entries_.size());
  for (auto& e : entries_) {
    struct pollfd p {};
    p.fd = e.fd;
    p.events = 0;
    if (e.interest == Interest::Read || e.interest == Interest::ReadWrite) p.events |= POLLIN;
    if (e.interest == Interest::Write || e.interest == Interest::ReadWrite) p.events |= POLLOUT;
    pfds.push_back(p);
  }
  int n = ::poll(pfds.data(), pfds.size(), timeout_ms);
  if (n < 0) {
    if (errno == EINTR) return 0;
    return -1;
  }
  for (auto& p : pfds) {
    if (p.revents != 0) {
      FdEvent e;
      e.fd = p.fd;
      e.readable = (p.revents & POLLIN) != 0;
      e.writable = (p.revents & POLLOUT) != 0;
      e.error = (p.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0;
      out.push_back(e);
    }
  }
  return (int)out.size();
}

}  // namespace elaina

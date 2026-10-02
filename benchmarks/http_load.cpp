#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <atomic>

int main(int argc, char** argv) {
  const char* host = "127.0.0.1";
  int port = 18080;
  int nthreads = 8;
  int reqs_per_thread = 8000;
  if (argc > 1) port = atoi(argv[1]);
  if (argc > 2) nthreads = atoi(argv[2]);
  if (argc > 3) reqs_per_thread = atoi(argv[3]);
  const std::string req = "GET / HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n";
  std::atomic<long> total_ok{0};
  auto t0 = std::chrono::steady_clock::now();
  std::vector<std::thread> th;
  std::atomic<bool> fail{false};
  for (int t = 0; t < nthreads; ++t) {
    th.emplace_back([&, t]() {
      int fd = ::socket(AF_INET, SOCK_STREAM, 0);
      if (fd < 0) { fail = true; return; }
      struct sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(port);
      ::inet_pton(AF_INET, host, &addr.sin_addr);
      if (::connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        perror("connect"); fail = true; ::close(fd); return;
      }
      char buf[8192];
      std::string acc;
      acc.reserve(4096);
      for (int i = 0; i < reqs_per_thread; ++i) {
        ssize_t s = ::send(fd, req.data(), req.size(), MSG_NOSIGNAL);
        if (s != (ssize_t)req.size()) { fail = true; break; }
        // read one response: headers + Content-Length: 11
        acc.clear();
        // read until \r\n\r\n
        while (acc.find("\r\n\r\n") == std::string::npos) {
          ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
          if (n <= 0) { fail = true; goto done; }
          acc.append(buf, n);
        }
        size_t he = acc.find("\r\n\r\n");
        // find Content-Length
        size_t clp = acc.find("Content-Length:");
        int cl = 11;
        if (clp != std::string::npos) cl = atoi(acc.c_str() + clp + 15);
        size_t have = acc.size() - (he + 4);
        while ((int)have < cl) {
          ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
          if (n <= 0) { fail = true; goto done; }
          have += n;
          acc.append(buf, n);
        }
        total_ok.fetch_add(1, std::memory_order_relaxed);
      }
    done:
      ::close(fd);
    });
  }
  for (auto& x : th) x.join();
  auto t1 = std::chrono::steady_clock::now();
  double s = std::chrono::duration<double>(t1 - t0).count();
  long ok = total_ok.load();
  printf("ok=%ld dur=%.3fs rps=%.0f threads=%d per=%d fail=%d\n", ok, s, ok / s, nthreads, reqs_per_thread, (int)fail.load());
  return fail ? 1 : 0;
}

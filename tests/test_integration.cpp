// End-to-end: boot Server on ephemeral port, speak raw HTTP over socket.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include <elaina/elaina.hpp>

#include "test_main.hpp"

namespace {

int connect_to(int port) {
  int fd = ::socket(AF_INET6, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  struct sockaddr_in6 addr {};
  addr.sin6_family = AF_INET6;
  addr.sin6_port = htons((uint16_t)port);
  addr.sin6_addr = in6addr_loopback;
  // Retry briefly (loop thread startup).
  for (int i = 0; i < 50; ++i) {
    if (::connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) return fd;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  ::close(fd);
  return -1;
}

std::string roundtrip(int port, const std::string& req) {
  int fd = connect_to(port);
  if (fd < 0) return "";
  ::send(fd, req.data(), req.size(), 0);
  std::string out;
  char buf[8192];
  // Read until we have full headers + declared body (or timeout).
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    struct timeval tv {0, 100000};
    int r = ::select(fd + 1, &rfds, nullptr, nullptr, &tv);
    if (r > 0 && FD_ISSET(fd, &rfds)) {
      ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
      if (n <= 0) break;
      out.append(buf, buf + n);
      // crude completion: headers done + body length satisfied or no CL.
      auto h = out.find("\r\n\r\n");
      if (h != std::string::npos) {
        auto cl = out.find("Content-Length:");
        if (cl == std::string::npos) break;
        int len = std::atoi(out.c_str() + cl + 15);
        if ((int)(out.size() - (h + 4)) >= len) break;
      }
    }
  }
  ::close(fd);
  return out;
}

}  // namespace

void run_integration() {
  elaina::Server app;
  app.threads(1);
  app.get("/", [](elaina::Context&) { return elaina::Response::text("Hello World"); });
  app.get("/users/:id", [](elaina::Context& ctx) {
    return elaina::Response::text(std::string("user:") + std::string(ctx.param("id")));
  });
  app.post("/echo", [](elaina::Context& ctx) { return elaina::Response::text(ctx.body()); });

  int port = app.listen_ephemeral();
  CHECK(port > 0);

  {
    std::string res = roundtrip(port, "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    CHECK(res.find("200 OK") != std::string::npos);
    CHECK(res.find("Hello World") != std::string::npos);
  }
  {
    std::string res = roundtrip(port, "GET /users/42 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    CHECK(res.find("200 OK") != std::string::npos);
    CHECK(res.find("user:42") != std::string::npos);
  }
  {
    std::string res = roundtrip(
        port, "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello");
    CHECK(res.find("200 OK") != std::string::npos);
    CHECK(res.find("hello") != std::string::npos);
  }
  {
    std::string res = roundtrip(port, "GET /nope HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    CHECK(res.find("404") != std::string::npos);
  }
  {
    std::string res = roundtrip(port, "DELETE / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    CHECK(res.find("405") != std::string::npos);
  }

  app.stop();
}

TEST_MAIN(integration)

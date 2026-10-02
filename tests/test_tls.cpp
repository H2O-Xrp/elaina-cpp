// TLS end-to-end: ephemeral HTTPS server + OpenSSL client.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include <elaina/elaina.hpp>

#include "test_main.hpp"

#if defined(ELAINA_HAS_TLS)

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

namespace {

const char* kCrt = "/tmp/elaina_test_tls.crt";
const char* kKey = "/tmp/elaina_test_tls.key";

bool make_cert() {
  std::string cmd =
      "openssl req -x509 -newkey rsa:2048 -keyout ";
  cmd += kKey;
  cmd += " -out ";
  cmd += kCrt;
  cmd += " -days 1 -nodes -subj \"/CN=localhost\" >/dev/null 2>&1";
  if (std::system(cmd.c_str()) != 0) return false;
  FILE* f = std::fopen(kCrt, "r");
  if (!f) return false;
  std::fclose(f);
  f = std::fopen(kKey, "r");
  if (!f) return false;
  std::fclose(f);
  return true;
}

int tcp_connect(int port) {
  int fd = ::socket(AF_INET6, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  struct sockaddr_in6 addr {};
  addr.sin6_family = AF_INET6;
  addr.sin6_port = htons((uint16_t)port);
  addr.sin6_addr = in6addr_loopback;
  for (int i = 0; i < 50; ++i) {
    if (::connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) return fd;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  ::close(fd);
  return -1;
}

std::string https_get(int port, const std::string& target) {
  int fd = tcp_connect(port);
  if (fd < 0) return "";
  SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
  if (!ctx) {
    ::close(fd);
    return "";
  }
  SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
  SSL* ssl = SSL_new(ctx);
  std::string out;
  if (ssl && SSL_set_fd(ssl, fd) == 1 && SSL_connect(ssl) == 1) {
    std::string req = "GET " + target +
                      " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    SSL_write(ssl, req.data(), (int)req.size());
    char buf[8192];
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
      int n = SSL_read(ssl, buf, sizeof(buf));
      if (n > 0) {
        out.append(buf, n);
        auto h = out.find("\r\n\r\n");
        if (h != std::string::npos) {
          auto cl = out.find("Content-Length:");
          if (cl == std::string::npos) break;
          int len = std::atoi(out.c_str() + cl + 15);
          if ((int)(out.size() - (h + 4)) >= len) break;
        }
      } else {
        int e = SSL_get_error(ssl, n);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) continue;
        break;
      }
    }
  }
  if (ssl) {
    SSL_shutdown(ssl);
    SSL_free(ssl);
  }
  SSL_CTX_free(ctx);
  ::close(fd);
  return out;
}

}  // namespace

void run_tls() {
  if (!make_cert()) {
    std::printf("tls: openssl CLI unavailable, SKIP\n");
    return;
  }
  elaina::Server app;
  app.threads(1);
  app.tls(kCrt, kKey);
  CHECK(app.has_tls());
  app.get("/", [](elaina::Context&) { return elaina::Response::text("secure hello"); });
  app.get("/users/:id", [](elaina::Context& ctx) {
    return elaina::Response::json(elaina::json::obj({{"id", std::string(ctx.param("id"))}}));
  });
  app.not_found([](elaina::Context&) { return elaina::Response::not_found("tls-404"); });

  int port = app.listen_ephemeral();
  CHECK(port > 0);

  {
    std::string res = https_get(port, "/");
    CHECK(res.find("200 OK") != std::string::npos);
    CHECK(res.find("secure hello") != std::string::npos);
  }
  {
    std::string res = https_get(port, "/users/42");
    CHECK(res.find("200 OK") != std::string::npos);
    CHECK(res.find("\"id\":\"42\"") != std::string::npos);
  }
  {
    std::string res = https_get(port, "/nope");
    CHECK(res.find("404") != std::string::npos);
    CHECK(res.find("tls-404") != std::string::npos);
  }
  {
    // Plain HTTP garbage must not crash the HTTPS server.
    int fd = tcp_connect(port);
    CHECK(fd >= 0);
    const char* junk = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    ::send(fd, junk, std::strlen(junk), 0);
    ::close(fd);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::string res = https_get(port, "/");
    CHECK(res.find("secure hello") != std::string::npos);
  }
  {
    // Bad cert path must fail fast.
    elaina::Server bad;
    bad.tls("/tmp/does-not-exist.crt", "/tmp/does-not-exist.key");
    bool threw = false;
    try {
      bad.listen_ephemeral();
    } catch (const std::runtime_error&) {
      threw = true;
    }
    CHECK(threw);
  }

  app.stop();
}

#else

void run_tls() { std::printf("tls: built without OpenSSL, SKIP\n"); }

#endif

TEST_MAIN(tls)

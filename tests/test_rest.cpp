// REST + middleware built-in tests: pure unit + live HTTP roundtrip.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

#include <elaina/elaina.hpp>

#include "test_main.hpp"

namespace {

elaina::Response call(elaina::Middleware& mw, elaina::Context& ctx, std::string_view body = "ok") {
  elaina::Handler term = [b = std::string(body)](elaina::Context&) {
    return elaina::Response::text(b);
  };
  return mw(ctx, [&](elaina::Context& c) -> elaina::Response { return term(c); });
}

int connect_to(int port) {
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

std::string roundtrip(int port, const std::string& req) {
  int fd = connect_to(port);
  if (fd < 0) return "";
  ::send(fd, req.data(), req.size(), 0);
  std::string out;
  char buf[8192];
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

void run_rest() {
  // ---- json::parse ----
  {
    auto v = elaina::json::parse(R"({"name":"ada","age":30,"admin":true})");
    CHECK(v.has_value());
    auto* o = std::get_if<elaina::json::Object>(&v->data);
    CHECK(o != nullptr);
    CHECK(elaina::json::get_string(*elaina::json::find(*o, "name")) == "ada");
    CHECK(elaina::json::get_int(*elaina::json::find(*o, "age")) == 30);
    CHECK(elaina::json::get_bool(*elaina::json::find(*o, "admin")) == true);
    CHECK(!elaina::json::parse("{oops").has_value());
    CHECK(!elaina::json::parse("").has_value());
  }
  // ---- Response helpers ----
  {
    auto r = elaina::Response::json(elaina::json::obj({{"id", 1}}));
    CHECK(r.status == elaina::Status::Ok);
    CHECK(r.content_type == "application/json");
    CHECK(r.body == "{\"id\":1}");
    CHECK(elaina::Response::created("x").status == elaina::Status::Created);
    CHECK(elaina::Response::json_created(elaina::json::obj({})).status ==
          elaina::Status::Created);
    CHECK(elaina::Response::no_content().status == elaina::Status::NoContent);
    CHECK(elaina::Response::bad_request().status == elaina::Status::BadRequest);
    CHECK(elaina::Response::unauthorized().status == elaina::Status::Unauthorized);
    CHECK(elaina::Response::forbidden().status == elaina::Status::Forbidden);
    CHECK(elaina::Response::not_found().status == elaina::Status::NotFound);
    CHECK(elaina::Response::internal().status == elaina::Status::InternalError);
    auto red = elaina::Response::redirect("/login");
    CHECK(red.status == elaina::Status::Found);
    bool has_loc = false;
    for (auto& [k, v] : red.headers)
      if (k == "Location" && v == "/login") has_loc = true;
    CHECK(has_loc);
  }
  // ---- Context helpers ----
  {
    elaina::Request req;
    req.path = "/users/42";
    req.body = R"({"a":1})";
    req.headers = {{"Content-Type", "application/json; charset=utf-8"},
                   {"Authorization", "Bearer tok123"},
                   {"Cookie", "sess=abc; theme=dark"}};
    elaina::ParamMap pm;
    pm.add("id", "42");
    elaina::Context ctx(req, pm);
    CHECK(ctx.is_json());
    auto j = ctx.json();
    CHECK(j.has_value());
    CHECK(ctx.param_int("id", 0) == 42);
    CHECK(ctx.param_int("missing", 7) == 7);
    CHECK(ctx.bearer_token() == std::optional<std::string_view>("tok123"));
    CHECK(ctx.cookie("theme") == std::optional<std::string_view>("dark"));
    CHECK(!ctx.cookie("nope").has_value());
    CHECK(ctx.content_type() == "application/json");
  }
  {
    // query_int / query_double need a parsed query: build via parser.
    elaina::StrictHttpParser p;
    const char* raw = "GET /s?page=3&x=1.5 HTTP/1.1\r\nHost: h\r\n\r\n";
    auto pr = p.parse(raw, std::strlen(raw));
    CHECK(pr.status == elaina::ParseStatus::Complete);
    elaina::ParamMap pm;
    elaina::Context ctx(pr.request, pm);
    CHECK(ctx.query_int("page", 1) == 3);
    CHECK(ctx.query_int("missing", 9) == 9);
    CHECK(ctx.query_int("x", 9) == 9);  // not an int
    CHECK(ctx.query_double("x", 0.0) == 1.5);
  }
  // ---- Middleware unit ----
  {
    elaina::Request req;
    req.path = "/";
    elaina::ParamMap pm;
    elaina::Context ctx(req, pm);
    auto mw = elaina::RequestId();
    elaina::Response r = call(mw, ctx);
    bool has_id = false;
    for (auto& [k, v] : r.headers)
      if (k == "X-Request-Id" && !v.empty()) has_id = true;
    CHECK(has_id);
  }
  {
    elaina::Request req;
    req.path = "/";
    elaina::ParamMap pm;
    elaina::Context ctx(req, pm);
    auto mw = elaina::Recovery();
    elaina::Middleware boom = [](elaina::Context&, elaina::Next) -> elaina::Response {
      throw std::runtime_error("kaboom");
    };
    (void)boom;
    elaina::Response r = mw(ctx, [](elaina::Context&) -> elaina::Response {
      throw std::runtime_error("kaboom");
    });
    CHECK(r.status == elaina::Status::InternalError);
  }
  {
    elaina::Request req;
    req.path = "/";
    elaina::ParamMap pm;
    elaina::Context ctx(req, pm, "10.0.0.9");
    auto mw = elaina::RateLimit(2, std::chrono::seconds(60));
    CHECK(call(mw, ctx).status == elaina::Status::Ok);
    CHECK(call(mw, ctx).status == elaina::Status::Ok);
    auto r3 = call(mw, ctx);
    CHECK(r3.status == elaina::Status::TooManyRequests);
    bool retry = false;
    for (auto& [k, v] : r3.headers)
      if (k == "Retry-After") retry = true;
    CHECK(retry);
  }
  {
    elaina::Request req;
    req.path = "/";
    req.headers = {{"Authorization", "Basic YWxhZGRpbjpvcGVuc2VzYW1l"}};  // aladdin:opensesame
    elaina::ParamMap pm;
    elaina::Context ctx(req, pm);
    auto mw = elaina::BasicAuth("aladdin", "opensesame");
    CHECK(call(mw, ctx).status == elaina::Status::Ok);
    auto mw2 = elaina::BasicAuth("aladdin", "wrong");
    CHECK(call(mw2, ctx).status == elaina::Status::Unauthorized);
  }
  {
    elaina::Request req;
    req.path = "/";
    req.headers = {{"Authorization", "Bearer s3cr3t"}};
    elaina::ParamMap pm;
    elaina::Context ctx(req, pm);
    auto mw = elaina::BearerAuth("s3cr3t");
    CHECK(call(mw, ctx).status == elaina::Status::Ok);
    auto mw2 = elaina::BearerAuth("other");
    CHECK(call(mw2, ctx).status == elaina::Status::Unauthorized);
  }
  {
    const char* good =
        "Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9."
        "eyJzdWIiOiJ1MSIsImV4cCI6MjAwMDAwMDAwMH0."
        "7jiyP1_4A-x83vt7ze78W1vaBMRPlClkJqHUyHEMYxQ";
    elaina::Request req;
    req.path = "/";
    std::string auth = good;
    req.headers = {{"Authorization", std::string_view(auth)}};
    elaina::ParamMap pm;
    elaina::Context ctx(req, pm);
    auto mw = elaina::JwtHs256Auth("test-secret");
    CHECK(call(mw, ctx).status == elaina::Status::Ok);
    auto mw2 = elaina::JwtHs256Auth("wrong-secret");
    CHECK(call(mw2, ctx).status == elaina::Status::Unauthorized);
  }
  // ---- Live: group / resource / health / not_found / verbs ----
  {
    elaina::Server app;
    app.threads(1);
    app.use(elaina::RequestId());
    app.health();
    app.not_found([](elaina::Context&) { return elaina::Response::not_found("custom-404"); });
    app.group("/api/v1", [](elaina::RouteGroup& api) {
      api.get("/users", [](elaina::Context&) {
        return elaina::Response::json(elaina::json::obj({{"users", elaina::json::Array{}}}));
      });
      api.post("/users", [](elaina::Context& ctx) {
        auto v = ctx.json();
        if (!v) return elaina::Response::bad_request("invalid json");
        return elaina::Response::json_created(*v);
      });
      api.group("/admin", [](elaina::RouteGroup& a) {
        a.get("/stats", [](elaina::Context&) { return elaina::Response::text("stats"); });
      });
    });
    elaina::ResourceHandlers items;
    items.list = [](elaina::Context&) { return elaina::Response::json("[]"); };
    items.show = [](elaina::Context& ctx) {
      return elaina::Response::text(ctx.param("id"));
    };
    items.create = [](elaina::Context& ctx) { return elaina::Response::created(ctx.body()); };
    app.resource("/items", std::move(items));
    app.any("/ping", [](elaina::Context&) { return elaina::Response::text("pong"); });

    int port = app.listen_ephemeral();
    CHECK(port > 0);

    auto g = [&](const std::string& target) {
      return roundtrip(port, "GET " + target + " HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    };
    CHECK(g("/health").find("\"status\":\"ok\"") != std::string::npos);
    CHECK(g("/api/v1/users").find("application/json") != std::string::npos);
    CHECK(g("/api/v1/admin/stats").find("stats") != std::string::npos);
    CHECK(g("/items").find("[]") != std::string::npos);
    CHECK(g("/items/7").find("7") != std::string::npos);
    CHECK(g("/ping").find("pong") != std::string::npos);
    {
      std::string res = roundtrip(port, "POST /api/v1/users HTTP/1.1\r\nHost: x\r\nContent-Type: "
                                        "application/json\r\nContent-Length: 7\r\nConnection: "
                                        "close\r\n\r\n{\"a\":1}");
      CHECK(res.find("201 Created") != std::string::npos);
      CHECK(res.find("{\"a\":1}") != std::string::npos);
    }
    {
      std::string res = roundtrip(port, "POST /api/v1/users HTTP/1.1\r\nHost: x\r\nContent-Length: "
                                        "3\r\nConnection: close\r\n\r\n{{{");
      CHECK(res.find("400") != std::string::npos);
    }
    {
      std::string res = g("/tidak-ada");
      CHECK(res.find("404") != std::string::npos);
      CHECK(res.find("custom-404") != std::string::npos);
      CHECK(res.find("X-Request-Id:") != std::string::npos);  // middleware ran on 404
    }
    {
      std::string res =
          roundtrip(port, "DELETE /ping HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
      CHECK(res.find("pong") != std::string::npos);  // any()
    }
    app.stop();
  }
  // ---- Live: static_files ----
  {
    std::string dir = "/tmp/elaina_static_test";
    ::system(("rm -rf " + dir + " && mkdir -p " + dir).c_str());
    {
      std::ofstream f(dir + "/app.js");
      f << "console.log(1);";
    }
    elaina::Server app;
    app.threads(1);
    app.static_files("/static", dir);
    int port = app.listen_ephemeral();
    CHECK(port > 0);
    auto g = [&](const std::string& target) {
      return roundtrip(port, "GET " + target + " HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    };
    std::string js = g("/static/app.js");
    CHECK(js.find("200 OK") != std::string::npos);
    CHECK(js.find("javascript") != std::string::npos);
    CHECK(js.find("console.log(1);") != std::string::npos);
    CHECK(g("/static/../secret").find("403") != std::string::npos);
    CHECK(g("/static/missing.js").find("404") != std::string::npos);
    app.stop();
    ::system(("rm -rf " + dir).c_str());
  }
}

TEST_MAIN(rest)

#include <elaina/elaina.hpp>

#include "test_main.hpp"

void run_http_codec() {
  elaina::StrictHttpParser p;

  // Simple GET.
  {
    const char* raw = "GET /hello?q=1 HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n";
    auto r = p.parse(raw, __builtin_strlen(raw));
    CHECK(r.status == elaina::ParseStatus::Complete);
    CHECK(r.request.method == elaina::Method::GET);
    CHECK(r.request.path == "hello" || r.request.path == "/hello");
    CHECK(r.request.query == "q=1");
    CHECK(r.request.keep_alive);
  }
  // POST with body.
  {
    const char* raw =
        "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello";
    auto r = p.parse(raw, __builtin_strlen(raw));
    CHECK(r.status == elaina::ParseStatus::Complete);
    CHECK(r.request.body == "hello");
  }
  // NeedMore on partial.
  {
    const char* raw = "GET / HTTP/1.1\r\nHost: x\r\n";
    auto r = p.parse(raw, __builtin_strlen(raw));
    CHECK(r.status == elaina::ParseStatus::NeedMore);
  }
  // Bad request line.
  {
    const char* raw = "BADLINE\r\n\r\n";
    auto r = p.parse(raw, __builtin_strlen(raw));
    CHECK(r.status == elaina::ParseStatus::Error);
  }
  // Smuggling guard: both CL + chunked.
  {
    const char* raw =
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nTransfer-Encoding: "
        "chunked\r\n\r\nhello";
    auto r = p.parse(raw, __builtin_strlen(raw));
    CHECK(r.status == elaina::ParseStatus::Error);
  }
  // Oversized body.
  {
    elaina::ServerLimits lim;
    lim.max_body_size = 4;
    elaina::StrictHttpParser p2(lim);
    const char* raw =
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 100\r\n\r\nxxxx";
    auto r = p2.parse(raw, __builtin_strlen(raw));
    CHECK(r.status == elaina::ParseStatus::TooLarge);
  }
  // Duplicate content-length.
  {
    const char* raw =
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nContent-Length: "
        "1\r\n\r\na";
    auto r = p.parse(raw, __builtin_strlen(raw));
    CHECK(r.status == elaina::ParseStatus::Error);
  }
  // Encode sanity.
  {
    auto res = elaina::Response::text("hi");
    std::string enc = elaina::encode_response(res, true, "HTTP/1.1");
    CHECK(enc.find("200 OK") != std::string::npos);
    CHECK(enc.find("Content-Length: 2") != std::string::npos);
    CHECK(enc.substr(enc.size() - 2) == "hi");
  }
  // Context query parsing.
  {
    const char* raw = "GET /s?q=hello&n=2 HTTP/1.1\r\nHost: x\r\n\r\n";
    auto r = p.parse(raw, __builtin_strlen(raw));
    CHECK(r.status == elaina::ParseStatus::Complete);
    elaina::ParamMap pm;
    elaina::Context ctx(r.request, pm);
    CHECK(ctx.query("q") == std::optional<std::string_view>("hello"));
    CHECK(ctx.query_or("missing", "d") == "d");
  }
}

TEST_MAIN(http_codec)

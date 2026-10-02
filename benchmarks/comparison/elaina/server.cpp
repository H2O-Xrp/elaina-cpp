// Elaina comparison server: implements all 18 benchmark workloads (see BENCHMARK.md).
// Run: ./elaina_server [port]  (default 3000, threads=auto)
#include <elaina/elaina.hpp>
#include <string>

static std::string large_body;

int main(int argc, char** argv) {
  int port = argc > 1 ? atoi(argv[1]) : 3000;
  large_body.assign(1024 * 1024, 'x');

  elaina::Server app;
  // threads(0) = auto = hardware_concurrency (multi-core scaling bench varies this)
  app.get("/", [](elaina::Context&) { return elaina::Response::text("Hello World"); });
  app.get("/static", [](elaina::Context&) { return elaina::Response::text("static"); });
  app.get("/users/:id", [](elaina::Context& c) { return elaina::Response::text(c.param("id")); });
  app.get("/users/:id/posts/:postId", [](elaina::Context& c) {
    std::string b;
    b.reserve(32);
    auto a = c.param("id");
    auto b2 = c.param("postId");
    b.append(a.data(), a.size());
    b += '/';
    b.append(b2.data(), b2.size());
    return elaina::Response::text(b);
  });
  app.get("/search", [](elaina::Context& c) {
    auto q = c.query_or("q", "");
    auto n = c.query_or("n", "");
    std::string b;
    b.append(q.data(), q.size());
    b += ':';
    b.append(n.data(), n.size());
    return elaina::Response::text(b);
  });
  app.get("/headers", [](elaina::Context& c) {
    return elaina::Response::text(c.header_or("x-test", "missing"));
  });
  app.get("/json", [](elaina::Context&) {
    return elaina::Response::json(R"({"id":42,"name":"elaina","ok":true})");
  });
  app.post("/json", [](elaina::Context& c) { return elaina::Response::json(c.body()); });
  app.post("/echo", [](elaina::Context& c) { return elaina::Response::text(c.body()); });
  app.get("/bad", [](elaina::Context&) {
    return elaina::Response::text("Bad Request", elaina::Status::BadRequest);
  });
  app.get("/large", [](elaina::Context&) { return elaina::Response::text(large_body); });
  // 404 handled by framework (no route). 405 via wrong method.
  app.listen(port);
  return 0;
}

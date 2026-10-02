// HTTPS example: same handler style, TLS on top.
//
// Generate a self-signed cert for local testing:
//   openssl req -x509 -newkey rsa:2048 -keyout server.key -out server.crt \
//     -days 365 -nodes -subj "/CN=localhost"
//
//   ./https_example            # :3443 with ./server.crt ./server.key
//   ./https_example 8443 /path/cert.pem /path/key.pem
//   curl -k https://localhost:3443/
//   curl -k https://localhost:3443/api/hello?name=ada
#include <elaina/elaina.hpp>

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
  int port = argc > 1 ? std::atoi(argv[1]) : 3443;
  std::string cert = argc > 2 ? argv[2] : "server.crt";
  std::string key = argc > 3 ? argv[3] : "server.key";

  elaina::Server app;
  app.threads(2);
  app.use(elaina::Logger());
  app.use(elaina::Cors());
  app.use(elaina::RequestId());
  app.use(elaina::Recovery());

  app.health();
  app.not_found([](elaina::Context&) {
    return elaina::Response::not_found(R"({"error":"not found"})");
  });

  app.get("/", [](elaina::Context&) -> elaina::Response {
    return elaina::Response::text("secure hello");
  });

  app.group("/api", [](elaina::RouteGroup& api) {
    api.get("/hello", [](elaina::Context& ctx) -> elaina::Response {
      std::string who(ctx.query_or("name", "world"));
      return elaina::Response::json(
          elaina::json::obj({{"hello", who}}));
    });
    api.post("/echo", [](elaina::Context& ctx) -> elaina::Response {
      auto v = ctx.json();
      if (!v) return elaina::Response::bad_request(R"({"error":"invalid json"})");
      return elaina::Response::json(*v);
    });
  });

  std::printf("https example on :%d (cert=%s key=%s)\n", port, cert.c_str(), key.c_str());
#if defined(ELAINA_HAS_TLS)
  app.tls(cert, key).listen(port);
#else
  std::printf("built without TLS support; serving plain HTTP instead\n");
  app.listen(port);
#endif
  return 0;
}

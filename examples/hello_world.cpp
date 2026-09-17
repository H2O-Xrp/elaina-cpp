#include <elaina/elaina.hpp>

#include <cstdio>

int main() {
  elaina::Server app;
  app.threads(2);
  app.use(elaina::Logger());

  app.get("/", [](elaina::Context& ctx) -> elaina::Response {
    (void)ctx;
    return elaina::Response::text("Hello World");
  });

  app.get("/users/:id", [](elaina::Context& ctx) -> elaina::Response {
    std::string body = "user:";
    body += ctx.param("id");
    auto q = ctx.query("verbose");
    if (q && *q == "1") body += " (verbose)";
    return elaina::Response::text(body);
  });

  app.post("/echo", [](elaina::Context& ctx) -> elaina::Response {
    return elaina::Response::text(ctx.body());
  });

  std::printf("listening on :3000 (threads=auto)\n");
  app.listen(3000);
  return 0;
}

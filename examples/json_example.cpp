#include <elaina/elaina.hpp>

#include <cstdio>
#include "../modules/json/json.hpp"

int main() {
  elaina::Server app;
  app.threads(2);
  app.use(elaina::Logger());
  app.use(elaina::Cors());

  app.get("/", [](elaina::Context&) -> elaina::Response {
    elaina::json::Value v = elaina::json::Value(
        elaina::json::obj({{"hello", "world"}, {"framework", "elaina-cpp"}}));
    return elaina::Response::json(elaina::json::stringify(v));
  });

  app.get("/users/:id", [](elaina::Context& ctx) -> elaina::Response {
    std::string id(ctx.param("id"));
    elaina::json::Value v = elaina::json::Value(elaina::json::obj({
        {"id", elaina::json::Value(id)},
        {"verbose", elaina::json::Value(ctx.query_or("verbose", "0") == "1")},
    }));
    return elaina::Response::json(elaina::json::stringify(v));
  });

  std::printf("json example on :3001\n");
  app.listen(3001);
  return 0;
}

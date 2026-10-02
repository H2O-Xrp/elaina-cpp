// REST API example: same handler style, with group + JSON + middleware.
//
//   ./rest_example            # :3002
//   curl :3002/health
//   curl :3002/api/v1/users
//   curl -X POST :3002/api/v1/users -H 'Content-Type: application/json' -d '{"name":"ada"}'
//   curl :3002/api/v1/users/1
//   curl -X PUT :3002/api/v1/users/1 -d '{"name":"budi"}'
//   curl -X DELETE :3002/api/v1/users/1
#include <elaina/elaina.hpp>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <unordered_map>

namespace {

std::mutex g_mu;
std::unordered_map<std::string, std::string> g_users;  // id -> name
std::atomic<int> g_next{1};

elaina::Response users_to_json() {
  std::lock_guard<std::mutex> lk(g_mu);
  elaina::json::Array arr;
  for (auto& [id, name] : g_users)
    arr.push_back(elaina::json::Value(elaina::json::obj({{"id", id}, {"name", name}})));
  return elaina::Response::json(arr);
}

}  // namespace

int main() {
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

  app.group("/api/v1", [](elaina::RouteGroup& api) {
    api.get("/users", [](elaina::Context& ctx) -> elaina::Response {
      (void)ctx;
      return users_to_json();
    });
    api.post("/users", [](elaina::Context& ctx) -> elaina::Response {
      auto v = ctx.json();
      if (!v) return elaina::Response::bad_request(R"({"error":"invalid json"})");
      auto* o = std::get_if<elaina::json::Object>(&v->data);
      if (!o) return elaina::Response::bad_request(R"({"error":"object expected"})");
      auto* name = elaina::json::find(*o, "name");
      auto s = name ? elaina::json::get_string(*name) : std::nullopt;
      if (!s || s->empty()) return elaina::Response::bad_request(R"({"error":"name required"})");
      std::string id = std::to_string(g_next.fetch_add(1));
      {
        std::lock_guard<std::mutex> lk(g_mu);
        g_users.emplace(id, std::string(*s));
      }
      return elaina::Response::json_created(
          elaina::json::obj({{"id", id}, {"name", std::string(*s)}}));
    });
    api.get("/users/:id", [](elaina::Context& ctx) -> elaina::Response {
      std::string id(ctx.param("id"));
      std::lock_guard<std::mutex> lk(g_mu);
      auto it = g_users.find(id);
      if (it == g_users.end()) return elaina::Response::not_found(R"({"error":"no such user"})");
      return elaina::Response::json(elaina::json::obj({{"id", it->first}, {"name", it->second}}));
    });
    api.put("/users/:id", [](elaina::Context& ctx) -> elaina::Response {
      auto v = ctx.json();
      if (!v) return elaina::Response::bad_request(R"({"error":"invalid json"})");
      std::string id(ctx.param("id"));
      std::lock_guard<std::mutex> lk(g_mu);
      auto it = g_users.find(id);
      if (it == g_users.end()) return elaina::Response::not_found(R"({"error":"no such user"})");
      if (auto* o = std::get_if<elaina::json::Object>(&v->data)) {
        if (auto* n = elaina::json::find(*o, "name"))
          if (auto s = elaina::json::get_string(*n); s && !s->empty()) it->second = std::string(*s);
      }
      return elaina::Response::json(elaina::json::obj({{"id", it->first}, {"name", it->second}}));
    });
    api.del("/users/:id", [](elaina::Context& ctx) -> elaina::Response {
      std::string id(ctx.param("id"));
      std::lock_guard<std::mutex> lk(g_mu);
      if (!g_users.erase(id)) return elaina::Response::not_found(R"({"error":"no such user"})");
      return elaina::Response::no_content();
    });
  });

  std::printf("rest example on :3002\n");
  app.listen(3002);
  return 0;
}

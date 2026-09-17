#include <elaina/elaina.hpp>

#include "test_main.hpp"

void run_router() {
  elaina::Router r;
  CHECK(r.add(elaina::Method::GET, "/", [](elaina::Context&) {
    return elaina::Response::text("root");
  }));
  CHECK(r.add(elaina::Method::GET, "/users", [](elaina::Context&) {
    return elaina::Response::text("list");
  }));
  CHECK(r.add(elaina::Method::GET, "/users/:id", [](elaina::Context&) {
    return elaina::Response::text("one");
  }));
  CHECK(r.add(elaina::Method::GET, "/users/:id/posts", [](elaina::Context&) {
    return elaina::Response::text("posts");
  }));
  CHECK(r.add(elaina::Method::POST, "/users", [](elaina::Context&) {
    return elaina::Response::text("create");
  }));
  CHECK(r.add(elaina::Method::DELETE, "/users/:id", [](elaina::Context&) {
    return elaina::Response::text("del");
  }));
  // Duplicate must fail.
  CHECK(!r.add(elaina::Method::GET, "/users", [](elaina::Context&) {
    return elaina::Response::text("dup");
  }));

  auto m0 = r.match(elaina::Method::GET, "/");
  CHECK(m0.handler != nullptr);

  auto m1 = r.match(elaina::Method::GET, "/users");
  CHECK(m1.handler != nullptr);

  auto m2 = r.match(elaina::Method::GET, "/users/42");
  CHECK(m2.handler != nullptr);
  CHECK(m2.params.get("id") == std::optional<std::string_view>("42"));

  auto m3 = r.match(elaina::Method::GET, "/users/42/posts");
  CHECK(m3.handler != nullptr);
  CHECK(m3.params.get("id") == std::optional<std::string_view>("42"));

  // Static priority: /users/new should match static if registered, else param.
  CHECK(r.add(elaina::Method::GET, "/users/new", [](elaina::Context&) {
    return elaina::Response::text("new");
  }));
  auto m4 = r.match(elaina::Method::GET, "/users/new");
  CHECK(m4.handler != nullptr);

  // Method mismatch -> 405 signal.
  auto m5 = r.match(elaina::Method::POST, "/users/42");
  CHECK(m5.handler == nullptr);
  CHECK(m5.method_mismatch);

  // Not found.
  auto m6 = r.match(elaina::Method::GET, "/nope");
  CHECK(m6.handler == nullptr);
  CHECK(!m6.method_mismatch);

  // Wildcard.
  CHECK(r.add(elaina::Method::GET, "/static/*", [](elaina::Context&) {
    return elaina::Response::text("wild");
  }));
  auto m7 = r.match(elaina::Method::GET, "/static/a/b/c");
  CHECK(m7.handler != nullptr);

  // Query stripped.
  auto m8 = r.match(elaina::Method::GET, "/users/7?verbose=1");
  CHECK(m8.handler != nullptr);
}

TEST_MAIN(router)

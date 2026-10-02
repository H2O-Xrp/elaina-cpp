/// @file elaina.hpp
/// @brief Header utama: cukup include satu file ini.
///
/// @code
///   #include <elaina/elaina.hpp>
///   int main() {
///     elaina::Server app;
///     app.get("/", [](elaina::Context& ctx) -> elaina::Response {
///       return elaina::Response::text("Hello World");
///     });
///     app.listen(3000);
///   }
/// @endcode
///
/// Urutan include di bawah mencerminkan lapisan arsitektur:
/// fondasi (method/error/limits) -> HTTP (http/context) -> inti
/// (router/middleware/codec/reactor/tls/connection) -> server.
#pragma once

#include "elaina/method.hpp"
#include "elaina/error.hpp"
#include "elaina/limits.hpp"
#include "elaina/http.hpp"
#include "elaina/context.hpp"
#include "elaina/router.hpp"
#include "elaina/middleware.hpp"
#include "elaina/http_codec.hpp"
#include "elaina/reactor.hpp"
#include "elaina/tls.hpp"
#include "elaina/connection.hpp"
#include "elaina/server.hpp"

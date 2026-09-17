#pragma once

// HttpCodec abstraction: StrictHttpParser is the MVP built-in strict parser.
// It exposes an llhttp-compatible seam: Server/Connection depend on the
// Parser concept below, so a future LlhttpParser backend can be dropped in
// behind the same interface (see ADR-007).
//
// Supported subset (MVP): HTTP/1.0 + HTTP/1.1 request line, headers,
// Content-Length body, chunked transfer-encoding (assembled up to
// max_body_size), keep-alive semantics. Anything else -> strict 400.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "http.hpp"
#include "limits.hpp"
#include "method.hpp"

namespace elaina {

enum class ParseStatus {
  NeedMore,   // need more bytes
  Complete,   // one full request (head+body) parsed
  Error,      // malformed -> respond 400
  TooLarge,   // limit exceeded -> respond 413/431
};

struct ParseResult {
  ParseStatus status = ParseStatus::NeedMore;
  Request request;             // valid if Complete; views into buffer
  std::size_t consumed = 0;    // bytes consumed from input buffer
  std::string error;           // valid if Error/TooLarge
  Status http_status = Status::BadRequest;
};

class StrictHttpParser {
 public:
  explicit StrictHttpParser(const ServerLimits& limits = {}) : limits_(limits) {}

  // Tries to parse one request from [data, data+len).
  // On Complete, `consumed` includes head + body bytes.
  ParseResult parse(const char* data, std::size_t len) const noexcept;

 private:
  ServerLimits limits_;
};

// Encodes a Response into a flat buffer ready for send().
// Adds Content-Length, Content-Type, Connection, Date, Server headers.
std::string encode_response(const Response& res, bool keep_alive,
                            std::string_view request_version);

}  // namespace elaina

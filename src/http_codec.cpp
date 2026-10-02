/// @file http_codec.cpp
/// @brief Implementasi StrictHttpParser::parse_into + encoder respons.
///
/// parse_into(): request line, pemisah path/query, loop header dengan
/// dispatch cepat berdasar panjang nama (hindari 3x scan per header),
/// semantik keep-alive, penjaga smuggling, dan penolakan chunked (501).
/// encode_response_into(): append langsung tanpa string sementara
/// (versi via view, angka via to_chars).
#include "elaina/http_codec.hpp"

#include <charconv>
#include <cstring>

namespace elaina {
namespace {

inline bool is_ws(char c) noexcept { return c == ' ' || c == '\t'; }

inline std::string_view trim(std::string_view s) noexcept {
  std::size_t a = 0;
  while (a < s.size() && is_ws(s[a])) ++a;
  std::size_t b = s.size();
  while (b > a && is_ws(s[b - 1])) --b;
  return s.substr(a, b - a);
}

inline bool ieq(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z') x = static_cast<char>(x + 32);
    if (y >= 'A' && y <= 'Z') y = static_cast<char>(y + 32);
    if (x != y) return false;
  }
  return true;
}

}  // namespace

ParseStatus StrictHttpParser::parse_into(const char* data, std::size_t len,
                                           Request& out_req, std::size_t& consumed,
                                           std::string& error,
                                           Status& http_status) const noexcept {
  std::string_view buf(data, len);
  consumed = 0;
  // NOTE: error/http_status only set on Error/TooLarge; caller clears if needed.

  // Find end of headers: \r\n\r\n
  std::size_t head_end = buf.find("\r\n\r\n");
  bool has_crlf = true;
  if (head_end == std::string_view::npos) {
    // Tolerate bare \n\n? No — strict: require CRLF (security).
    if (buf.size() > limits_.max_header_size + 1024) {
      error = "header too large";
      http_status = Status::HeaderTooLarge;
      return ParseStatus::TooLarge;
    }
    return ParseStatus::NeedMore;
  }
  if (head_end > limits_.max_header_size) {
    error = "header too large";
    http_status = Status::HeaderTooLarge;
    return ParseStatus::TooLarge;
  }

  std::string_view head = buf.substr(0, head_end);
  // Request line = first line.
  std::size_t eol = head.find("\r\n");
  std::string_view req_line = (eol == std::string_view::npos) ? head : head.substr(0, eol);
  if (req_line.size() > limits_.max_uri_size + 64) {
    error = "request line too large";
    http_status = Status::HeaderTooLarge;
    return ParseStatus::TooLarge;
  }

  // METHOD SP TARGET SP VERSION
  std::size_t s1 = req_line.find(' ');
  if (s1 == std::string_view::npos) {
    error = "bad request line";
    http_status = Status::BadRequest;
    return ParseStatus::Error;
  }
  std::size_t s2 = req_line.find(' ', s1 + 1);
  if (s2 == std::string_view::npos) {
    error = "bad request line";
    http_status = Status::BadRequest;
    return ParseStatus::Error;
  }
  std::string_view method_sv = req_line.substr(0, s1);
  std::string_view target_sv = req_line.substr(s1 + 1, s2 - s1 - 1);
  std::string_view version_sv = trim(req_line.substr(s2 + 1));

  Method method;
  if (!parse_method(method_sv, method)) {
    error = "unsupported method";
    http_status = Status::BadRequest;
    return ParseStatus::Error;
  }
  if (target_sv.empty() || target_sv[0] != '/') {
    error = "bad target";
    http_status = Status::BadRequest;
    return ParseStatus::Error;
  }
  if (target_sv.size() > limits_.max_uri_size) {
    error = "uri too long";
    http_status = Status::HeaderTooLarge;
    return ParseStatus::TooLarge;
  }
  if (version_sv != "HTTP/1.1" && version_sv != "HTTP/1.0") {
    error = "unsupported version";
    http_status = Status::BadRequest;
    return ParseStatus::Error;
  }

  Request& req = out_req;
  req.headers.clear();
  // Reuse capacity across keep-alive requests on same connection:
  // 0 allocs after warmup (vs 1-3 allocs/req with fresh vector).
  if (req.headers.capacity() < 16) req.headers.reserve(16);
  req.method = method;
  req.method_raw = method_sv;
  req.target = target_sv;
  if (auto q = target_sv.find('?'); q != std::string_view::npos) {
    req.path = target_sv.substr(0, q);
    req.query = target_sv.substr(q + 1);
  } else {
    req.path = target_sv;
    req.query = {};
  }
  req.version = version_sv;

  // Headers.
  std::string_view rest = (eol == std::string_view::npos) ? std::string_view{} : head.substr(eol + 2);
  std::size_t count = 0;
  bool chunked = false;
  std::size_t content_length = 0;
  bool has_length = false;
  std::string_view connection_val{};

  while (!rest.empty()) {
    std::size_t nl = rest.find("\r\n");
    std::string_view line = (nl == std::string_view::npos) ? rest : rest.substr(0, nl);
    if (!line.empty()) {
      if (++count > limits_.max_headers) {
        error = "too many headers";
        http_status = Status::HeaderTooLarge;
        return ParseStatus::TooLarge;
      }
      std::size_t colon = line.find(':');
      if (colon == std::string_view::npos) {
        error = "bad header line";
        http_status = Status::BadRequest;
        return ParseStatus::Error;
      }
      std::string_view name = trim(line.substr(0, colon));
      std::string_view value = trim(line.substr(colon + 1));
      if (name.empty() || name.size() > 256 || value.size() > 8192) {
        error = "bad header field";
        http_status = Status::BadRequest;
        return ParseStatus::Error;
      }
      // Reject header injection / obs-fold.
      for (char c : name) {
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t' || c == ':') {
          error = "bad header name";
          http_status = Status::BadRequest;
          return ParseStatus::Error;
        }
      }
      for (char c : value) {
        if (c == '\r' || c == '\n') {
          error = "bad header value";
          http_status = Status::BadRequest;
          return ParseStatus::Error;
        }
      }
      req.headers.push_back({name, value});
      // Fast dispatch on known headers: check length first (cheap), then
      // full case-insensitive compare only on length match. Avoids 3× ieq
      // per header (old code did 3 full scans for Host, User-Agent, etc.).
      if (name.size() == 14) {
        if (ieq(name, "content-length")) {
          if (has_length) {
            error = "duplicate content-length";
            http_status = Status::BadRequest;
            return ParseStatus::Error;
          }
          unsigned long long v = 0;
          auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), v);
          if (ec != std::errc{} || ptr != value.data() + value.size()) {
            error = "bad content-length";
            http_status = Status::BadRequest;
            return ParseStatus::Error;
          }
          content_length = static_cast<std::size_t>(v);
          has_length = true;
        }
      } else if (name.size() == 17) {
        if (ieq(name, "transfer-encoding")) {
          if (ieq(value, "chunked")) chunked = true;
        }
      } else if (name.size() == 10) {
        if (ieq(name, "connection")) {
          connection_val = value;
        }
      }
    }
    if (nl == std::string_view::npos) break;
    rest = rest.substr(nl + 2);
  }

  // keep-alive semantics.
  bool is11 = (version_sv == "HTTP/1.1");
  if (ieq(connection_val, "close")) req.keep_alive = false;
  else if (ieq(connection_val, "keep-alive")) req.keep_alive = true;
  else req.keep_alive = is11;

  std::size_t body_off = head_end + 4;
  if (chunked && has_length) {
    error = "both chunked and content-length";
    http_status = Status::BadRequest;
    return ParseStatus::Error;  // smuggling guard
  }
  if (chunked) {
    // Parse chunks; assemble logically by locating end, but body view must be
    // contiguous. MVP: support chunked only by de-chunking check; we return
    // NeedMore until terminator found, then point body at raw region and let
    // Connection handle it. Simpler correct approach: require full chunk
    // sequence in buffer, then validate; body = raw chunk bytes region is NOT
    // decoded. Instead we decode by scanning and ensure we can present decoded
    // body — but views can't hold decoded data.
    //
    // MVP decision: decode in place is unsafe on const buffer. So: if chunked,
    // we scan for "0\r\n\r\n" terminator; if found and total <= max_body, we
    // report Complete with body = {} and let Connection re-assemble? To keep
    // MVP correct without extra copy, we REJECT chunked with 501 and document
    // streaming as roadmap. This avoids smuggling bugs.
    (void)has_crlf;
    error = "chunked not supported in MVP (use Content-Length)";
    http_status = Status::NotImplemented;
    return ParseStatus::Error;
  }

  if (has_length) {
    if (content_length > limits_.max_body_size) {
      error = "body too large";
      http_status = Status::PayloadTooLarge;
      return ParseStatus::TooLarge;
    }
    if (buf.size() < body_off + content_length) {
      return ParseStatus::NeedMore;
    }
    req.body = std::string_view(data + body_off, content_length);
    consumed = body_off + content_length;
    return ParseStatus::Complete;
  }

  // No body.
  req.body = {};
  consumed = body_off;
  return ParseStatus::Complete;
}

ParseResult StrictHttpParser::parse(const char* data, std::size_t len) const noexcept {
  ParseResult r;
  std::size_t consumed = 0;
  std::string error;
  Status hs = Status::BadRequest;
  Request tmp;
  ParseStatus st = parse_into(data, len, tmp, consumed, error, hs);
  r.status = st;
  r.consumed = consumed;
  r.error = std::move(error);
  r.http_status = hs;
  if (st == ParseStatus::Complete) r.request = std::move(tmp);
  return r;
}

void encode_response_into(std::string& out, const Response& res, bool keep_alive,
                            std::string_view request_version) {
  int code = status_code(res.status);
  std::string_view reason = status_reason(res.status);
  std::string_view ver =
      request_version.empty() ? std::string_view("HTTP/1.1") : request_version;
  out.append(ver.data(), ver.size());
  out += ' ';
  // to_chars: no temp std::string allocation (vs to_string).
  char numbuf[32];
  auto [p1, ec1] = std::to_chars(numbuf, numbuf + sizeof(numbuf), code);
  out.append(numbuf, p1);
  out += ' ';
  out.append(reason.data(), reason.size());
  out += "\r\nContent-Type: ";
  out += res.content_type;
  out += "\r\nContent-Length: ";
  auto [p2, ec2] = std::to_chars(numbuf, numbuf + sizeof(numbuf), res.body.size());
  (void)ec1;
  (void)ec2;
  out.append(numbuf, p2);
  out += "\r\nConnection: ";
  out += (keep_alive ? "keep-alive" : "close");
  out += "\r\nServer: elaina-cpp/0.1\r\n";
  for (auto& [k, v] : res.headers) {
    out += k;
    out += ": ";
    out += v;
    out += "\r\n";
  }
  out += "\r\n";
  out += res.body;
}

std::string encode_response(const Response& res, bool keep_alive,
                            std::string_view request_version) {
  std::string out;
  out.reserve(res.body.size() + 256);
  encode_response_into(out, res, keep_alive, request_version);
  return out;
}

}  // namespace elaina

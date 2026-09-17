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

ParseResult StrictHttpParser::parse(const char* data, std::size_t len) const noexcept {
  ParseResult r;
  std::string_view buf(data, len);

  // Find end of headers: \r\n\r\n
  std::size_t head_end = buf.find("\r\n\r\n");
  bool has_crlf = true;
  if (head_end == std::string_view::npos) {
    // Tolerate bare \n\n? No — strict: require CRLF (security).
    if (buf.size() > limits_.max_header_size + 1024) {
      r.status = ParseStatus::TooLarge;
      r.http_status = Status::HeaderTooLarge;
      r.error = "header too large";
      return r;
    }
    r.status = ParseStatus::NeedMore;
    return r;
  }
  if (head_end > limits_.max_header_size) {
    r.status = ParseStatus::TooLarge;
    r.http_status = Status::HeaderTooLarge;
    r.error = "header too large";
    return r;
  }

  std::string_view head = buf.substr(0, head_end);
  // Request line = first line.
  std::size_t eol = head.find("\r\n");
  std::string_view req_line = (eol == std::string_view::npos) ? head : head.substr(0, eol);
  if (req_line.size() > limits_.max_uri_size + 64) {
    r.status = ParseStatus::TooLarge;
    r.http_status = Status::HeaderTooLarge;
    r.error = "request line too large";
    return r;
  }

  // METHOD SP TARGET SP VERSION
  std::size_t s1 = req_line.find(' ');
  if (s1 == std::string_view::npos) {
    r.status = ParseStatus::Error;
    r.error = "bad request line";
    return r;
  }
  std::size_t s2 = req_line.find(' ', s1 + 1);
  if (s2 == std::string_view::npos) {
    r.status = ParseStatus::Error;
    r.error = "bad request line";
    return r;
  }
  std::string_view method_sv = req_line.substr(0, s1);
  std::string_view target_sv = req_line.substr(s1 + 1, s2 - s1 - 1);
  std::string_view version_sv = trim(req_line.substr(s2 + 1));

  Method method;
  if (!parse_method(method_sv, method)) {
    r.status = ParseStatus::Error;
    r.error = "unsupported method";
    return r;
  }
  if (target_sv.empty() || target_sv[0] != '/') {
    r.status = ParseStatus::Error;
    r.error = "bad target";
    return r;
  }
  if (target_sv.size() > limits_.max_uri_size) {
    r.status = ParseStatus::TooLarge;
    r.http_status = Status::HeaderTooLarge;
    r.error = "uri too long";
    return r;
  }
  if (version_sv != "HTTP/1.1" && version_sv != "HTTP/1.0") {
    r.status = ParseStatus::Error;
    r.error = "unsupported version";
    return r;
  }

  Request req;
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
        r.status = ParseStatus::TooLarge;
        r.http_status = Status::HeaderTooLarge;
        r.error = "too many headers";
        return r;
      }
      std::size_t colon = line.find(':');
      if (colon == std::string_view::npos) {
        r.status = ParseStatus::Error;
        r.error = "bad header line";
        return r;
      }
      std::string_view name = trim(line.substr(0, colon));
      std::string_view value = trim(line.substr(colon + 1));
      if (name.empty() || name.size() > 256 || value.size() > 8192) {
        r.status = ParseStatus::Error;
        r.error = "bad header field";
        return r;
      }
      // Reject header injection / obs-fold.
      for (char c : name) {
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t' || c == ':') {
          r.status = ParseStatus::Error;
          r.error = "bad header name";
          return r;
        }
      }
      for (char c : value) {
        if (c == '\r' || c == '\n') {
          r.status = ParseStatus::Error;
          r.error = "bad header value";
          return r;
        }
      }
      req.headers.push_back({name, value});
      if (ieq(name, "content-length")) {
        if (has_length) {
          r.status = ParseStatus::Error;
          r.error = "duplicate content-length";
          return r;
        }
        unsigned long long v = 0;
        auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), v);
        if (ec != std::errc{} || ptr != value.data() + value.size()) {
          r.status = ParseStatus::Error;
          r.error = "bad content-length";
          return r;
        }
        content_length = static_cast<std::size_t>(v);
        has_length = true;
      } else if (ieq(name, "transfer-encoding")) {
        if (ieq(value, "chunked")) chunked = true;
      } else if (ieq(name, "connection")) {
        connection_val = value;
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
    r.status = ParseStatus::Error;  // smuggling guard
    r.error = "both chunked and content-length";
    return r;
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
    r.status = ParseStatus::Error;
    r.http_status = Status::NotImplemented;
    r.error = "chunked not supported in MVP (use Content-Length)";
    return r;
  }

  if (has_length) {
    if (content_length > limits_.max_body_size) {
      r.status = ParseStatus::TooLarge;
      r.http_status = Status::PayloadTooLarge;
      r.error = "body too large";
      return r;
    }
    if (buf.size() < body_off + content_length) {
      r.status = ParseStatus::NeedMore;
      return r;
    }
    req.body = std::string_view(data + body_off, content_length);
    r.request = std::move(req);
    r.consumed = body_off + content_length;
    r.status = ParseStatus::Complete;
    return r;
  }

  // No body.
  req.body = {};
  r.request = std::move(req);
  r.consumed = body_off;
  r.status = ParseStatus::Complete;
  return r;
}

std::string encode_response(const Response& res, bool keep_alive,
                            std::string_view request_version) {
  std::string out;
  out.reserve(res.body.size() + 256);
  int code = status_code(res.status);
  std::string_view reason = status_reason(res.status);
  out.append(request_version.empty() ? "HTTP/1.1" : std::string(request_version));
  out += ' ';
  out += std::to_string(code);
  out += ' ';
  out.append(reason.data(), reason.size());
  out += "\r\n";
  out += "Content-Type: ";
  out += res.content_type;
  out += "\r\nContent-Length: ";
  out += std::to_string(res.body.size());
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
  return out;
}

}  // namespace elaina

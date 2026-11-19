#include "kestrel/http.hpp"

#include <cstdio>
#include <cstring>

namespace kestrel {
namespace {

constexpr std::size_t kMaxHttpMessage = 64u * 1024u;

[[nodiscard]] bool is_ws(unsigned char c) noexcept {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

[[nodiscard]] unsigned char lower_ascii(unsigned char c) noexcept {
  if (c >= 'A' && c <= 'Z') {
    return static_cast<unsigned char>(c - 'A' + 'a');
  }
  return c;
}

[[nodiscard]] bool header_name_eq(const char* begin, const char* end, const char* literal) noexcept {
  const std::size_t n = std::strlen(literal);
  if (static_cast<std::size_t>(end - begin) != n) {
    return false;
  }
  for (std::size_t i = 0; i < n; ++i) {
    if (lower_ascii(static_cast<unsigned char>(begin[i])) != static_cast<unsigned char>(literal[i])) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool contains_token(const char* begin, const char* end, const char* token) noexcept {
  const std::size_t n = std::strlen(token);
  const char* p = begin;
  while (p < end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == ',')) {
      ++p;
    }
    const char* start = p;
    while (p < end && *p != ',') {
      ++p;
    }
    const char* stop = p;
    while (stop > start && (stop[-1] == ' ' || stop[-1] == '\t')) {
      --stop;
    }
    if (static_cast<std::size_t>(stop - start) == n) {
      bool match = true;
      for (std::size_t i = 0; i < n; ++i) {
        if (lower_ascii(static_cast<unsigned char>(start[i])) != static_cast<unsigned char>(token[i])) {
          match = false;
          break;
        }
      }
      if (match) {
        return true;
      }
    }
  }
  return false;
}

struct Cursor {
  const char* p = nullptr;
  const char* end = nullptr;
  ChatFields* fields = nullptr;
  char* text = nullptr;
  std::size_t cap = 0;
  std::size_t len = 0;
  bool overflow = false;
  bool glue = false;
  int depth = 0;
};

[[nodiscard]] bool skip_ws(Cursor& c) noexcept {
  while (c.p < c.end && is_ws(static_cast<unsigned char>(*c.p))) {
    ++c.p;
  }
  return c.p < c.end;
}

// Consumes one JSON string. Stores at most `dest_cap` unescaped bytes.
// A string longer than the destination sets `truncated` and is still consumed.
[[nodiscard]] bool parse_string(Cursor& c, char* dest, std::size_t dest_cap, std::size_t* written,
                                bool* truncated) noexcept {
  if (c.p >= c.end || *c.p != '"') {
    return false;
  }
  ++c.p;
  std::size_t n = 0;
  while (c.p < c.end) {
    const unsigned char ch = static_cast<unsigned char>(*c.p++);
    unsigned char out = ch;
    int extra = 0;
    char utf[3]{};
    if (ch == '"') {
      if (written != nullptr) {
        *written = n;
      }
      return true;
    }
    if (ch == '\\') {
      if (c.p >= c.end) {
        return false;
      }
      const unsigned char esc = static_cast<unsigned char>(*c.p++);
      if (esc == 'u') {
        if (c.p + 4 > c.end) {
          return false;
        }
        unsigned code = 0;
        for (int i = 0; i < 4; ++i) {
          const unsigned char hex = static_cast<unsigned char>(*c.p++);
          code <<= 4u;
          if (hex >= '0' && hex <= '9') {
            code |= static_cast<unsigned>(hex - '0');
          } else if (hex >= 'a' && hex <= 'f') {
            code |= static_cast<unsigned>(hex - 'a' + 10);
          } else if (hex >= 'A' && hex <= 'F') {
            code |= static_cast<unsigned>(hex - 'A' + 10);
          } else {
            return false;
          }
        }
        if (code < 0x80u) {
          utf[0] = static_cast<char>(code);
          extra = 1;
        } else if (code < 0x800u) {
          utf[0] = static_cast<char>(0xC0u | (code >> 6u));
          utf[1] = static_cast<char>(0x80u | (code & 0x3Fu));
          extra = 2;
        } else {
          utf[0] = static_cast<char>(0xE0u | (code >> 12u));
          utf[1] = static_cast<char>(0x80u | ((code >> 6u) & 0x3Fu));
          utf[2] = static_cast<char>(0x80u | (code & 0x3Fu));
          extra = 3;
        }
      } else {
        switch (esc) {
          case '"':
          case '\\':
          case '/':
            out = esc;
            break;
          case 'b':
            out = '\b';
            break;
          case 'f':
            out = '\f';
            break;
          case 'n':
            out = '\n';
            break;
          case 'r':
            out = '\r';
            break;
          case 't':
            out = '\t';
            break;
          default:
            return false;
        }
        extra = 0;
      }
    } else if (ch < 0x20u) {
      return false;
    }

    const int count = extra > 0 ? extra : 1;
    char one = static_cast<char>(out);
    const char* src = extra > 0 ? utf : &one;
    for (int i = 0; i < count; ++i) {
      if (dest != nullptr && n < dest_cap) {
        dest[n] = src[i];
        ++n;
      } else if (dest != nullptr) {
        if (truncated != nullptr) {
          *truncated = true;
        }
      }
    }
  }
  return false;
}

[[nodiscard]] bool parse_value(Cursor& c, const char* key, std::size_t key_len) noexcept;

[[nodiscard]] bool skip_literal(Cursor& c, const char* literal) noexcept {
  const std::size_t n = std::strlen(literal);
  if (static_cast<std::size_t>(c.end - c.p) < n || std::memcmp(c.p, literal, n) != 0) {
    return false;
  }
  c.p += n;
  return true;
}

[[nodiscard]] bool skip_number(Cursor& c) noexcept {
  if (c.p < c.end && *c.p == '-') {
    ++c.p;
  }
  if (c.p >= c.end || *c.p < '0' || *c.p > '9') {
    return false;
  }
  while (c.p < c.end && *c.p >= '0' && *c.p <= '9') {
    ++c.p;
  }
  if (c.p < c.end && *c.p == '.') {
    ++c.p;
    if (c.p >= c.end || *c.p < '0' || *c.p > '9') {
      return false;
    }
    while (c.p < c.end && *c.p >= '0' && *c.p <= '9') {
      ++c.p;
    }
  }
  if (c.p < c.end && (*c.p == 'e' || *c.p == 'E')) {
    ++c.p;
    if (c.p < c.end && (*c.p == '+' || *c.p == '-')) {
      ++c.p;
    }
    if (c.p >= c.end || *c.p < '0' || *c.p > '9') {
      return false;
    }
    while (c.p < c.end && *c.p >= '0' && *c.p <= '9') {
      ++c.p;
    }
  }
  return true;
}

[[nodiscard]] bool append_content(Cursor& c) noexcept {
  if (c.text == nullptr) {
    return parse_string(c, nullptr, 0, nullptr, nullptr);
  }
  if (c.len > 0 && !c.glue) {
    if (c.len >= c.cap) {
      c.overflow = true;
    } else {
      c.text[c.len++] = '\n';
    }
  }
  bool truncated = false;
  std::size_t wrote = 0;
  const std::size_t room = c.len < c.cap ? c.cap - c.len : 0;
  if (!parse_string(c, c.text + c.len, room, &wrote, &truncated)) {
    return false;
  }
  c.len += wrote;
  if (truncated) {
    c.overflow = true;
  }
  return true;
}

[[nodiscard]] bool key_is(const char* key, std::size_t key_len, const char* literal) noexcept {
  const std::size_t n = std::strlen(literal);
  return key != nullptr && key_len == n && std::memcmp(key, literal, n) == 0;
}

bool parse_value(Cursor& c, const char* key, std::size_t key_len) noexcept {
  if (!skip_ws(c) || c.depth > 32) {
    return false;
  }
  const char ch = *c.p;
  if (ch == '"') {
    if (key_is(key, key_len, "content")) {
      return append_content(c);
    }
    if (key_is(key, key_len, "model") && c.fields != nullptr) {
      bool truncated = false;
      std::size_t wrote = 0;
      if (!parse_string(c, c.fields->model, sizeof(c.fields->model) - 1u, &wrote, &truncated)) {
        return false;
      }
      c.fields->model[wrote] = '\0';
      c.fields->model_len = wrote;
      (void)truncated;
      return true;
    }
    return parse_string(c, nullptr, 0, nullptr, nullptr);
  }
  if (ch == '{') {
    ++c.p;
    ++c.depth;
    if (!skip_ws(c)) {
      return false;
    }
    if (c.p < c.end && *c.p == '}') {
      ++c.p;
      --c.depth;
      return true;
    }
    while (true) {
      char kbuf[64];
      bool truncated = false;
      std::size_t klen = 0;
      if (!skip_ws(c) || !parse_string(c, kbuf, sizeof(kbuf), &klen, &truncated)) {
        return false;
      }
      if (!skip_ws(c) || *c.p != ':') {
        return false;
      }
      ++c.p;
      const char* kptr = truncated ? nullptr : kbuf;
      const std::size_t kuse = truncated ? 0 : klen;
      if (!parse_value(c, kptr, kuse)) {
        return false;
      }
      if (!skip_ws(c)) {
        return false;
      }
      if (*c.p == ',') {
        ++c.p;
        continue;
      }
      if (*c.p == '}') {
        ++c.p;
        --c.depth;
        return true;
      }
      return false;
    }
  }
  if (ch == '[') {
    ++c.p;
    ++c.depth;
    if (!skip_ws(c)) {
      return false;
    }
    if (c.p < c.end && *c.p == ']') {
      ++c.p;
      --c.depth;
      return true;
    }
    while (true) {
      if (!parse_value(c, nullptr, 0)) {
        return false;
      }
      if (!skip_ws(c)) {
        return false;
      }
      if (*c.p == ',') {
        ++c.p;
        continue;
      }
      if (*c.p == ']') {
        ++c.p;
        --c.depth;
        return true;
      }
      return false;
    }
  }
  if (ch == 't') {
    if (key_is(key, key_len, "stream") && c.fields != nullptr) {
      c.fields->stream = true;
    }
    return skip_literal(c, "true");
  }
  if (ch == 'f') {
    if (key_is(key, key_len, "stream") && c.fields != nullptr) {
      c.fields->stream = false;
    }
    return skip_literal(c, "false");
  }
  if (ch == 'n') {
    return skip_literal(c, "null");
  }
  if (ch == '-' || (ch >= '0' && ch <= '9')) {
    return skip_number(c);
  }
  return false;
}

[[nodiscard]] ExtractStatus walk_json(std::string_view json, ChatFields* fields, char* text,
                                      std::size_t text_cap, std::size_t& text_len, bool append,
                                      bool glue) noexcept {
  if (!append) {
    text_len = 0;
  }
  if (fields != nullptr && !append) {
    fields->stream = false;
    fields->model[0] = '\0';
    fields->model_len = 0;
  }
  Cursor cursor;
  cursor.p = json.data();
  cursor.end = json.data() + json.size();
  cursor.fields = fields;
  cursor.text = text;
  cursor.cap = text_cap;
  cursor.len = text_len;
  cursor.glue = glue;
  if (json.data() == nullptr && !json.empty()) {
    return ExtractStatus::Bad;
  }
  if (!skip_ws(cursor) || !parse_value(cursor, nullptr, 0)) {
    text_len = cursor.len;
    return cursor.overflow ? ExtractStatus::Overflow : ExtractStatus::Bad;
  }
  (void)skip_ws(cursor);
  text_len = cursor.len;
  if (cursor.p != cursor.end) {
    return ExtractStatus::Bad;
  }
  return cursor.overflow ? ExtractStatus::Overflow : ExtractStatus::Ok;
}

struct Writer {
  std::byte* dest = nullptr;
  std::size_t cap = 0;
  std::size_t used = 0;
  bool overflow = false;

  void add(const char* data, std::size_t n) noexcept {
    if (overflow || data == nullptr) {
      return;
    }
    if (n > cap - used) {
      overflow = true;
      return;
    }
    if (n > 0) {
      std::memcpy(dest + used, data, n);
      used += n;
    }
  }

  void add_sv(std::string_view text) noexcept { add(text.data(), text.size()); }

  void add_char(char ch) noexcept { add(&ch, 1); }
};

void append_json_string(Writer& w, std::string_view text) noexcept {
  for (unsigned char ch : text) {
    if (ch == '"' || ch == '\\') {
      w.add_char('\\');
      w.add_char(static_cast<char>(ch));
    } else if (ch == '\n') {
      w.add("\\n", 2);
    } else if (ch == '\r') {
      w.add("\\r", 2);
    } else if (ch == '\t') {
      w.add("\\t", 2);
    } else if (ch < 0x20u) {
      char hex[6] = {'\\', 'u', '0', '0', '0', '0'};
      constexpr char kDigits[] = "0123456789abcdef";
      hex[4] = kDigits[(ch >> 4u) & 0x0fu];
      hex[5] = kDigits[ch & 0x0fu];
      w.add(hex, 6);
    } else {
      w.add_char(static_cast<char>(ch));
    }
  }
}

[[nodiscard]] std::size_t escaped_len(std::string_view text) noexcept {
  std::size_t n = 0;
  for (unsigned char ch : text) {
    if (ch == '"' || ch == '\\' || ch == '\n' || ch == '\r' || ch == '\t') {
      n += 2;
    } else if (ch < 0x20u) {
      n += 6;
    } else {
      ++n;
    }
  }
  return n;
}

[[nodiscard]] std::size_t finish(Writer& w) noexcept {
  if (w.overflow || w.dest == nullptr) {
    return 0;
  }
  return w.used;
}

[[nodiscard]] const char* reason_phrase(int status) noexcept {
  switch (status) {
    case 400:
      return "Bad Request";
    case 401:
      return "Unauthorized";
    case 404:
      return "Not Found";
    case 413:
      return "Payload Too Large";
    case 502:
      return "Bad Gateway";
    default:
      return "Error";
  }
}

void append_u32(Writer& w, unsigned value) noexcept {
  char tmp[16];
  int n = std::snprintf(tmp, sizeof(tmp), "%u", value);
  if (n <= 0) {
    w.overflow = true;
    return;
  }
  w.add(tmp, static_cast<std::size_t>(n));
}

[[nodiscard]] const std::byte* header_end(const std::byte* begin, const std::byte* end) noexcept {
  for (const std::byte* p = begin; p + 4 <= end; ++p) {
    if (p[0] == std::byte{'\r'} && p[1] == std::byte{'\n'} && p[2] == std::byte{'\r'} &&
        p[3] == std::byte{'\n'}) {
      return p;
    }
  }
  return nullptr;
}

[[nodiscard]] bool parse_content_length(const char* begin, const char* end, std::uint64_t& value) noexcept {
  if (begin == end) {
    return false;
  }
  std::uint64_t acc = 0;
  for (const char* p = begin; p < end; ++p) {
    if (*p < '0' || *p > '9') {
      return false;
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(*p - '0');
    if (acc > (UINT64_C(0xffffffffffffffff) - digit) / 10u) {
      return false;
    }
    acc = acc * 10u + digit;
  }
  value = acc;
  return true;
}

}  // namespace

ParseStatus parse_http(std::span<const std::byte> buf, HttpMessage& out) noexcept {
  out = HttpMessage{};
  if (buf.empty() || buf.data() == nullptr) {
    return ParseStatus::NeedMore;
  }
  if (buf.size() > kMaxHttpMessage) {
    return ParseStatus::TooLarge;
  }
  const std::byte* header = header_end(buf.data(), buf.data() + buf.size());
  if (header == nullptr) {
    return buf.size() == kMaxHttpMessage ? ParseStatus::TooLarge : ParseStatus::NeedMore;
  }
  const char* begin = reinterpret_cast<const char*>(buf.data());
  // `header` points at the first byte of the terminating CRLF CRLF.
  // Include those four bytes so the last header line has its CRLF.
  const char* hend = reinterpret_cast<const char*>(header) + 4;
  const char* line_end = nullptr;
  for (const char* p = begin; p + 1 < hend; ++p) {
    if (p[0] == '\r' && p[1] == '\n') {
      line_end = p;
      break;
    }
  }
  if (line_end == nullptr) {
    return ParseStatus::Bad;
  }

  std::string_view method;
  std::string_view target;
  bool http11 = false;
  if (static_cast<std::size_t>(line_end - begin) >= 5 && std::memcmp(begin, "HTTP/", 5) == 0) {
    return ParseStatus::Bad;
  }
  const char* sp1 = nullptr;
    const char* sp2 = nullptr;
    for (const char* p = begin; p < line_end; ++p) {
      if (*p == ' ') {
        if (sp1 == nullptr) {
          sp1 = p;
        } else if (sp2 == nullptr) {
          sp2 = p;
          break;
        }
      }
    }
    if (sp1 == nullptr || sp2 == nullptr || sp1 == begin || sp2 == sp1 + 1) {
      return ParseStatus::Bad;
    }
    method = std::string_view(begin, static_cast<std::size_t>(sp1 - begin));
    target = std::string_view(sp1 + 1, static_cast<std::size_t>(sp2 - sp1 - 1));
    const char* ver = sp2 + 1;
    if (static_cast<std::size_t>(line_end - ver) != 8 || std::memcmp(ver, "HTTP/1.", 7) != 0 ||
        (ver[7] != '0' && ver[7] != '1')) {
      return ParseStatus::Bad;
    }
    http11 = ver[7] == '1';

  bool saw_length = false;
  std::uint64_t content_length = 0;
  bool close_conn = false;
  bool keep_conn = false;
  bool chunked = false;
  std::string_view authorization{};
  const char* cursor = line_end + 2;
  while (cursor < hend) {
    const char* nl = nullptr;
    for (const char* p = cursor; p + 1 < hend; ++p) {
      if (p[0] == '\r' && p[1] == '\n') {
        nl = p;
        break;
      }
    }
    if (nl == nullptr) {
      return ParseStatus::Bad;
    }
    if (nl == cursor) {
      break;
    }
    if (*cursor == ' ' || *cursor == '\t') {
      return ParseStatus::Bad;
    }
    const char* colon = nullptr;
    for (const char* p = cursor; p < nl; ++p) {
      if (*p == ':') {
        colon = p;
        break;
      }
    }
    if (colon == nullptr || colon == cursor) {
      return ParseStatus::Bad;
    }
    const char* value = colon + 1;
    while (value < nl && (*value == ' ' || *value == '\t')) {
      ++value;
    }
    const char* value_end = nl;
    while (value_end > value && (value_end[-1] == ' ' || value_end[-1] == '\t')) {
      --value_end;
    }
    if (header_name_eq(cursor, colon, "content-length")) {
      std::uint64_t parsed = 0;
      if (!parse_content_length(value, value_end, parsed)) {
        return ParseStatus::Bad;
      }
      if (saw_length && parsed != content_length) {
        return ParseStatus::Bad;
      }
      saw_length = true;
      content_length = parsed;
    } else if (header_name_eq(cursor, colon, "transfer-encoding")) {
      if (contains_token(value, value_end, "chunked")) {
        chunked = true;
      }
    } else if (header_name_eq(cursor, colon, "authorization")) {
      if (authorization.empty() && value_end >= value) {
        authorization = std::string_view(value, static_cast<std::size_t>(value_end - value));
      }
    } else if (header_name_eq(cursor, colon, "connection")) {
      if (contains_token(value, value_end, "close")) {
        close_conn = true;
      }
      if (contains_token(value, value_end, "keep-alive")) {
        keep_conn = true;
      }
    }
    cursor = nl + 2;
  }
  if (chunked) {
    return ParseStatus::Bad;
  }

  const bool body_method = method == "POST" || method == "PUT" || method == "PATCH";
  if (body_method && !saw_length) {
    return ParseStatus::Bad;
  }
  if (!saw_length) {
    content_length = 0;
  }
  if (content_length > kMaxHttpMessage) {
    return ParseStatus::TooLarge;
  }
  const std::size_t head_bytes = static_cast<std::size_t>(hend - begin);
  if (head_bytes > kMaxHttpMessage || content_length > kMaxHttpMessage - head_bytes) {
    return ParseStatus::TooLarge;
  }
  const std::size_t total = head_bytes + static_cast<std::size_t>(content_length);
  if (buf.size() < total) {
    return ParseStatus::NeedMore;
  }
  out.method = method;
  out.target = target;
  out.body = std::string_view(begin + head_bytes, static_cast<std::size_t>(content_length));
  out.authorization = authorization;
  out.total = total;
  if (close_conn) {
    out.keep_alive = false;
  } else if (keep_conn) {
    out.keep_alive = true;
  } else {
    out.keep_alive = http11;
  }
  return ParseStatus::Ok;
}

ExtractStatus extract_json_contents(std::string_view json, ChatFields* fields, char* text,
                                    std::size_t text_cap, std::size_t& text_len) noexcept {
  if (text == nullptr && text_cap != 0) {
    return ExtractStatus::Bad;
  }
  return walk_json(json, fields, text, text_cap, text_len, false, false);
}

ExtractStatus extract_assistant(std::string_view body, char* text, std::size_t text_cap,
                                std::size_t& text_len) noexcept {
  text_len = 0;
  if (text == nullptr || body.data() == nullptr) {
    return body.empty() ? ExtractStatus::Ok : ExtractStatus::Bad;
  }
  const char* p = body.data();
  const char* end = p + body.size();
  while (p < end && is_ws(static_cast<unsigned char>(*p))) {
    ++p;
  }
  if (p == end) {
    return ExtractStatus::Ok;
  }
  if (*p == '{' || *p == '[') {
    return walk_json(std::string_view(p, static_cast<std::size_t>(end - p)), nullptr, text, text_cap,
                     text_len, false, false);
  }

  const char* line = p;
  while (line < end) {
    const char* nl = line;
    while (nl < end && *nl != '\n') {
      ++nl;
    }
    const char* stop = nl;
    if (stop > line && stop[-1] == '\r') {
      --stop;
    }
    std::string_view row(line, static_cast<std::size_t>(stop - line));
    if (row.size() >= 5 && std::memcmp(row.data(), "data:", 5) == 0) {
      std::string_view data = row.substr(5);
      if (!data.empty() && data.front() == ' ') {
        data.remove_prefix(1);
      }
      if (!(data.size() == 6 && std::memcmp(data.data(), "[DONE]", 6) == 0) && !data.empty()) {
        const ExtractStatus status =
            walk_json(data, nullptr, text, text_cap, text_len, true, true);
        if (status != ExtractStatus::Ok) {
          return status;
        }
      }
    }
    if (nl == end) {
      break;
    }
    line = nl + 1;
  }
  return ExtractStatus::Ok;
}

std::size_t write_error(int http_status, std::string_view message, std::span<std::byte> dest) noexcept {
  Writer body;
  alignas(16) std::byte body_buf[1024];
  body.dest = body_buf;
  body.cap = sizeof(body_buf);
  body.add_sv("{\"error\":{\"message\":\"");
  append_json_string(body, message);
  body.add_sv("\",\"type\":\"invalid_request_error\"}}");
  const std::size_t body_len = finish(body);
  if (body_len == 0 && message.size() > 0) {
    return 0;
  }
  Writer out;
  out.dest = dest.data();
  out.cap = dest.size();
  out.add_sv("HTTP/1.1 ");
  append_u32(out, static_cast<unsigned>(http_status));
  out.add_char(' ');
  out.add_sv(reason_phrase(http_status));
  out.add_sv("\r\nContent-Type: application/json\r\nContent-Length: ");
  append_u32(out, static_cast<unsigned>(body_len));
  out.add_sv("\r\nConnection: close\r\n\r\n");
  out.add(reinterpret_cast<const char*>(body_buf), body_len);
  return finish(out);
}

std::size_t write_chat_json(std::string_view model, std::string_view content,
                            std::span<std::byte> dest) noexcept {
  if (model.empty()) {
    model = "kestrel";
  }
  const std::size_t body =
      std::strlen("{\"id\":\"chatcmpl-kestrel\",\"object\":\"chat.completion\",\"model\":\"") +
      escaped_len(model) +
      std::strlen("\",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"") +
      escaped_len(content) + std::strlen("\"},\"finish_reason\":\"stop\"}]}");
  Writer out;
  out.dest = dest.data();
  out.cap = dest.size();
  out.add_sv("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: ");
  append_u32(out, static_cast<unsigned>(body));
  out.add_sv("\r\nConnection: keep-alive\r\n\r\n");
  out.add_sv("{\"id\":\"chatcmpl-kestrel\",\"object\":\"chat.completion\",\"model\":\"");
  append_json_string(out, model);
  out.add_sv("\",\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\"");
  append_json_string(out, content);
  out.add_sv("\"},\"finish_reason\":\"stop\"}]}");
  return finish(out);
}

std::size_t write_chat_sse(std::string_view model, std::string_view content,
                           std::span<std::byte> dest) noexcept {
  if (model.empty()) {
    model = "kestrel";
  }
  const std::size_t first =
      std::strlen("data: {\"id\":\"chatcmpl-kestrel\",\"object\":\"chat.completion.chunk\",\"model\":\"") +
      escaped_len(model) +
      std::strlen("\",\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"content\":\"") +
      escaped_len(content) + std::strlen("\"}}]}\n\n");
  const std::size_t second =
      std::strlen("data: {\"id\":\"chatcmpl-kestrel\",\"object\":\"chat.completion.chunk\",\"model\":\"") +
      escaped_len(model) +
      std::strlen("\",\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n");
  const std::size_t done = std::strlen("data: [DONE]\n\n");
  const std::size_t body = first + second + done;

  Writer out;
  out.dest = dest.data();
  out.cap = dest.size();
  out.add_sv("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: ");
  append_u32(out, static_cast<unsigned>(body));
  out.add_sv("\r\nConnection: keep-alive\r\n\r\n");
  out.add_sv("data: {\"id\":\"chatcmpl-kestrel\",\"object\":\"chat.completion.chunk\",\"model\":\"");
  append_json_string(out, model);
  out.add_sv("\",\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"content\":\"");
  append_json_string(out, content);
  out.add_sv("\"}}]}\n\n");
  out.add_sv("data: {\"id\":\"chatcmpl-kestrel\",\"object\":\"chat.completion.chunk\",\"model\":\"");
  append_json_string(out, model);
  out.add_sv("\",\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n");
  out.add_sv("data: [DONE]\n\n");
  return finish(out);
}

std::size_t write_upstream_request(std::string_view model, std::string_view prompt,
                                  std::span<std::byte> dest) noexcept {
  if (model.empty()) {
    model = "kestrel";
  }
  const std::size_t json = std::strlen("{\"model\":\"") + escaped_len(model) +
                           std::strlen("\",\"messages\":[{\"role\":\"user\",\"content\":\"") + escaped_len(prompt) +
                           std::strlen("\"}]}");
  Writer out;
  out.dest = dest.data();
  out.cap = dest.size();
  out.add_sv("POST /v1/chat/completions HTTP/1.1\r\nHost: kestrel\r\nContent-Type: application/json\r\nContent-Length: ");
  append_u32(out, static_cast<unsigned>(json));
  out.add_sv("\r\nConnection: close\r\n\r\n{\"model\":\"");
  append_json_string(out, model);
  out.add_sv("\",\"messages\":[{\"role\":\"user\",\"content\":\"");
  append_json_string(out, prompt);
  out.add_sv("\"}]}");
  return finish(out);
}

}  // namespace kestrel

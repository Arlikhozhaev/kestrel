#include "kestrel/http.hpp"
#include "check.hpp"

#include <cstring>
#include <string>
#include <vector>

namespace {

void test_parse() {
  const std::string body = "{\"a\":true}";
  const std::string message = "POST /v1/chat/completions HTTP/1.1\r\nHost: h\r\nContent-Length: " +
                              std::to_string(body.size()) + "\r\n\r\n" + body + "TRAIL";
  std::vector<std::byte> buf(message.size());
  for (std::size_t i = 0; i < message.size(); ++i) {
    buf[i] = static_cast<std::byte>(message[i]);
  }
  kestrel::HttpMessage out;
  kestrel::ParseStatus status = kestrel::ParseStatus::NeedMore;
  for (std::size_t n = 1; n < message.size(); ++n) {
    status = kestrel::parse_http(std::span<const std::byte>(buf.data(), n), out);
    if (n < message.size() - 5) {
      CHECK(status == kestrel::ParseStatus::NeedMore || status == kestrel::ParseStatus::Ok);
    }
  }
  status = kestrel::parse_http(buf, out);
  CHECK(status == kestrel::ParseStatus::Ok);
  CHECK(out.method == "POST");
  CHECK(out.target == "/v1/chat/completions");
  CHECK(out.body == body);
  CHECK(out.keep_alive);
  CHECK(out.total + 5 == message.size());
}

void test_rejects() {
  const char* chunked = "POST /v1/chat/completions HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n";
  auto as_bytes = [](const char* text) {
    const std::size_t n = std::strlen(text);
    std::vector<std::byte> buf(n);
    for (std::size_t i = 0; i < n; ++i) {
      buf[i] = static_cast<std::byte>(text[i]);
    }
    return buf;
  };
  auto chunked_buf = as_bytes(chunked);
  kestrel::HttpMessage out;
  CHECK(kestrel::parse_http(chunked_buf, out) == kestrel::ParseStatus::Bad);
  auto missing = as_bytes("POST /v1/chat/completions HTTP/1.1\r\nHost: h\r\n\r\n");
  CHECK(kestrel::parse_http(missing, out) == kestrel::ParseStatus::Bad);
  auto get = as_bytes("GET /health HTTP/1.0\r\nHost: h\r\n\r\n");
  CHECK(kestrel::parse_http(get, out) == kestrel::ParseStatus::Ok);
  CHECK(!out.keep_alive);
  CHECK(out.body.empty());
  auto auth = as_bytes(
      "POST /v1/chat/completions HTTP/1.1\r\nAuthorization: Bearer s3cret\r\nContent-Length: 2\r\n\r\n{}");
  CHECK(kestrel::parse_http(auth, out) == kestrel::ParseStatus::Ok);
  CHECK(out.authorization == "Bearer s3cret");
}

void test_json_and_writers() {
  const char* json = "{\"model\":\"m\",\"stream\":true,\"messages\":[{\"role\":\"user\",\"content\":\"ping\"}]}";
  kestrel::ChatFields fields;
  char text[64];
  std::size_t len = 0;
  CHECK(kestrel::extract_json_contents(json, &fields, text, sizeof(text), len) == kestrel::ExtractStatus::Ok);
  CHECK(len == 4);
  CHECK(std::string(text, len) == "ping");
  CHECK(fields.stream);
  CHECK(std::string(fields.model, fields.model_len) == "m");

  const char* sse =
      "data: {\"choices\":[{\"delta\":{\"content\":\"pon\"}}]}\n"
      "data: {\"choices\":[{\"delta\":{\"content\":\"g\"}}]}\n"
      "data: [DONE]\n";
  CHECK(kestrel::extract_assistant(sse, text, sizeof(text), len) == kestrel::ExtractStatus::Ok);
  CHECK(std::string(text, len) == "pong");

  std::byte dest[2048];
  const std::size_t json_n = kestrel::write_chat_json("m", "pong", dest);
  CHECK(json_n > 0);
  const std::string json_out(reinterpret_cast<char*>(dest), json_n);
  CHECK(json_out.find("Content-Length:") != std::string::npos);
  CHECK(json_out.find("pong") != std::string::npos);
  const std::size_t sse_n = kestrel::write_chat_sse("m", "pong", dest);
  CHECK(sse_n > 0);
  const std::string sse_out(reinterpret_cast<char*>(dest), sse_n);
  CHECK(sse_out.find("data: [DONE]") != std::string::npos);
  CHECK(sse_out.find("text/event-stream") != std::string::npos);
  const std::size_t err_n = kestrel::write_error(400, "nope", dest);
  CHECK(err_n > 0);
  CHECK(std::string(reinterpret_cast<char*>(dest), err_n).find("Connection: close") != std::string::npos);
}

}  // namespace

int main() {
  test_parse();
  test_rejects();
  test_json_and_writers();
  return kestrel::test::failures.load() == 0 ? 0 : 1;
}

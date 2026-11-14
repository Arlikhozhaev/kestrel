#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace kestrel {

// TooLarge: the message cannot fit in a 64 KiB connection buffer.
enum class ParseStatus : std::uint8_t { NeedMore, Ok, Bad, TooLarge };

// One HTTP/1.0 or HTTP/1.1 message with a Content-Length body.
// Chunked transfer is rejected (Bad): the bytes are not framed without it,
// and the clients this gateway is built for send Content-Length.
// Views point into `buf`. `total` is the number of bytes the message occupies.
struct HttpMessage {
  std::string_view method{};
  std::string_view target{};
  std::string_view body{};
  std::size_t total = 0;
  bool keep_alive = true;
};

[[nodiscard]] ParseStatus parse_http(std::span<const std::byte> buf, HttpMessage& out) noexcept;

struct ChatFields {
  bool stream = false;
  char model[64]{};
  std::size_t model_len = 0;
};

enum class ExtractStatus : std::uint8_t { Ok, Bad, Overflow };

// Walks a JSON value. Copies every "content" string, unescaped, into `text`,
// separated by '\n' when more than one is present. Reads "model" and "stream"
// when `fields` is non-null. Does not allocate.
[[nodiscard]] ExtractStatus extract_json_contents(std::string_view json, ChatFields* fields, char* text,
                                                  std::size_t text_cap, std::size_t& text_len) noexcept;

// Assistant text from an upstream body: a JSON document, or SSE `data:` lines.
[[nodiscard]] ExtractStatus extract_assistant(std::string_view body, char* text, std::size_t text_cap,
                                              std::size_t& text_len) noexcept;

[[nodiscard]] std::size_t write_error(int http_status, std::string_view message, std::span<std::byte> dest) noexcept;

[[nodiscard]] std::size_t write_chat_json(std::string_view model, std::string_view content,
                                          std::span<std::byte> dest) noexcept;

[[nodiscard]] std::size_t write_chat_sse(std::string_view model, std::string_view content,
                                         std::span<std::byte> dest) noexcept;

}  // namespace kestrel

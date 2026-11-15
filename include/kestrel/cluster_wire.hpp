#pragma once

#include "kestrel/frame.hpp"
#include "kestrel/hash_ring.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace kestrel {

// Payloads carried by version-1 frames between processes.
// CacheEntry keeps the WAL body (dimension, lengths, vector, text, response).
// stream_id 0 is a replica push. A non-zero stream_id is the fetch rpc.
// Fetch payload: u16 model_len, u16 zero, u32 text_len, model, text.
// The zero keeps text_len aligned. Decode rejects a non-zero value.

struct WireIdentity {
  MemberEndpoint endpoint{};
  std::uint64_t incarnation = 0;
};

[[nodiscard]] inline std::uint64_t fnv1a64(std::span<const std::byte> bytes) noexcept {
  std::uint64_t hash = 14695981039346656037ull;
  for (std::byte b : bytes) {
    hash ^= static_cast<std::uint64_t>(b);
    hash *= 1099511628211ull;
  }
  return hash;
}

[[nodiscard]] inline bool encode_identity(WireIdentity id, std::span<std::byte> dest, std::size_t& out) noexcept {
  if (dest.size() < 16) {
    return false;
  }
  detail::store_u32(dest.data() + 0, id.endpoint.addr);
  detail::store_u16(dest.data() + 4, id.endpoint.port);
  detail::store_u16(dest.data() + 6, 0);
  detail::store_u32(dest.data() + 8, static_cast<std::uint32_t>(id.incarnation & 0xffffffffu));
  detail::store_u32(dest.data() + 12, static_cast<std::uint32_t>(id.incarnation >> 32));
  out = 16;
  return true;
}

[[nodiscard]] inline bool decode_identity(std::span<const std::byte> src, WireIdentity& id) noexcept {
  if (src.size() < 16) {
    return false;
  }
  id.endpoint.addr = detail::load_u32(src.data() + 0);
  id.endpoint.port = detail::load_u16(src.data() + 4);
  const std::uint32_t lo = detail::load_u32(src.data() + 8);
  const std::uint32_t hi = detail::load_u32(src.data() + 12);
  id.incarnation = static_cast<std::uint64_t>(lo) | (static_cast<std::uint64_t>(hi) << 32);
  return true;
}

[[nodiscard]] inline bool encode_member(WireIdentity id, std::uint16_t status, std::span<std::byte> dest,
                                       std::size_t& out) noexcept {
  if (!encode_identity(id, dest, out)) {
    return false;
  }
  detail::store_u16(dest.data() + 6, status);
  return true;
}

[[nodiscard]] inline bool decode_member(std::span<const std::byte> src, WireIdentity& id, std::uint16_t& status) noexcept {
  if (!decode_identity(src, id)) {
    return false;
  }
  status = detail::load_u16(src.data() + 6);
  return true;
}

// PingReq: requester identity, then the member to probe.
[[nodiscard]] inline bool encode_pingreq(WireIdentity from, WireIdentity target, std::span<std::byte> dest,
                                        std::size_t& out) noexcept {
  if (dest.size() < 32) {
    return false;
  }
  std::size_t n = 0;
  if (!encode_identity(from, dest, n) || !encode_identity(target, dest.subspan(16), n)) {
    return false;
  }
  out = 32;
  return true;
}

[[nodiscard]] inline bool decode_pingreq(std::span<const std::byte> src, WireIdentity& from, WireIdentity& target) noexcept {
  return src.size() >= 32 && decode_identity(src, from) && decode_identity(src.subspan(16), target);
}

struct FetchView {
  std::string_view model{};
  std::string_view text{};
};

[[nodiscard]] inline bool encode_fetch(FetchView view, std::span<std::byte> dest, std::size_t& out) noexcept {
  if (view.model.size() > 63 || view.text.size() > 0xffffu) {
    return false;
  }
  const std::size_t need = 8u + view.model.size() + view.text.size();
  if (dest.size() < need) {
    return false;
  }
  detail::store_u16(dest.data() + 0, static_cast<std::uint16_t>(view.model.size()));
  detail::store_u16(dest.data() + 2, 0);
  detail::store_u32(dest.data() + 4, static_cast<std::uint32_t>(view.text.size()));
  if (!view.model.empty()) {
    std::memcpy(dest.data() + 8, view.model.data(), view.model.size());
  }
  if (!view.text.empty()) {
    std::memcpy(dest.data() + 8 + view.model.size(), view.text.data(), view.text.size());
  }
  out = need;
  return true;
}

[[nodiscard]] inline bool decode_fetch(std::span<const std::byte> src, FetchView& view) noexcept {
  if (src.size() < 8) {
    return false;
  }
  const std::uint16_t model_len = detail::load_u16(src.data() + 0);
  const std::uint32_t text_len = detail::load_u32(src.data() + 4);
  const std::size_t need = 8u + static_cast<std::size_t>(model_len) + static_cast<std::size_t>(text_len);
  if (model_len > 63 || detail::load_u16(src.data() + 2) != 0 || src.size() < need) {
    return false;
  }
  view.model = std::string_view(reinterpret_cast<const char*>(src.data() + 8), model_len);
  view.text = std::string_view(reinterpret_cast<const char*>(src.data() + 8 + model_len), text_len);
  return true;
}

[[nodiscard]] inline bool encode_key(std::uint64_t key, std::span<std::byte> dest, std::size_t& out) noexcept {
  if (dest.size() < 8) {
    return false;
  }
  detail::store_u32(dest.data() + 0, static_cast<std::uint32_t>(key & 0xffffffffu));
  detail::store_u32(dest.data() + 4, static_cast<std::uint32_t>(key >> 32));
  out = 8;
  return true;
}

[[nodiscard]] inline bool decode_key(std::span<const std::byte> src, std::uint64_t& key) noexcept {
  if (src.size() < 8) {
    return false;
  }
  const std::uint32_t lo = detail::load_u32(src.data() + 0);
  const std::uint32_t hi = detail::load_u32(src.data() + 4);
  key = static_cast<std::uint64_t>(lo) | (static_cast<std::uint64_t>(hi) << 32);
  return true;
}

// WAL record body. The same bytes Wal::append writes after the frame header.
[[nodiscard]] inline bool encode_cache_payload(std::span<const float> vector, std::string_view text,
                                              std::string_view response, std::span<std::byte> dest,
                                              std::size_t& out) noexcept {
  if (vector.size() > 1024u || text.size() > 0xffffffffu || response.size() > 0xffffffffu) {
    return false;
  }
  const std::size_t need = 12u + vector.size() * sizeof(float) + text.size() + response.size();
  if (need > kMaxFramePayload || dest.size() < need) {
    return false;
  }
  detail::store_u32(dest.data() + 0, static_cast<std::uint32_t>(vector.size()));
  detail::store_u32(dest.data() + 4, static_cast<std::uint32_t>(text.size()));
  detail::store_u32(dest.data() + 8, static_cast<std::uint32_t>(response.size()));
  if (!vector.empty()) {
    std::memcpy(dest.data() + 12, vector.data(), vector.size() * sizeof(float));
  }
  if (!text.empty()) {
    std::memcpy(dest.data() + 12 + vector.size() * sizeof(float), text.data(), text.size());
  }
  if (!response.empty()) {
    std::memcpy(dest.data() + 12 + vector.size() * sizeof(float) + text.size(), response.data(),
                response.size());
  }
  out = need;
  return true;
}

struct CacheView {
  std::span<const float> vector{};
  std::string_view text{};
  std::string_view response{};
};

[[nodiscard]] inline bool decode_cache_payload(std::span<const std::byte> src, CacheView& view) noexcept {
  if (src.size() < 12) {
    return false;
  }
  const std::uint32_t dimension = detail::load_u32(src.data() + 0);
  const std::uint32_t text_len = detail::load_u32(src.data() + 4);
  const std::uint32_t response_len = detail::load_u32(src.data() + 8);
  if (dimension > 1024u) {
    return false;
  }
  const std::size_t need =
      12u + static_cast<std::size_t>(dimension) * sizeof(float) + text_len + response_len;
  if (src.size() < need) {
    return false;
  }
  const auto* floats = reinterpret_cast<const float*>(src.data() + 12);
  const char* text = reinterpret_cast<const char*>(src.data() + 12 + static_cast<std::size_t>(dimension) * sizeof(float));
  view.vector = std::span<const float>(floats, dimension);
  view.text = std::string_view(text, text_len);
  view.response = std::string_view(text + text_len, response_len);
  return true;
}

}  // namespace kestrel

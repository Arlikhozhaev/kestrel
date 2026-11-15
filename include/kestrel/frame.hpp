#pragma once

#include "kestrel/crc32c.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace kestrel {

// Inter-node frame, little-endian, version 1. 32-byte header.
// Phase 0 ships the codec and nothing that opens a socket.
//
//   offset  0  u32 magic          'KSTR' = 0x5254534B
//   offset  4  u16 version        1
//   offset  6  u16 flags          unknown bits are preserved, not rejected
//   offset  8  u32 header_bytes   32
//   offset 12  u32 payload_bytes
//   offset 16  u32 crc32c         header (this field zero) || payload
//   offset 20  u64 stream_id
//   offset 28  u16 type
//   offset 30  u16 reserved       must be 0 on encode
//
// Partial I/O is a return value, not a short read inside the codec.
//   NeedMore    buffer does not yet hold header + declared payload
//   BadMagic    32 bytes are present and magic is not KSTR
//   BadVersion  version is not 1
//   BadLength   header_bytes != 32, reserved != 0, or payload_bytes > 16 MiB
//               (a hostile length fails before we wait for it)
//   BadCrc      the bytes are present and the checksum does not match
// Unknown type values decode successfully. Dispatch is the caller's job.
// The payload span points into `buffer`. It dies with that buffer.

inline constexpr std::size_t kFrameHeaderBytes = 32;
inline constexpr std::uint32_t kFrameMagic = 0x5254534Bu;
inline constexpr std::uint16_t kFrameVersion = 1;
inline constexpr std::uint32_t kMaxFramePayload = 16u * 1024u * 1024u;

enum class FrameType : std::uint16_t {
  Hello = 1,
  CacheEntry = 2,
  CacheInvalidate = 3,
  Ping = 4,
  PingAck = 5,
  PingReq = 6,
  Member = 7,
  Fetch = 8,
  FetchMiss = 9,
  FetchFail = 10,
};

enum class DecodeStatus : std::uint8_t {
  Ok = 0,
  NeedMore = 1,
  BadMagic = 2,
  BadVersion = 3,
  BadLength = 4,
  BadCrc = 5,
};

struct FrameFields {
  std::uint16_t flags{0};
  std::uint64_t stream_id{0};
  FrameType type{FrameType::Ping};
};

struct DecodedFrame {
  std::uint16_t version{0};
  std::uint16_t flags{0};
  std::uint64_t stream_id{0};
  FrameType type{FrameType::Ping};
  std::span<const std::byte> payload{};
};

namespace detail {

inline void store_u16(std::byte* dest, std::uint16_t value) noexcept {
  dest[0] = static_cast<std::byte>(value & 0xFFu);
  dest[1] = static_cast<std::byte>((value >> 8) & 0xFFu);
}

inline void store_u32(std::byte* dest, std::uint32_t value) noexcept {
  dest[0] = static_cast<std::byte>(value & 0xFFu);
  dest[1] = static_cast<std::byte>((value >> 8) & 0xFFu);
  dest[2] = static_cast<std::byte>((value >> 16) & 0xFFu);
  dest[3] = static_cast<std::byte>((value >> 24) & 0xFFu);
}

inline void store_u64(std::byte* dest, std::uint64_t value) noexcept {
  for (int i = 0; i < 8; ++i) {
    dest[i] = static_cast<std::byte>((value >> (8 * i)) & 0xFFu);
  }
}

[[nodiscard]] inline std::uint16_t load_u16(const std::byte* src) noexcept {
  return static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(static_cast<std::uint8_t>(src[0])) |
      (static_cast<std::uint16_t>(static_cast<std::uint8_t>(src[1])) << 8));
}

[[nodiscard]] inline std::uint32_t load_u32(const std::byte* src) noexcept {
  return static_cast<std::uint32_t>(static_cast<std::uint8_t>(src[0])) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(src[1])) << 8) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(src[2])) << 16) |
         (static_cast<std::uint32_t>(static_cast<std::uint8_t>(src[3])) << 24);
}

[[nodiscard]] inline std::uint64_t load_u64(const std::byte* src) noexcept {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(src[i]))
             << (8 * i);
  }
  return value;
}

[[nodiscard]] inline bool ranges_overlap(const std::byte* a, std::size_t a_n,
                                         const std::byte* b, std::size_t b_n) noexcept {
  if (a == nullptr || b == nullptr || a_n == 0 || b_n == 0) {
    return false;
  }
  const auto a_begin = reinterpret_cast<std::uintptr_t>(a);
  const auto b_begin = reinterpret_cast<std::uintptr_t>(b);
  const auto a_end = a_begin + a_n;
  const auto b_end = b_begin + b_n;
  if (a_end < a_begin || b_end < b_begin) {
    return true;
  }
  return a_begin < b_end && b_begin < a_end;
}

}  // namespace detail

// Writes a complete frame into `dest`. Returns false, and does not claim
// success, when `dest` is shorter than the frame or when `payload` overlaps
// the header region. A payload that already sits at dest+32 is not copied.
[[nodiscard]] inline bool encode_frame(std::span<std::byte> dest, const FrameFields& fields,
                                       std::span<const std::byte> payload) noexcept {
  if (payload.size() > kMaxFramePayload) {
    return false;
  }
  const std::size_t total = kFrameHeaderBytes + payload.size();
  if (dest.size() < total || dest.data() == nullptr) {
    return false;
  }
  const bool inplace =
      payload.empty() || payload.data() == dest.data() + kFrameHeaderBytes;
  if (!inplace) {
    if (detail::ranges_overlap(payload.data(), payload.size(), dest.data(),
                               kFrameHeaderBytes)) {
      return false;
    }
    std::memmove(dest.data() + kFrameHeaderBytes, payload.data(), payload.size());
  }
  std::memset(dest.data(), 0, kFrameHeaderBytes);
  detail::store_u32(dest.data() + 0, kFrameMagic);
  detail::store_u16(dest.data() + 4, kFrameVersion);
  detail::store_u16(dest.data() + 6, fields.flags);
  detail::store_u32(dest.data() + 8, static_cast<std::uint32_t>(kFrameHeaderBytes));
  detail::store_u32(dest.data() + 12, static_cast<std::uint32_t>(payload.size()));
  detail::store_u64(dest.data() + 20, fields.stream_id);
  detail::store_u16(dest.data() + 28, static_cast<std::uint16_t>(fields.type));
  const std::uint32_t crc =
      crc32c(std::span<const std::byte>(dest.data(), total));
  detail::store_u32(dest.data() + 16, crc);
  return true;
}

[[nodiscard]] inline DecodeStatus decode_frame(std::span<const std::byte> buffer,
                                               DecodedFrame& out) noexcept {
  out = DecodedFrame{};
  if (buffer.size() < kFrameHeaderBytes || buffer.data() == nullptr) {
    return DecodeStatus::NeedMore;
  }
  const std::byte* header = buffer.data();
  const std::uint32_t magic = detail::load_u32(header + 0);
  if (magic != kFrameMagic) {
    return DecodeStatus::BadMagic;
  }
  const std::uint16_t version = detail::load_u16(header + 4);
  if (version != kFrameVersion) {
    return DecodeStatus::BadVersion;
  }
  const std::uint32_t header_bytes = detail::load_u32(header + 8);
  if (header_bytes != kFrameHeaderBytes) {
    return DecodeStatus::BadLength;
  }
  const std::uint32_t payload_bytes = detail::load_u32(header + 12);
  if (payload_bytes > kMaxFramePayload) {
    return DecodeStatus::BadLength;
  }
  const std::size_t total = kFrameHeaderBytes + static_cast<std::size_t>(payload_bytes);
  if (buffer.size() < total) {
    return DecodeStatus::NeedMore;
  }
  if (detail::load_u16(header + 30) != 0) {
    return DecodeStatus::BadLength;
  }

  Crc32c crc;
  crc.update(std::span<const std::byte>(header, 16));
  const std::byte zeros[4] = {};
  crc.update(std::span<const std::byte>(zeros, 4));
  crc.update(std::span<const std::byte>(header + 20, total - 20));
  const std::uint32_t actual = detail::load_u32(header + 16);
  if (crc.finish() != actual) {
    return DecodeStatus::BadCrc;
  }

  out.version = version;
  out.flags = detail::load_u16(header + 6);
  out.stream_id = detail::load_u64(header + 20);
  out.type = static_cast<FrameType>(detail::load_u16(header + 28));
  out.payload = std::span<const std::byte>(header + kFrameHeaderBytes, payload_bytes);
  return DecodeStatus::Ok;
}

}  // namespace kestrel

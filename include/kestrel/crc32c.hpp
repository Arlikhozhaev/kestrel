#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#if defined(__SSE4_2__)
#include <nmmintrin.h>
#endif

namespace kestrel {

// CRC-32C (Castagnoli), the polynomial the SSE4.2 `crc32` instruction
// implements. This host has SSE4.2 (i7-8650U). ISO-HDLC CRC-32 is a
// different polynomial and does not have an instruction here; the WAL
// and the inter-node frame both use CRC-32C so the checksum is a handful
// of u64 steps instead of a table walk.
//
// Init 0xFFFFFFFF, xorout 0xFFFFFFFF, reflected polynomial 0x82F63B78.
// Empty input checksums to 0. "123456789" checksums to 0xE3069283.
class Crc32c {
 public:
  void update(std::span<const std::byte> data) noexcept {
    const auto* cursor = data.data();
    std::size_t remaining = data.size();
#if defined(__SSE4_2__) && defined(__x86_64__)
    while (remaining >= 8) {
      std::uint64_t chunk = 0;
      std::memcpy(&chunk, cursor, sizeof(chunk));
      state_ = static_cast<std::uint32_t>(
          _mm_crc32_u64(static_cast<std::uint64_t>(state_), chunk));
      cursor += 8;
      remaining -= 8;
    }
    if (remaining >= 4) {
      std::uint32_t chunk = 0;
      std::memcpy(&chunk, cursor, sizeof(chunk));
      state_ = _mm_crc32_u32(state_, chunk);
      cursor += 4;
      remaining -= 4;
    }
    if (remaining >= 2) {
      std::uint16_t chunk = 0;
      std::memcpy(&chunk, cursor, sizeof(chunk));
      state_ = _mm_crc32_u16(state_, chunk);
      cursor += 2;
      remaining -= 2;
    }
    if (remaining == 1) {
      state_ = _mm_crc32_u8(state_, static_cast<unsigned char>(*cursor));
    }
#else
    for (std::size_t i = 0; i < remaining; ++i) {
      std::uint32_t crc = state_ ^ static_cast<std::uint32_t>(static_cast<std::uint8_t>(cursor[i]));
      for (int bit = 0; bit < 8; ++bit) {
        const std::uint32_t mask = static_cast<std::uint32_t>(-(crc & 1u));
        crc = (crc >> 1) ^ (0x82F63B78u & mask);
      }
      state_ = crc;
    }
#endif
  }

  [[nodiscard]] std::uint32_t finish() const noexcept { return ~state_; }

 private:
  std::uint32_t state_{0xFFFFFFFFu};
};

[[nodiscard]] inline std::uint32_t crc32c(std::span<const std::byte> data) noexcept {
  Crc32c crc;
  crc.update(data);
  return crc.finish();
}

// Reference implementation. Production crc32c() uses SSE4.2 on this host.
// Tests require the two to agree, including on split update() calls.
[[nodiscard]] inline std::uint32_t crc32c_software(std::span<const std::byte> data) noexcept {
  std::uint32_t crc = 0xFFFFFFFFu;
  for (const std::byte byte : data) {
    crc ^= static_cast<std::uint32_t>(static_cast<std::uint8_t>(byte));
    for (int bit = 0; bit < 8; ++bit) {
      const std::uint32_t mask = static_cast<std::uint32_t>(-(crc & 1u));
      crc = (crc >> 1) ^ (0x82F63B78u & mask);
    }
  }
  return ~crc;
}

}  // namespace kestrel

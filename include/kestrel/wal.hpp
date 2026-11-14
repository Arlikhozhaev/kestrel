#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace kestrel {

// One append-only mmap log per core. Records are version-1 frames
// (FrameType::CacheEntry) so the checksum is the same CRC-32C as the wire.
//
// Payload, little-endian, immediately after the 32-byte frame header:
//   u32 dimension
//   u32 text_len
//   u32 response_len
//   f32 vector[dimension]
//   u8  text[text_len]
//   u8  response[response_len]
//
// A torn tail fails the CRC. recover() truncates the file to the last good
// record and replays what remains. append() msyncs that record before it
// returns. The cache hit path does not call append().
struct WalRecord {
  std::uint64_t payload_offset = 0;
  std::span<const float> vector{};
  std::string_view text{};
  std::string_view response{};
};

class Wal {
 public:
  explicit Wal(const char* path) noexcept;
  Wal(const Wal&) = delete;
  Wal& operator=(const Wal&) = delete;
  Wal(Wal&&) = delete;
  Wal& operator=(Wal&&) = delete;
  ~Wal();

  [[nodiscard]] bool ok() const noexcept { return map_ != nullptr; }
  [[nodiscard]] std::uint64_t end() const noexcept { return end_; }

  // Durable on success. Returns the payload offset, or UINT64_MAX.
  [[nodiscard]] std::uint64_t append(std::span<const float> vector, std::string_view text,
                                    std::string_view response) noexcept;

  // Views point into the mapping. They die at the next append() that grows
  // the file, and at destruction. Do not hold them across append().
  [[nodiscard]] bool read(std::uint64_t payload_offset, WalRecord& out) const noexcept;

  // Invokes fn(record) for each good record. Truncates a torn tail first.
  // fn returns false to stop. Returns how many records were visited.
  template <typename Fn>
  std::size_t recover(Fn&& fn) noexcept;

 private:
  [[nodiscard]] bool ensure(std::uint64_t need) noexcept;
  [[nodiscard]] bool sync_range(std::uint64_t begin, std::uint64_t end) noexcept;
  void truncate_to(std::uint64_t size) noexcept;

  int fd_ = -1;
  std::byte* map_ = nullptr;
  std::uint64_t mapped_ = 0;
  std::uint64_t end_ = 0;
  std::size_t page_ = 4096;
};

}  // namespace kestrel

#include "kestrel/frame.hpp"

#include <cstring>

namespace kestrel {

template <typename Fn>
std::size_t Wal::recover(Fn&& fn) noexcept {
  if (!ok()) {
    return 0;
  }
  std::uint64_t pos = 0;
  std::size_t count = 0;
  while (pos < end_) {
    const auto available = static_cast<std::size_t>(end_ - pos);
    DecodedFrame frame;
    const DecodeStatus status =
        decode_frame(std::span<const std::byte>(map_ + pos, available), frame);
    if (status == DecodeStatus::NeedMore || status == DecodeStatus::BadCrc ||
        status == DecodeStatus::BadMagic || status == DecodeStatus::BadLength ||
        status == DecodeStatus::BadVersion) {
      truncate_to(pos);
      break;
    }
    const std::uint64_t payload_offset = pos + kFrameHeaderBytes;
    const std::uint64_t next = payload_offset + frame.payload.size();
    if (frame.type == FrameType::CacheEntry) {
      WalRecord record;
      if (read(payload_offset, record)) {
        ++count;
        if (!fn(record)) {
          return count;
        }
      }
    }
    pos = next;
  }
  return count;
}

}  // namespace kestrel

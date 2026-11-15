#pragma once

#include "kestrel/spsc_ring.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace kestrel {

// One directed SPSC edge per ordered pair of cores. The ring carries a buffer
// index. The bytes live beside the ring, so a 16 KiB cache record does not
// enter the L2-bounded slot array. Two buffers: the producer does not reuse
// a buffer until the consumer has pushed its index back.
//
// publish() is called only by `from`. take() is called only by `to`.
inline constexpr std::uint32_t kMailTextCap = 16u * 1024u;
inline constexpr std::uint32_t kMailModelCap = 64;

enum class MailKind : std::uint32_t {
  Replica = 1,
  Resolve = 2,
  Hit = 3,
  Fill = 4,
  Fail = 5,
};

struct MailView {
  MailKind kind = MailKind::Replica;
  std::uint64_t key = 0;
  std::uint32_t conn = 0;
  std::span<const float> vector{};
  std::string_view text{};
  std::string_view response{};
  std::string_view model{};
};

class CoreMail {
 public:
  CoreMail(std::uint32_t cores, std::uint32_t dimension) noexcept;
  CoreMail(const CoreMail&) = delete;
  CoreMail& operator=(const CoreMail&) = delete;
  ~CoreMail();

  [[nodiscard]] bool ok() const noexcept { return cores_ < 2 || ready_; }
  [[nodiscard]] std::uint32_t cores() const noexcept { return cores_; }

  // Copies the view into a free buffer. False when both buffers are still
  // with the consumer: the caller keeps the record locally and does not block.
  [[nodiscard]] bool publish(std::uint32_t from, std::uint32_t to, const MailView& view) noexcept;

  // Copies one record into the caller buffers. Spans in `out` point at those
  // buffers. False when that edge is empty.
  [[nodiscard]] bool take(std::uint32_t to, std::uint32_t from, MailView& out, float* vector, char* text,
                          char* response, char* model) noexcept;

 private:
  struct Edge;

  Edge** edges_ = nullptr;
  std::uint32_t cores_ = 0;
  std::uint32_t dimension_ = 0;
  bool ready_ = false;

  [[nodiscard]] Edge* edge(std::uint32_t from, std::uint32_t to) noexcept;
};

}  // namespace kestrel

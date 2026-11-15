#pragma once

#include "kestrel/hash_ring.hpp"

#include <cstdint>

namespace kestrel {

// SWIM failure detector, as a pure state machine. The reactor owns the
// sockets and the clock. One probe is in flight at a time: a direct ping,
// then one indirect ping through a third member, then suspicion, then dead.
// A higher incarnation wins. This process refutes suspicion of itself by
// bumping its incarnation and emitting Alive.
inline constexpr std::uint32_t kSwimMaxMembers = kHashRingMaxMembers;

enum class SwimStatus : std::uint8_t { Alive = 1, Suspected = 2, Dead = 3 };

struct SwimMember {
  MemberEndpoint endpoint{};
  SwimStatus status = SwimStatus::Alive;
  std::uint64_t incarnation = 0;
};

enum class SwimActionKind : std::uint8_t { Ping = 1, PingReq = 2, PingAck = 3, Member = 4 };

// `to` is who the reactor should send the frame to.
// `about` is the member the payload describes.
struct SwimAction {
  SwimActionKind kind = SwimActionKind::Ping;
  MemberEndpoint to{};
  MemberEndpoint about{};
  std::uint64_t incarnation = 0;
  SwimStatus status = SwimStatus::Alive;
};

class Swim {
 public:
  Swim() noexcept = default;

  void configure(std::uint32_t period_ms, std::uint32_t timeout_ms, std::uint32_t suspect_ms) noexcept;

  // Installs this process as an Alive member. Later calls replace it.
  void start(MemberEndpoint self, std::uint64_t incarnation) noexcept;

  [[nodiscard]] MemberEndpoint self() const noexcept { return self_; }
  [[nodiscard]] std::uint64_t incarnation() const noexcept { return self_incarnation_; }
  [[nodiscard]] std::uint32_t count() const noexcept { return count_; }
  [[nodiscard]] const SwimMember& member(std::uint32_t index) const noexcept { return members_[index]; }

  // Alive members, including this process. Dead members stay in the table
  // so a later incarnation can refute, but they are not probed and not owners.
  [[nodiscard]] std::uint32_t alive(MemberEndpoint* out, std::uint32_t cap) const noexcept;

  // Records a member we were configured to dial, before any message arrives.
  // Returns false when the table is full.
  [[nodiscard]] bool seed(MemberEndpoint endpoint) noexcept;

  // Applies a membership announcement. May append a self-refutation to `out`.
  [[nodiscard]] std::uint32_t observe(MemberEndpoint endpoint, SwimStatus status, std::uint64_t incarnation,
                                     std::uint64_t now_ms, SwimAction* out, std::uint32_t max) noexcept;

  [[nodiscard]] std::uint32_t tick(std::uint64_t now_ms, SwimAction* out, std::uint32_t max) noexcept;

  [[nodiscard]] std::uint32_t on_ping(MemberEndpoint from, std::uint64_t incarnation, std::uint64_t now_ms,
                                     SwimAction* out, std::uint32_t max) noexcept;

  // `about` is the member the ack declares alive. A direct ack declares the
  // sender. An indirect ack declares the member a third process probed.
  [[nodiscard]] std::uint32_t on_ack(MemberEndpoint about, std::uint64_t incarnation, std::uint64_t now_ms,
                                    SwimAction* out, std::uint32_t max) noexcept;

  [[nodiscard]] std::uint32_t on_pingreq(MemberEndpoint from, MemberEndpoint target, std::uint64_t incarnation,
                                        std::uint64_t now_ms, SwimAction* out, std::uint32_t max) noexcept;

 private:
  [[nodiscard]] std::uint32_t index_of(MemberEndpoint endpoint) const noexcept;
  [[nodiscard]] std::uint32_t push(SwimAction* out, std::uint32_t n, std::uint32_t max, SwimAction action) const noexcept;
  void clear_probe() noexcept;
  [[nodiscard]] std::uint32_t suspect(std::uint32_t index, std::uint64_t now_ms, SwimAction* out,
                                     std::uint32_t n, std::uint32_t max) noexcept;

  SwimMember members_[kSwimMaxMembers]{};
  std::uint64_t suspect_since_[kSwimMaxMembers]{};
  std::uint32_t count_ = 0;
  std::uint32_t self_index_ = 0;
  MemberEndpoint self_{};
  std::uint64_t self_incarnation_ = 1;
  std::uint32_t cursor_ = 0;
  std::uint64_t next_period_ms_ = 0;
  std::uint64_t probe_deadline_ms_ = 0;
  std::uint32_t probe_index_ = kSwimMaxMembers;
  bool probe_indirect_ = false;
  std::uint32_t req_from_ = kSwimMaxMembers;
  MemberEndpoint req_target_{};
  std::uint32_t period_ms_ = 200;
  std::uint32_t timeout_ms_ = 200;
  std::uint32_t suspect_ms_ = 1000;
};

}  // namespace kestrel

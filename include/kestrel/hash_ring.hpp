#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace kestrel {

// Consistent hash of cache owners. The hit path does not consult this.
// A process that already holds the entry serves it from the local HNSW.
// The ring is consulted only on a miss, to choose who may call the model
// and who is asked for a copy.
//
// 128 virtual nodes per member. The ring is rebuilt when membership changes,
// which is rare. Lookup is a binary search plus a short walk. R is 2, or
// the member count when the cluster is smaller than that.
inline constexpr std::uint32_t kHashRingMaxMembers = 32;
inline constexpr std::uint32_t kVnodesPerMember = 128;
inline constexpr std::uint32_t kReplicaCount = 2;

struct MemberEndpoint {
  std::uint32_t addr = 0;  // IPv4, host order
  std::uint16_t port = 0;

  friend bool operator==(MemberEndpoint a, MemberEndpoint b) noexcept {
    return a.addr == b.addr && a.port == b.port;
  }
};

class HashRing {
 public:
  HashRing() noexcept = default;

  // Replaces the member set. Endpoints must be unique. Returns false when
  // the set is empty or larger than kHashRingMaxMembers.
  [[nodiscard]] bool set_members(std::span<const MemberEndpoint> members) noexcept;

  [[nodiscard]] std::uint32_t member_count() const noexcept { return members_; }

  // Writes up to kReplicaCount owners, clockwise from `key`, distinct members.
  // Returns how many were written. Zero when the ring is empty.
  [[nodiscard]] std::uint32_t owners(std::uint64_t key, MemberEndpoint* out) const noexcept;

  [[nodiscard]] static std::uint64_t key_of(std::string_view text) noexcept;

 private:
  struct Vnode {
    std::uint64_t hash = 0;
    std::uint32_t member = 0;
  };

  static void sort_vnodes(Vnode* item, std::uint32_t count) noexcept;

  MemberEndpoint member_[kHashRingMaxMembers]{};
  Vnode vnode_[kHashRingMaxMembers * kVnodesPerMember]{};
  std::uint32_t members_ = 0;
  std::uint32_t vnodes_ = 0;
};

}  // namespace kestrel

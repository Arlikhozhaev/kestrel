#include "kestrel/hash_ring.hpp"

#include "kestrel/cluster_wire.hpp"

#include <cstring>

namespace kestrel {

void HashRing::sort_vnodes(Vnode* item, std::uint32_t count) noexcept {
  auto before = [](const Vnode& a, const Vnode& b) noexcept {
    return a.hash < b.hash || (a.hash == b.hash && a.member < b.member);
  };
  auto swap = [](Vnode* a, Vnode* b) noexcept {
    const Vnode tmp = *a;
    *a = *b;
    *b = tmp;
  };
  auto sift_down = [&](Vnode* heap, std::uint32_t start, std::uint32_t n) noexcept {
    std::uint32_t root = start;
    for (;;) {
      const std::uint32_t left = root * 2u + 1u;
      if (left >= n) {
        return;
      }
      std::uint32_t child = root;
      if (before(heap[child], heap[left])) {
        child = left;
      }
      const std::uint32_t right = left + 1u;
      if (right < n && before(heap[child], heap[right])) {
        child = right;
      }
      if (child == root) {
        return;
      }
      swap(&heap[root], &heap[child]);
      root = child;
    }
  };
  if (count < 2) {
    return;
  }
  std::uint32_t start = count / 2u;
  while (start > 0) {
    --start;
    sift_down(item, start, count);
  }
  std::uint32_t end = count;
  while (end > 1) {
    --end;
    swap(&item[0], &item[end]);
    sift_down(item, 0, end);
  }
}

namespace {

std::uint64_t mix_vnode(MemberEndpoint endpoint, std::uint32_t index) noexcept {
  std::byte raw[10];
  raw[0] = static_cast<std::byte>(endpoint.addr & 0xffu);
  raw[1] = static_cast<std::byte>((endpoint.addr >> 8) & 0xffu);
  raw[2] = static_cast<std::byte>((endpoint.addr >> 16) & 0xffu);
  raw[3] = static_cast<std::byte>((endpoint.addr >> 24) & 0xffu);
  raw[4] = static_cast<std::byte>(endpoint.port & 0xffu);
  raw[5] = static_cast<std::byte>((endpoint.port >> 8) & 0xffu);
  raw[6] = static_cast<std::byte>(index & 0xffu);
  raw[7] = static_cast<std::byte>((index >> 8) & 0xffu);
  raw[8] = static_cast<std::byte>((index >> 16) & 0xffu);
  raw[9] = static_cast<std::byte>((index >> 24) & 0xffu);
  return fnv1a64(std::span<const std::byte>(raw, sizeof(raw)));
}

}  // namespace

bool HashRing::set_members(std::span<const MemberEndpoint> members) noexcept {
  members_ = 0;
  vnodes_ = 0;
  if (members.empty() || members.size() > kHashRingMaxMembers) {
    return false;
  }
  for (std::size_t i = 0; i < members.size(); ++i) {
    for (std::size_t j = 0; j < i; ++j) {
      if (members[i] == member_[j]) {
        members_ = 0;
        return false;
      }
    }
    member_[i] = members[i];
  }
  members_ = static_cast<std::uint32_t>(members.size());
  std::uint32_t n = 0;
  for (std::uint32_t m = 0; m < members_; ++m) {
    for (std::uint32_t v = 0; v < kVnodesPerMember; ++v) {
      vnode_[n].hash = mix_vnode(member_[m], v);
      vnode_[n].member = m;
      ++n;
    }
  }
  sort_vnodes(vnode_, n);
  vnodes_ = n;
  return true;
}

std::uint32_t HashRing::owners(std::uint64_t key, MemberEndpoint* out) const noexcept {
  if (members_ == 0 || out == nullptr || vnodes_ == 0) {
    return 0;
  }
  std::uint32_t lo = 0;
  std::uint32_t hi = vnodes_;
  while (lo < hi) {
    const std::uint32_t mid = lo + (hi - lo) / 2u;
    if (vnode_[mid].hash < key) {
      lo = mid + 1u;
    } else {
      hi = mid;
    }
  }
  const std::uint32_t start = lo == vnodes_ ? 0u : lo;
  const std::uint32_t want = members_ < kReplicaCount ? members_ : kReplicaCount;
  std::uint32_t seen[kReplicaCount];
  std::uint32_t found = 0;
  for (std::uint32_t step = 0; step < vnodes_ && found < want; ++step) {
    const std::uint32_t member = vnode_[(start + step) % vnodes_].member;
    bool dup = false;
    for (std::uint32_t i = 0; i < found; ++i) {
      if (seen[i] == member) {
        dup = true;
        break;
      }
    }
    if (dup) {
      continue;
    }
    seen[found] = member;
    out[found] = member_[member];
    ++found;
  }
  return found;
}

std::uint64_t HashRing::key_of(std::string_view text) noexcept {
  return fnv1a64(std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()));
}

}  // namespace kestrel

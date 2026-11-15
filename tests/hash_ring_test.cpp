#include "kestrel/hash_ring.hpp"
#include "check.hpp"

#include <cstdio>
#include <span>

int main() {
  kestrel::MemberEndpoint members[3] = {{0x7f000001u, 1}, {0x7f000001u, 2}, {0x7f000001u, 3}};
  kestrel::HashRing ring;
  CHECK(!ring.set_members({}));
  CHECK(ring.set_members(std::span<const kestrel::MemberEndpoint>(members, 3)));
  CHECK(ring.member_count() == 3);

  kestrel::MemberEndpoint dup[2] = {members[0], members[0]};
  kestrel::HashRing bad;
  CHECK(!bad.set_members(std::span<const kestrel::MemberEndpoint>(dup, 2)));

  const std::uint64_t key = kestrel::HashRing::key_of("ping");
  CHECK(key == kestrel::HashRing::key_of("ping"));
  CHECK(key != kestrel::HashRing::key_of("pong"));

  kestrel::MemberEndpoint owners[2]{};
  CHECK(ring.owners(key, owners) == 2);
  CHECK(!(owners[0] == owners[1]));
  bool known0 = owners[0] == members[0] || owners[0] == members[1] || owners[0] == members[2];
  bool known1 = owners[1] == members[0] || owners[1] == members[1] || owners[1] == members[2];
  CHECK(known0);
  CHECK(known1);

  kestrel::HashRing again;
  CHECK(again.set_members(std::span<const kestrel::MemberEndpoint>(members, 3)));
  kestrel::MemberEndpoint owners2[2]{};
  CHECK(again.owners(key, owners2) == 2);
  CHECK(owners2[0] == owners[0]);
  CHECK(owners2[1] == owners[1]);

  kestrel::MemberEndpoint one[1] = {members[0]};
  kestrel::HashRing single;
  CHECK(single.set_members(std::span<const kestrel::MemberEndpoint>(one, 1)));
  CHECK(single.owners(key, owners) == 1);
  CHECK(owners[0] == members[0]);

  std::uint32_t hits[3] = {};
  for (std::uint32_t i = 0; i < 300; ++i) {
    char text[16];
    std::snprintf(text, sizeof(text), "k%u", i);
    const std::uint32_t n = ring.owners(kestrel::HashRing::key_of(text), owners);
    CHECK(n == 2);
    for (std::uint32_t o = 0; o < n; ++o) {
      for (std::uint32_t m = 0; m < 3; ++m) {
        if (owners[o] == members[m]) {
          ++hits[m];
        }
      }
    }
  }
  CHECK(hits[0] > 50);
  CHECK(hits[1] > 50);
  CHECK(hits[2] > 50);
  return kestrel::test::failures.load() == 0 ? 0 : 1;
}

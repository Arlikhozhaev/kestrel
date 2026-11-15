#include "kestrel/swim.hpp"
#include "check.hpp"

int main() {
  constexpr kestrel::MemberEndpoint kA{1, 1};
  constexpr kestrel::MemberEndpoint kB{1, 2};
  constexpr kestrel::MemberEndpoint kC{1, 3};

  kestrel::Swim swim;
  swim.configure(100, 50, 200);
  swim.start(kA, 1);
  CHECK(swim.seed(kB));
  CHECK(swim.seed(kC));
  CHECK(swim.count() == 3);

  kestrel::MemberEndpoint alive[4]{};
  CHECK(swim.alive(alive, 4) == 3);

  kestrel::SwimAction action[8]{};
  const std::uint32_t pinged = swim.tick(0, action, 8);
  CHECK(pinged == 1);
  CHECK(action[0].kind == kestrel::SwimActionKind::Ping);
  CHECK(action[0].about == kA);

  const kestrel::MemberEndpoint target = action[0].to;
  CHECK(swim.tick(40, action, 8) == 0);

  const std::uint32_t indirect = swim.tick(50, action, 8);
  CHECK(indirect >= 1);
  CHECK(action[0].kind == kestrel::SwimActionKind::PingReq);
  CHECK(action[0].about == target);

  const std::uint32_t suspected = swim.tick(100, action, 8);
  CHECK(suspected >= 1);
  bool saw_suspect = false;
  for (std::uint32_t i = 0; i < suspected; ++i) {
    if (action[i].kind == kestrel::SwimActionKind::Member && action[i].status == kestrel::SwimStatus::Suspected &&
        action[i].about == target) {
      saw_suspect = true;
    }
  }
  CHECK(saw_suspect);

  const std::uint32_t dead = swim.tick(300, action, 8);
  bool saw_dead = false;
  for (std::uint32_t i = 0; i < dead; ++i) {
    if (action[i].kind == kestrel::SwimActionKind::Member && action[i].status == kestrel::SwimStatus::Dead &&
        action[i].about == target) {
      saw_dead = true;
    }
  }
  CHECK(saw_dead);
  CHECK(swim.alive(alive, 4) == 2);

  kestrel::Swim refute;
  refute.start(kA, 4);
  const std::uint32_t n = refute.observe(kA, kestrel::SwimStatus::Suspected, 4, 0, action, 8);
  CHECK(n == 1);
  CHECK(action[0].kind == kestrel::SwimActionKind::Member);
  CHECK(action[0].status == kestrel::SwimStatus::Alive);
  CHECK(action[0].incarnation == 5);
  CHECK(refute.incarnation() == 5);

  kestrel::Swim ack;
  ack.configure(100, 50, 200);
  ack.start(kA, 1);
  CHECK(ack.seed(kB));
  CHECK(ack.tick(0, action, 8) == 1);
  const std::uint32_t cleared = ack.on_ack(action[0].to, 1, 10, action, 8);
  CHECK(cleared == 0 || action[0].kind != kestrel::SwimActionKind::Ping);
  CHECK(ack.tick(60, action, 8) == 0);
  return kestrel::test::failures.load() == 0 ? 0 : 1;
}

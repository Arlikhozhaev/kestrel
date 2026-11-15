#include "kestrel/circuit.hpp"
#include "check.hpp"

int main() {
  kestrel::Circuit circuit;
  circuit.failure_limit = 3;
  circuit.cooldown_ms = 100;
  CHECK(circuit.allow(0));
  circuit.fail(0);
  circuit.fail(0);
  CHECK(circuit.allow(0));
  circuit.fail(0);
  CHECK(circuit.state == kestrel::Circuit::State::Open);
  CHECK(!circuit.allow(50));
  CHECK(circuit.allow(100));
  CHECK(circuit.state == kestrel::Circuit::State::HalfOpen);
  CHECK(!circuit.allow(100));
  circuit.fail(100);
  CHECK(circuit.state == kestrel::Circuit::State::Open);
  CHECK(circuit.allow(200));
  circuit.succeed();
  CHECK(circuit.state == kestrel::Circuit::State::Closed);
  CHECK(circuit.failures == 0);
  CHECK(circuit.allow(200));
  return kestrel::test::failures.load() == 0 ? 0 : 1;
}

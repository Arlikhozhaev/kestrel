#include "kestrel/cpu.hpp"
#include "check.hpp"

#include <cstdio>

namespace {

void test_topology_and_pin() {
  kestrel::Topology topology;
  CHECK(kestrel::load_topology(topology));
  CHECK(topology.logical_count > 0);
  CHECK(topology.physical_count > 0);
  CHECK(topology.physical_count <= topology.logical_count);

  for (int i = 0; i < topology.physical_count; ++i) {
    const int cpu = topology.reactor_cpus[i];
    CHECK(cpu >= 0);
    for (int j = 0; j < i; ++j) {
      CHECK(topology.reactor_cpus[j] != cpu);
    }
    const int sibling = topology.sibling_of_cpu(cpu);
    if (sibling >= 0) {
      CHECK(topology.sibling_of_cpu(sibling) == cpu);
      for (int j = 0; j < topology.physical_count; ++j) {
        CHECK(topology.reactor_cpus[j] != sibling);
      }
    }
  }

  // Phase 0 development host: i7-8650U, 4 cores, SMT on, pairs (0,1) (2,3) (4,5) (6,7).
  if (topology.logical_count == 8 && topology.physical_count == 4) {
    CHECK(topology.reactor_cpus[0] == 0);
    CHECK(topology.reactor_cpus[1] == 2);
    CHECK(topology.reactor_cpus[2] == 4);
    CHECK(topology.reactor_cpus[3] == 6);
    CHECK(topology.sibling_of_cpu(0) == 1);
    CHECK(topology.sibling_of_cpu(1) == 0);
    CHECK(topology.sibling_of_cpu(2) == 3);
    CHECK(topology.sibling_of_cpu(3) == 2);
    CHECK(topology.sibling_of_cpu(4) == 5);
    CHECK(topology.sibling_of_cpu(5) == 4);
    CHECK(topology.sibling_of_cpu(6) == 7);
    CHECK(topology.sibling_of_cpu(7) == 6);
  }

  const int target = topology.reactor_cpus[0];
  CHECK(kestrel::pin_thread_to(target));
  CHECK(kestrel::current_cpu() == target);
  if (topology.physical_count >= 2) {
    const int other = topology.reactor_cpus[1];
    CHECK(kestrel::pin_thread_to(other));
    CHECK(kestrel::current_cpu() == other);
  }
  CHECK(!kestrel::pin_thread_to(-1));
  CHECK(kestrel::reset_affinity(topology));
}

}  // namespace

int main() {
  test_topology_and_pin();
  const int failed = kestrel::test::failures.load();
  if (failed == 0) {
    std::puts("cpu_test ok");
  }
  return failed == 0 ? 0 : 1;
}

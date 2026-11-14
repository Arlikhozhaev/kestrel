#pragma once

#include <cstddef>

namespace kestrel {

// One logical CPU per physical core: the lowest-numbered sibling.
// On the Phase 0 host the pairs are (0,1) (2,3) (4,5) (6,7), so the
// reactor set is 0, 2, 4, 6. Pinning both siblings of a pair makes them
// share a 32 KiB L1d and the same execution units. That is the wrong
// place to put two hot reactors.
//
// Guest affinity is real (pthread_setaffinity_np / sched_getcpu). On WSL2
// the host can still migrate the VM's vCPUs, so a number taken here is a
// development number, not the launch p99.9 record.
inline constexpr int kMaxLogicalCpus = 256;

struct Topology {
  int logical_count{0};
  int physical_count{0};
  int logical_ids[kMaxLogicalCpus]{};
  int reactor_cpus[kMaxLogicalCpus]{};
  int sibling_of[kMaxLogicalCpus]{};

  Topology() noexcept {
    for (int& sibling : sibling_of) {
      sibling = -1;
    }
  }

  [[nodiscard]] int sibling_of_cpu(int cpu) const noexcept {
    if (cpu < 0 || cpu >= kMaxLogicalCpus) {
      return -1;
    }
    return sibling_of[cpu];
  }
};

// Reads /sys/devices/system/cpu. Returns false if sysfs is missing,
// unparsable, or names a cpu id >= kMaxLogicalCpus.
[[nodiscard]] bool load_topology(Topology& out) noexcept;

[[nodiscard]] bool pin_thread_to(int cpu) noexcept;
[[nodiscard]] bool reset_affinity(const Topology& topology) noexcept;
[[nodiscard]] int current_cpu() noexcept;

}  // namespace kestrel

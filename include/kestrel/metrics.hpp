#pragma once

#include "kestrel/hdr.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

#include <time.h>

namespace kestrel {

[[nodiscard]] inline std::uint64_t steady_us() noexcept {
  timespec ts{};
  ::clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000000ull +
         static_cast<std::uint64_t>(ts.tv_nsec) / 1000ull;
}

enum class ObserveKind : std::uint8_t { Hit = 0, Fill = 1, Copy = 2, Error = 3 };

inline constexpr int kObserveKinds = 4;
inline constexpr int kTraceSlots = 16;

// Private to the reactor thread. record() does not synchronize.
struct CoreStats {
  Hdr hdr[kObserveKinds]{};
  std::uint64_t trace[kTraceSlots]{};
  std::uint8_t trace_n = 0;
  std::uint8_t trace_i = 0;
  bool dirty = false;

  void record(ObserveKind kind, std::uint64_t us) noexcept {
    dirty = true;
    hdr[static_cast<int>(kind)].record(us);
    trace[trace_i] = (us << 8) | static_cast<std::uint64_t>(kind);
    trace_i = static_cast<std::uint8_t>((trace_i + 1) & (kTraceSlots - 1));
    if (trace_n < kTraceSlots) {
      ++trace_n;
    }
  }
};

// Published copy. The owner writes it under `seq`. A scrape retries while `seq` is odd.
struct Published {
  std::atomic<std::uint64_t> seq{0};
  std::atomic<std::uint64_t> total[kObserveKinds]{};
  std::atomic<std::uint64_t> sum_us[kObserveKinds]{};
  std::atomic<std::uint64_t> counts[kObserveKinds][kHdrLen];
  std::atomic<std::uint64_t> trace[kTraceSlots]{};
};

void publish_stats(CoreStats& src, Published& dst) noexcept;

[[nodiscard]] std::size_t write_prometheus(const Published* slots, int cores, std::span<std::byte> dest) noexcept;

[[nodiscard]] std::size_t write_debug(const Published* slots, int cores, bool drop_upstream, bool drop_peers,
                                     std::span<std::byte> dest) noexcept;

}  // namespace kestrel

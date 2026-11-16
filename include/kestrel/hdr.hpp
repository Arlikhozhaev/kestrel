#pragma once

#include <cstdint>

namespace kestrel {

// HDR histogram, 2 significant figures, 1 microsecond units, up to 60 seconds.
// The bucket index is the one from Gil Tene's histogram: a magnitude, then
// 128 linear sub-buckets inside it. One thread records. A scrape reads a copy.

inline constexpr int kHdrSubHalfMag = 7;
inline constexpr int kHdrUnitMag = 0;
inline constexpr int kHdrSubHalf = 1 << kHdrSubHalfMag;
inline constexpr int kHdrSubMask = (kHdrSubHalf << 1) - 1;
inline constexpr std::uint64_t kHdrHighestUs = 60ull * 1000ull * 1000ull;

[[nodiscard]] constexpr int hdr_bucket(std::uint64_t value) noexcept {
  const int ceil_log = 64 - __builtin_clzll(value | static_cast<std::uint64_t>(kHdrSubMask));
  return ceil_log - kHdrUnitMag - (kHdrSubHalfMag + 1);
}

[[nodiscard]] constexpr int hdr_counts_index(std::uint64_t value) noexcept {
  const int bucket = hdr_bucket(value);
  const int sub = static_cast<int>(value >> (bucket + kHdrUnitMag));
  return ((bucket + 1) << kHdrSubHalfMag) + (sub - kHdrSubHalf);
}

[[nodiscard]] constexpr int hdr_len_for(std::uint64_t highest) noexcept {
  return (hdr_bucket(highest) + 2) * kHdrSubHalf;
}

inline constexpr int kHdrLen = hdr_len_for(kHdrHighestUs);
static_assert(kHdrLen > kHdrSubHalf && kHdrLen < 4096);

// Lowest value that lands in `index`. Index 0 is unused.
[[nodiscard]] constexpr std::uint64_t hdr_lowest(int index) noexcept {
  if (index < kHdrSubHalf) {
    return static_cast<std::uint64_t>(index);
  }
  const int bucket = (index >> kHdrSubHalfMag) - 1;
  const int sub = (index & (kHdrSubHalf - 1)) + kHdrSubHalf;
  return static_cast<std::uint64_t>(sub) << (bucket + kHdrUnitMag);
}

struct Hdr {
  std::uint64_t counts[kHdrLen]{};
  std::uint64_t total = 0;
  std::uint64_t sum_us = 0;

  void record(std::uint64_t us) noexcept {
    if (us < 1) {
      us = 1;
    }
    if (us > kHdrHighestUs) {
      us = kHdrHighestUs;
    }
    const int index = hdr_counts_index(us);
    ++counts[index];
    ++total;
    sum_us += us;
  }

  void add(const Hdr& other) noexcept {
    total += other.total;
    sum_us += other.sum_us;
    for (int i = 0; i < kHdrLen; ++i) {
      counts[i] += other.counts[i];
    }
  }

  // `p` is 0..100. Returns the lowest value of the bucket that crosses p.
  [[nodiscard]] std::uint64_t percentile(int p) const noexcept {
    if (total == 0 || p <= 0) {
      return 0;
    }
    if (p > 100) {
      p = 100;
    }
    const std::uint64_t need = (total * static_cast<std::uint64_t>(p) + 99ull) / 100ull;
    std::uint64_t seen = 0;
    for (int i = 0; i < kHdrLen; ++i) {
      seen += counts[i];
      if (seen >= need) {
        return hdr_lowest(i);
      }
    }
    return kHdrHighestUs;
  }
};

}  // namespace kestrel

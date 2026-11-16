#include "kestrel/hdr.hpp"
#include "check.hpp"

#include <cstdint>

int main() {
  CHECK(kestrel::hdr_counts_index(1) >= 0);
  CHECK(kestrel::hdr_counts_index(1) < kestrel::kHdrLen);
  CHECK(kestrel::hdr_counts_index(1) < kestrel::hdr_counts_index(1000));
  CHECK(kestrel::hdr_counts_index(1000) < kestrel::hdr_counts_index(1000000));

  const std::uint64_t samples[] = {1, 2, 50, 127, 128, 255, 256, 1000, 1500, 1000000, kestrel::kHdrHighestUs};
  int previous = -1;
  for (const std::uint64_t value : samples) {
    const int index = kestrel::hdr_counts_index(value);
    CHECK(index > previous);
    CHECK(index < kestrel::kHdrLen);
    const std::uint64_t low = kestrel::hdr_lowest(index);
    const std::uint64_t high = kestrel::hdr_lowest(index + 1);
    CHECK(low <= value);
    CHECK(value < high);
    previous = index;
  }

  kestrel::Hdr hdr;
  for (int i = 0; i < 100; ++i) {
    hdr.record(1000);
  }
  CHECK(hdr.total == 100);
  CHECK(hdr.sum_us == 100000);
  const std::uint64_t p50 = hdr.percentile(50);
  const int index = kestrel::hdr_counts_index(1000);
  CHECK(p50 == kestrel::hdr_lowest(index));
  CHECK(p50 <= 1000);
  CHECK(1000 < kestrel::hdr_lowest(index + 1));
  return kestrel::test::failures.load() == 0 ? 0 : 1;
}

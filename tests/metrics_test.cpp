#include "kestrel/metrics.hpp"
#include "check.hpp"

#include <cstring>
#include <span>

int main() {
  kestrel::Published published;
  kestrel::CoreStats stats;
  stats.record(kestrel::ObserveKind::Hit, 1000);
  kestrel::publish_stats(stats, published);
  CHECK(!stats.dirty);

  char buf[8192];
  const std::size_t n = kestrel::write_prometheus(&published, 1, std::span<std::byte>(reinterpret_cast<std::byte*>(buf), sizeof(buf) - 1));
  CHECK(n > 0);
  CHECK(n < sizeof(buf));
  buf[n] = '\0';
  CHECK(std::strstr(buf, "kestrel_request_duration_seconds_count{result=\"hit\"} 1\n") != nullptr);
  CHECK(std::strstr(buf, "result=\"fill\"} 0\n") != nullptr);

  char page[16384];
  const std::size_t page_n = kestrel::write_debug(&published, 1, true, false, std::span<std::byte>(reinterpret_cast<std::byte*>(page), sizeof(page) - 1));
  CHECK(page_n > 0);
  page[page_n] = '\0';
  CHECK(std::strstr(page, "hit 1000us") != nullptr);
  CHECK(std::strstr(page, "upstream dropped") != nullptr);
  CHECK(std::strstr(page, "<form method=POST action=/chaos>") != nullptr);
  return kestrel::test::failures.load() == 0 ? 0 : 1;
}

#pragma once

#include <atomic>
#include <cstdio>

namespace kestrel::test {

inline std::atomic<int> failures{0};

inline void check(bool condition, const char* expression, const char* file, int line) noexcept {
  if (!condition) {
    std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expression);
    failures.fetch_add(1, std::memory_order_relaxed);
  }
}

}  // namespace kestrel::test

#define CHECK(condition) \
  ::kestrel::test::check(static_cast<bool>(condition), #condition, __FILE__, __LINE__)

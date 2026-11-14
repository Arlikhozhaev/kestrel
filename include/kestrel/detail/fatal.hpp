#pragma once

#include <cstdio>
#include <cstdlib>

namespace kestrel::detail {

[[noreturn]] inline void fatal(const char* message) noexcept {
  if (message != nullptr) {
    std::fputs(message, stderr);
    std::fputc('\n', stderr);
  }
  std::fflush(stderr);
  std::abort();
}

}  // namespace kestrel::detail

#pragma once

#include <cstdint>

namespace kestrel {

// Per-peer and per-upstream breaker. One reactor thread owns one instance.
// There is no lock. Open rejects work until cooldown_ms has passed, then a
// single half-open trial is allowed. A failure during that trial reopens.
struct Circuit {
  std::uint32_t failure_limit = 5;
  std::uint32_t cooldown_ms = 1000;
  std::uint32_t failures = 0;
  std::uint64_t open_until_ms = 0;
  bool trial = false;

  enum class State : std::uint8_t { Closed, Open, HalfOpen };
  State state = State::Closed;

  [[nodiscard]] bool allow(std::uint64_t now_ms) noexcept {
    if (state == State::Open) {
      if (now_ms < open_until_ms) {
        return false;
      }
      state = State::HalfOpen;
      trial = false;
    }
    if (state == State::HalfOpen) {
      if (trial) {
        return false;
      }
      trial = true;
    }
    return true;
  }

  void succeed() noexcept {
    failures = 0;
    trial = false;
    state = State::Closed;
  }

  void fail(std::uint64_t now_ms) noexcept {
    trial = false;
    if (state == State::HalfOpen || failures + 1u >= failure_limit) {
      failures = failure_limit;
      state = State::Open;
      open_until_ms = now_ms + cooldown_ms;
      return;
    }
    ++failures;
  }
};

}  // namespace kestrel

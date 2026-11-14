#pragma once

#include <cstddef>
#include <cstdint>

// Host this was frozen against (WSL2, measured):
//   Intel Core i7-8650U, x86_64, no AVX-512
//   L1d 32 KiB 8-way, L2 256 KiB, L3 8 MiB, coherency line 64 B
//   Kaby Lake adjacent-line prefetcher pairs lines into 128 B chunks
//
// kL2BudgetBytes is the hard ceiling for one SPSC slot array. Override it
// when the target core has a different L2:
//   -DKESTREL_L2_BYTES=1048576
#ifndef KESTREL_L2_BYTES
#define KESTREL_L2_BYTES (256 * 1024)
#endif

namespace kestrel {

inline constexpr std::size_t kCacheLineBytes = 64;
inline constexpr std::size_t kFalseSharingStride = 128;
inline constexpr std::size_t kL1DataBudgetBytes = 32 * 1024;
inline constexpr std::size_t kL2BudgetBytes = KESTREL_L2_BYTES;

static_assert(kCacheLineBytes == 64, "x86_64 coherency line is 64 bytes");
static_assert(kFalseSharingStride == 2 * kCacheLineBytes,
              "producer and consumer index blocks must sit in different "
              "128-byte prefetcher pairs");

// Pause the SMT sibling without a syscall. The ring never calls this;
// a saturated producer uses it only inside push_bounded().
inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  asm volatile("pause" ::: "memory");
#elif defined(__aarch64__)
  asm volatile("yield" ::: "memory");
#else
  asm volatile("" ::: "memory");
#endif
}

}  // namespace kestrel

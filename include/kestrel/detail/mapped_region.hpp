#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>

namespace kestrel::detail {

// Startup-only anonymous mapping. The hot path never calls mmap.
// Pages are write-prefaulted so the first request does not take a minor fault
// (that fault is a p99.9 spike). Huge pages are not requested: this WSL2 host
// reports HugePages_Total == 0, and MAP_HUGETLB would fail closed.
struct MappedRegion {
  std::byte* base{nullptr};
  std::size_t bytes{0};

  MappedRegion() noexcept = default;

  MappedRegion(const MappedRegion&) = delete;
  MappedRegion& operator=(const MappedRegion&) = delete;

  MappedRegion(MappedRegion&& other) noexcept
      : base(other.base), bytes(other.bytes) {
    other.base = nullptr;
    other.bytes = 0;
  }

  MappedRegion& operator=(MappedRegion&& other) noexcept {
    if (this != &other) {
      unmap();
      base = other.base;
      bytes = other.bytes;
      other.base = nullptr;
      other.bytes = 0;
    }
    return *this;
  }

  ~MappedRegion() { unmap(); }

  [[nodiscard]] static std::size_t page_bytes() noexcept {
    const long page = ::sysconf(_SC_PAGESIZE);
    if (page <= 0) {
      return 4096;
    }
    return static_cast<std::size_t>(page);
  }

  // Returns an empty region on failure (base == nullptr). `bytes == 0` is
  // an empty success with base == nullptr; callers that need memory must
  // pass bytes > 0 and check base.
  [[nodiscard]] static MappedRegion map(std::size_t bytes, bool prefault) noexcept {
    MappedRegion region;
    if (bytes == 0) {
      return region;
    }
    const std::size_t page = page_bytes();
    if (page == 0 || bytes > SIZE_MAX - (page - 1)) {
      std::fputs("kestrel: mmap size overflow\n", stderr);
      return region;
    }
    const std::size_t rounded = (bytes + page - 1) & ~(page - 1);
    void* memory = ::mmap(nullptr, rounded, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) {
      std::fprintf(stderr, "kestrel: mmap(%zu) failed: %s\n", rounded,
                   std::strerror(errno));
      return region;
    }
    region.base = static_cast<std::byte*>(memory);
    region.bytes = rounded;
    if (prefault) {
      region.prefault();
    }
    return region;
  }

  void unmap() noexcept {
    if (base == nullptr) {
      return;
    }
    if (::munmap(base, bytes) != 0) {
      std::fprintf(stderr, "kestrel: munmap(%zu) failed: %s\n", bytes,
                   std::strerror(errno));
    }
    base = nullptr;
    bytes = 0;
  }

  // Write one byte per page. A read-only prefault leaves the page clean;
  // the first store still faults and perturbs tail latency.
  void prefault() noexcept {
    if (base == nullptr || bytes == 0) {
      return;
    }
    const std::size_t page = page_bytes();
    volatile std::byte* cursor = base;
    for (std::size_t offset = 0; offset < bytes; offset += page) {
      cursor[offset] = std::byte{0};
    }
  }

  // Optional. Fails closed (returns false) when RLIMIT_MEMLOCK is too small.
  // Not used on the hot path and not required for correctness.
  [[nodiscard]] bool lock_pages() noexcept {
    if (base == nullptr || bytes == 0) {
      return false;
    }
    if (::mlock(base, bytes) != 0) {
      return false;
    }
    return true;
  }

  void unlock_pages() noexcept {
    if (base == nullptr || bytes == 0) {
      return;
    }
    ::munlock(base, bytes);
  }
};

}  // namespace kestrel::detail

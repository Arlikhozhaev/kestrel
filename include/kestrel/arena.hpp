#pragma once

#include "kestrel/detail/mapped_region.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

#include <sys/mman.h>

namespace kestrel {

// Core-local bump allocator. One owner thread. No locks, no free of
// individual objects. A request-scoped region is `mark()` on entry and
// `rewind()` on the way out; `reset()` drops the whole generation.
//
// Destructors are not run. Putting a non-trivial owner (std::string,
// unique_ptr, a buffer that must be closed) in the arena is a contract
// violation. The arena holds bytes and trivially destructible records.
//
// Hot path: align the cursor, bounds-check, bump. Failure is nullptr.
// That nullptr is backpressure, same as a full ring: stop ingesting.
// nullptr is also returned when `alignment` is not a power of two.
//
// `reset()` keeps the pages resident. `discard()` is the idle path and
// may return them to the kernel; do not call it while any pointer into
// the arena is live, and do not call it on the request path.
class Arena {
 public:
  explicit Arena(std::size_t bytes) noexcept : region_(detail::MappedRegion::map(bytes, true)) {}

  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  Arena(Arena&& other) noexcept : region_(std::move(other.region_)), offset_(other.offset_) {
    other.offset_ = 0;
  }

  Arena& operator=(Arena&& other) noexcept {
    if (this != &other) {
      region_ = std::move(other.region_);
      offset_ = other.offset_;
      other.offset_ = 0;
    }
    return *this;
  }

  ~Arena() = default;

  // False when the caller asked for memory and mmap failed.
  // A zero-byte arena is not ok: it cannot serve a request.
  [[nodiscard]] bool ok() const noexcept { return region_.base != nullptr && region_.bytes > 0; }

  [[nodiscard]] std::size_t capacity() const noexcept { return region_.bytes; }
  [[nodiscard]] std::size_t used() const noexcept { return offset_; }
  [[nodiscard]] std::size_t mark() const noexcept { return offset_; }

  [[nodiscard]] void* allocate(std::size_t size, std::size_t alignment) noexcept {
    if (!ok() || alignment == 0 || (alignment & (alignment - 1)) != 0) {
      return nullptr;
    }
    if (size == 0) {
      size = 1;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(region_.base);
    const auto cursor = base + offset_;
    const auto aligned = (cursor + (alignment - 1)) & ~(alignment - 1);
    if (aligned < cursor || aligned + size < aligned) {
      return nullptr;
    }
    const auto end = base + region_.bytes;
    if (aligned + size > end) {
      return nullptr;
    }
    offset_ = static_cast<std::size_t>((aligned + size) - base);
    return reinterpret_cast<void*>(aligned);
  }

  template <typename T, typename... Args>
    requires std::is_nothrow_constructible_v<T, Args...>
  [[nodiscard]] T* create(Args&&... args) noexcept {
    void* memory = allocate(sizeof(T), alignof(T));
    if (memory == nullptr) {
      return nullptr;
    }
    return std::construct_at(static_cast<T*>(memory), std::forward<Args>(args)...);
  }

  void reset() noexcept { offset_ = 0; }

  // A watermark past used() is rejected and the cursor stays.
  // Rewind does not run destructors. It is illegal while a published Event
  // still names a byte in (watermark, used()]: the consumer may be reading it.
  [[nodiscard]] bool rewind(std::size_t watermark) noexcept {
    if (watermark > offset_) {
      return false;
    }
    offset_ = watermark;
    return true;
  }

  // Idle / shutdown only. Invalidates every pointer into the arena and
  // advises the kernel that the pages can be dropped.
  void discard() noexcept {
    offset_ = 0;
    if (region_.base != nullptr && region_.bytes > 0) {
      ::madvise(region_.base, region_.bytes, MADV_DONTNEED);
    }
  }

  // Best-effort. Returns false when the lock limit refuses us. The arena
  // remains usable either way.
  [[nodiscard]] bool lock_pages() noexcept { return region_.lock_pages(); }
  void unlock_pages() noexcept { region_.unlock_pages(); }

 private:
  detail::MappedRegion region_{};
  std::size_t offset_{0};
};

}  // namespace kestrel

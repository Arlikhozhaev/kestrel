#pragma once

#include "kestrel/detail/fatal.hpp"
#include "kestrel/detail/mapped_region.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace kestrel {

// Core-local fixed-size pool for objects that outlive a single request
// (connection state). Variable-size bytes belong in Arena, not here.
//
// One owner thread. allocate() / deallocate() take no locks.
// Exhaustion returns nullptr (backpressure). nullptr deallocate is a no-op.
// A pointer this slab did not hand out aborts in every build: silently
// ignoring it would corrupt the intrusive freelist.
// Debug builds also abort on double-free and on a second thread.
//
// A free block's first word is the freelist link, so the stride is at least
// sizeof(void*) and the block alignment is at least alignof(void*).
// Alignment is rounded up to a power of two the caller asked for; pass
// kCacheLineBytes when two live objects must not share a line.
class Slab {
 public:
  // A bad size or a failed mmap leaves ok() == false and prints the reason.
  // That is a startup failure, not a hot-path failure: allocate() then
  // returns nullptr. Double-free and foreign pointers still abort.
  Slab(std::size_t block_size, std::size_t block_count, std::size_t alignment) noexcept
      : count_(block_count), requested_(block_size) {
    if (block_size == 0 || block_count == 0 || alignment == 0 ||
        (alignment & (alignment - 1)) != 0) {
      std::fputs("kestrel::Slab: size, count, and alignment must be non-zero "
                 "and alignment must be a power of two\n",
                 stderr);
      count_ = 0;
      return;
    }
    if (alignment < alignof(void*)) {
      alignment = alignof(void*);
    }
    std::size_t stride = block_size;
    if (stride < sizeof(void*)) {
      stride = sizeof(void*);
    }
    const std::size_t rounded = (stride + alignment - 1) & ~(alignment - 1);
    if (rounded < stride) {
      std::fputs("kestrel::Slab: stride overflow\n", stderr);
      count_ = 0;
      return;
    }
    stride_ = rounded;
    if (stride_ == 0 || count_ > (static_cast<std::size_t>(-1) / stride_)) {
      std::fputs("kestrel::Slab: region size overflow\n", stderr);
      count_ = 0;
      stride_ = 0;
      return;
    }
    const std::size_t region_bytes = stride_ * count_;
#ifndef NDEBUG
    const std::size_t state_bytes = count_;
#else
    const std::size_t state_bytes = 0;
#endif
    if (region_bytes > static_cast<std::size_t>(-1) - state_bytes) {
      std::fputs("kestrel::Slab: region size overflow\n", stderr);
      count_ = 0;
      stride_ = 0;
      return;
    }
    region_ = detail::MappedRegion::map(region_bytes + state_bytes, true);
    if (region_.base == nullptr) {
      std::fputs("kestrel::Slab: mmap failed\n", stderr);
      count_ = 0;
      stride_ = 0;
      return;
    }
#ifndef NDEBUG
    states_ = reinterpret_cast<unsigned char*>(region_.base + region_bytes);
    std::memset(states_, static_cast<int>(kUntouched), count_);
#endif
  }

  Slab(const Slab&) = delete;
  Slab& operator=(const Slab&) = delete;

  Slab(Slab&& other) noexcept
      : region_(std::move(other.region_)),
        stride_(other.stride_),
        count_(other.count_),
        requested_(other.requested_),
        high_water_(other.high_water_),
        live_(other.live_),
        free_head_(other.free_head_)
#ifndef NDEBUG
        ,
        states_(other.states_)
#endif
  {
    other.stride_ = 0;
    other.count_ = 0;
    other.requested_ = 0;
    other.high_water_ = 0;
    other.live_ = 0;
    other.free_head_ = nullptr;
#ifndef NDEBUG
    other.states_ = nullptr;
    other.owner_.store(0, std::memory_order_relaxed);
#endif
  }

  Slab& operator=(Slab&&) = delete;

  ~Slab() = default;

  [[nodiscard]] bool ok() const noexcept { return region_.base != nullptr && count_ > 0; }
  [[nodiscard]] std::size_t stride() const noexcept { return stride_; }
  [[nodiscard]] std::size_t block_size() const noexcept { return requested_; }
  [[nodiscard]] std::size_t capacity() const noexcept { return count_; }
  [[nodiscard]] std::size_t live() const noexcept { return live_; }

  [[nodiscard]] void* allocate() noexcept {
    if (!ok()) {
      return nullptr;
    }
    claim();
    void* block = nullptr;
    if (free_head_ != nullptr) {
      block = free_head_;
      free_head_ = free_head_->next;
#ifndef NDEBUG
      const std::size_t index = index_of(block);
      if (states_[index] != kFree) {
        detail::fatal("kestrel::Slab: freelist block is not marked free");
      }
      states_[index] = kLive;
#endif
    } else if (high_water_ < count_) {
      block = region_.base + high_water_ * stride_;
#ifndef NDEBUG
      if (states_[high_water_] != kUntouched) {
        detail::fatal("kestrel::Slab: high-water block was already touched");
      }
      states_[high_water_] = kLive;
#endif
      ++high_water_;
    } else {
      return nullptr;
    }
    ++live_;
    return block;
  }

  void deallocate(void* pointer) noexcept {
    if (pointer == nullptr) {
      return;
    }
    claim();
    if (!owns(pointer)) {
      detail::fatal("kestrel::Slab: deallocate of a pointer this slab does not own");
    }
#ifndef NDEBUG
    const std::size_t index = index_of(pointer);
    if (states_[index] != kLive) {
      detail::fatal("kestrel::Slab: double free");
    }
    states_[index] = kFree;
    std::memset(pointer, 0xDD, stride_);
#endif
    auto* node = static_cast<FreeNode*>(pointer);
    node->next = free_head_;
    free_head_ = node;
    --live_;
  }

  [[nodiscard]] bool owns(const void* pointer) const noexcept {
    if (pointer == nullptr || region_.base == nullptr || stride_ == 0) {
      return false;
    }
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    const auto base = reinterpret_cast<std::uintptr_t>(region_.base);
    if (address < base) {
      return false;
    }
    const auto delta = static_cast<std::size_t>(address - base);
    if (delta % stride_ != 0) {
      return false;
    }
    const std::size_t index = delta / stride_;
    return index < high_water_;
  }

 private:
  struct FreeNode {
    FreeNode* next;
  };

#ifndef NDEBUG
  static constexpr unsigned char kUntouched = 0;
  static constexpr unsigned char kLive = 1;
  static constexpr unsigned char kFree = 2;
#endif

  void claim() noexcept {
#ifndef NDEBUG
    thread_local const char token = 0;
    const auto self = reinterpret_cast<std::uintptr_t>(&token);
    std::uintptr_t expected = 0;
    if (owner_.compare_exchange_strong(expected, self, std::memory_order_relaxed)) {
      return;
    }
    if (expected != self) {
      detail::fatal("kestrel::Slab: used from a second thread");
    }
#endif
  }

  [[nodiscard]] std::size_t index_of(const void* pointer) const noexcept {
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    const auto base = reinterpret_cast<std::uintptr_t>(region_.base);
    return static_cast<std::size_t>(address - base) / stride_;
  }

  detail::MappedRegion region_{};
  std::size_t stride_{0};
  std::size_t count_{0};
  std::size_t requested_{0};
  std::size_t high_water_{0};
  std::size_t live_{0};
  FreeNode* free_head_{nullptr};
#ifndef NDEBUG
  unsigned char* states_{nullptr};
  std::atomic<std::uintptr_t> owner_{0};
#endif
};

}  // namespace kestrel

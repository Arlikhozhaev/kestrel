#pragma once

#include "kestrel/cache_line.hpp"
#include "kestrel/detail/fatal.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace kestrel {

// Single-producer, single-consumer bounded queue.
//
// Memory map (object is 128-byte aligned):
//
//   offset 0    ProducerLine, 128 bytes
//               +0  atomic<uint64_t> write_pos     producer store-release,
//                                                  consumer load-acquire
//               +8  uint64_t cached_read           producer only
//   offset 128  ConsumerLine, 128 bytes
//               +0  atomic<uint64_t> read_pos      consumer store-release,
//                                                  producer load-acquire
//               +8  uint64_t cached_write          consumer only
//   offset 256  Slot[Capacity], packed at sizeof(T), first slot cache-aligned
//
// The 128-byte gap is required on this CPU. Kaby Lake's adjacent-line
// prefetcher pulls the buddy 64-byte line, so alignas(64) between the two
// indices still false-shares. Indices never use memory_order_seq_cst: on
// x86 TSO a seq_cst store is an mfence, and the ring does not need it.
// Release on the publisher and acquire on the observer are the whole protocol.
//
// Fast path (ring not full / not empty): one relaxed load of the thread's
// own index, a compare against the cached remote index, a slot move, one
// release store. The remote atomic is loaded only when the cached index says
// the ring might be full or empty. That load is an L3 transfer on this die
// and is the tail contributor. Batch with try_push_n / try_pop_n so a burst
// pays for the line transfer once.
//
// Failure modes, all of which return false / a short count. The ring does
// not block, drop, or allocate.
//   * full:  caller must stop ingesting (backpressure). See push_bounded().
//   * empty: caller parks. Phase 1 parks on io_uring / epoll, not in here.
//   * a second producer or a second consumer is a data race. Debug builds
//     abort on it. Release builds do not pay for the check.
//   * destroying the ring while either thread is inside it is UB.
//   * try_push moves (or copies) only after a slot is reserved, so a failed
//     push leaves the source intact.
//
// Occupancy invariant: write_pos - read_pos <= Capacity, unsigned, including
// across uint64 wrap. Capacity fits in 32 bits, so the distance cannot be
// confused with a wrapped empty ring. At 1e9 operations/sec a uint64 index
// wraps in ~584 years.
template <typename T, std::size_t Capacity>
class SpscRing {
 public:
  static_assert(Capacity >= 2, "SpscRing needs at least 2 slots");
  static_assert((Capacity & (Capacity - 1)) == 0,
                "SpscRing capacity must be a power of two");
  static_assert(Capacity <= (std::size_t{1} << 32),
                "capacity must stay <= 2^32 so the uint64 distance is unambiguous");
  static_assert(sizeof(T) * Capacity <= kL2BudgetBytes,
                "SpscRing slot array exceeds the L2 budget (default 256 KiB, "
                "i7-8650U). A ring that misses L2 is the p99.9 cliff. Override "
                "with -DKESTREL_L2_BYTES=N for a larger core.");
  static_assert(std::is_nothrow_destructible_v<T>);
  static_assert(std::is_nothrow_move_constructible_v<T>);
  static_assert(std::is_nothrow_move_assignable_v<T>);
  static_assert(std::is_object_v<T>);
  static_assert(!std::is_const_v<T>);
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                "the index protocol requires a lock-free 64-bit atomic; "
                "a locked atomic would put a mutex on the hot path");

  SpscRing() noexcept = default;
  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;
  SpscRing(SpscRing&&) = delete;
  SpscRing& operator=(SpscRing&&) = delete;

  ~SpscRing() {
    // Both threads must already be joined. Destroy leftovers in order.
    const std::uint64_t write = producer_.write_pos.load(std::memory_order_relaxed);
    std::uint64_t read = consumer_.read_pos.load(std::memory_order_relaxed);
    while (read != write) {
      std::destroy_at(slot_live(read));
      ++read;
    }
  }

  [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

  // Quiescent observation is exact. A concurrent observation may exceed
  // capacity (it is not a linearizable snapshot). Never use this to decide
  // whether to push or pop.
  [[nodiscard]] std::uint64_t size_approx() const noexcept {
    const auto read = consumer_.read_pos.load(std::memory_order_acquire);
    const auto write = producer_.write_pos.load(std::memory_order_acquire);
    return write - read;
  }

  template <typename U>
    requires std::is_nothrow_constructible_v<T, U&&>
  [[nodiscard]] bool try_push(U&& value) noexcept {
    std::uint64_t write = 0;
    std::uint64_t free_slots = 0;
    if (!try_reserve_push(write, free_slots)) {
      return false;
    }
    std::construct_at(slot_raw(write), std::forward<U>(value));
    publish_push(write, 1);
    return true;
  }

  template <typename... Args>
    requires std::is_nothrow_constructible_v<T, Args...>
  [[nodiscard]] bool try_emplace(Args&&... args) noexcept {
    std::uint64_t write = 0;
    std::uint64_t free_slots = 0;
    if (!try_reserve_push(write, free_slots)) {
      return false;
    }
    std::construct_at(slot_raw(write), std::forward<Args>(args)...);
    publish_push(write, 1);
    return true;
  }

  // Copies up to n elements. Returns how many were accepted (possibly short
  // when the ring fills). One release store for the whole prefix, which is
  // the point: a burst must not bounce the index line per element.
  // Move-only T has no batch copy; use try_push.
  [[nodiscard]] std::size_t try_push_n(const T* src, std::size_t n) noexcept
    requires std::is_nothrow_copy_constructible_v<T>
  {
    if (src == nullptr || n == 0) {
      return 0;
    }
    std::uint64_t write = 0;
    std::uint64_t free_slots = 0;
    if (!try_reserve_push(write, free_slots)) {
      return 0;
    }
    if (static_cast<std::uint64_t>(n) < free_slots) {
      free_slots = static_cast<std::uint64_t>(n);
    }
    const std::size_t count = static_cast<std::size_t>(free_slots);
    for (std::size_t i = 0; i < count; ++i) {
      std::construct_at(slot_raw(write + static_cast<std::uint64_t>(i)), src[i]);
    }
    publish_push(write, free_slots);
    return count;
  }

  // `out` must be a live object. On false, `out` is unchanged.
  [[nodiscard]] bool try_pop(T& out) noexcept {
    std::uint64_t read = 0;
    std::uint64_t available = 0;
    if (!try_reserve_pop(read, available)) {
      return false;
    }
    T* slot = slot_live(read);
    out = std::move(*slot);
    std::destroy_at(slot);
    publish_pop(read, 1);
    return true;
  }

  // `dest` must point at n live objects. Only the returned prefix is assigned.
  [[nodiscard]] std::size_t try_pop_n(T* dest, std::size_t n) noexcept {
    if (dest == nullptr || n == 0) {
      return 0;
    }
    std::uint64_t read = 0;
    std::uint64_t available = 0;
    if (!try_reserve_pop(read, available)) {
      return 0;
    }
    if (static_cast<std::uint64_t>(n) < available) {
      available = static_cast<std::uint64_t>(n);
    }
    const std::size_t count = static_cast<std::size_t>(available);
    for (std::size_t i = 0; i < count; ++i) {
      T* slot = slot_live(read + static_cast<std::uint64_t>(i));
      dest[i] = std::move(*slot);
      std::destroy_at(slot);
    }
    publish_pop(read, available);
    return count;
  }

  [[nodiscard]] static constexpr std::size_t control_separation() noexcept {
    return offsetof(SpscRing, consumer_);
  }

  [[nodiscard]] static constexpr std::size_t slots_offset() noexcept {
    return offsetof(SpscRing, slots_);
  }

 private:
  static constexpr std::uint64_t kCapacity = Capacity;
  static constexpr std::uint64_t kMask = kCapacity - 1;
  static constexpr std::size_t kSlotAlign =
      alignof(T) > kCacheLineBytes ? alignof(T) : kCacheLineBytes;

  struct alignas(kFalseSharingStride) ProducerLine {
    std::atomic<std::uint64_t> write_pos{0};
    std::uint64_t cached_read{0};
  };
  static_assert(sizeof(ProducerLine) == kFalseSharingStride);

  struct alignas(kFalseSharingStride) ConsumerLine {
    std::atomic<std::uint64_t> read_pos{0};
    std::uint64_t cached_write{0};
  };
  static_assert(sizeof(ConsumerLine) == kFalseSharingStride);

  struct Slot {
    alignas(T) std::byte storage[sizeof(T)];
  };
  static_assert(sizeof(Slot) == sizeof(T));

  // Invariant: write_pos - cached_read <= Capacity, so the subtraction in
  // the free-slot calculation cannot wrap.
  [[nodiscard]] bool try_reserve_push(std::uint64_t& write,
                                      std::uint64_t& free_slots) noexcept {
    claim_producer();
    write = producer_.write_pos.load(std::memory_order_relaxed);
    std::uint64_t occupied = write - producer_.cached_read;
    if (occupied == kCapacity) [[unlikely]] {
      producer_.cached_read = consumer_.read_pos.load(std::memory_order_acquire);
      occupied = write - producer_.cached_read;
      if (occupied == kCapacity) {
        free_slots = 0;
        return false;
      }
    }
    free_slots = kCapacity - occupied;
    return true;
  }

  void publish_push(std::uint64_t write, std::uint64_t count) noexcept {
    producer_.write_pos.store(write + count, std::memory_order_release);
  }

  [[nodiscard]] bool try_reserve_pop(std::uint64_t& read,
                                     std::uint64_t& available) noexcept {
    claim_consumer();
    read = consumer_.read_pos.load(std::memory_order_relaxed);
    available = consumer_.cached_write - read;
    if (available == 0) {
      consumer_.cached_write = producer_.write_pos.load(std::memory_order_acquire);
      available = consumer_.cached_write - read;
      if (available == 0) {
        return false;
      }
    }
    return true;
  }

  void publish_pop(std::uint64_t read, std::uint64_t count) noexcept {
    consumer_.read_pos.store(read + count, std::memory_order_release);
  }

  [[nodiscard]] static std::size_t slot_index(std::uint64_t pos) noexcept {
    return static_cast<std::size_t>(pos & kMask);
  }

  [[nodiscard]] T* slot_raw(std::uint64_t pos) noexcept {
    return reinterpret_cast<T*>(slots_[slot_index(pos)].storage);
  }

  [[nodiscard]] T* slot_live(std::uint64_t pos) noexcept {
    return std::launder(reinterpret_cast<T*>(slots_[slot_index(pos)].storage));
  }

  void claim_producer() noexcept {
#ifndef NDEBUG
    claim(owners_.producer);
#else
    return;
#endif
  }

  void claim_consumer() noexcept {
#ifndef NDEBUG
    claim(owners_.consumer);
#endif
  }

#ifndef NDEBUG
  // Address of a thread_local, not std::thread::id: thread::id is not
  // trivially copyable, and std::atomic requires that. 0 means unclaimed.
  static void claim(std::atomic<std::uintptr_t>& owner) noexcept {
    thread_local const char token = 0;
    const auto self = reinterpret_cast<std::uintptr_t>(&token);
    std::uintptr_t expected = 0;
    if (owner.compare_exchange_strong(expected, self, std::memory_order_relaxed)) {
      return;
    }
    if (expected != self) {
      detail::fatal("kestrel::SpscRing: second producer or second consumer");
    }
  }
#endif

  ProducerLine producer_{};
  ConsumerLine consumer_{};
  alignas(kSlotAlign) Slot slots_[Capacity];

#ifndef NDEBUG
  // Cold. Lives after the slots so the hot prefix (indices, then slots)
  // does not move between debug and release. Not a reference member:
  // a reference would make the ring non-standard-layout and break offsetof.
  struct alignas(kFalseSharingStride) Owners {
    std::atomic<std::uintptr_t> producer{0};
    std::atomic<std::uintptr_t> consumer{0};
  };
  static_assert(sizeof(Owners) == kFalseSharingStride);
  Owners owners_{};
#endif
};

static_assert(SpscRing<std::uint64_t, 2>::control_separation() == kFalseSharingStride);
static_assert(SpscRing<std::uint64_t, 2>::slots_offset() == 2 * kFalseSharingStride);
static_assert(alignof(SpscRing<std::uint64_t, 2>) >= kFalseSharingStride);
static_assert(std::is_standard_layout_v<SpscRing<std::uint64_t, 2>>);

// pause_budget is the number of cpu_relax calls after a failed try, not the
// number of attempts. Budget 0 is a single try. The source is moved only
// when a slot is reserved, so a failed budget leaves it intact.
//
// This is the only spin the core offers, and it is bounded. An unbounded
// spin hides a stalled consumer and shows up as a p99.9 cliff. When this
// returns false the caller stops reading the socket; it does not retry forever.
template <typename Ring, typename V>
[[nodiscard]] bool push_bounded(Ring& ring, V&& value, std::uint32_t pause_budget) noexcept {
  for (std::uint32_t pauses = 0;; ++pauses) {
    if (ring.try_push(std::forward<V>(value))) {
      return true;
    }
    if (pauses >= pause_budget) {
      return false;
    }
    cpu_relax();
  }
}

}  // namespace kestrel

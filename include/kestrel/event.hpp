#pragma once

#include "kestrel/cache_line.hpp"
#include "kestrel/spsc_ring.hpp"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace kestrel {

// In-process currency of a core. The ring carries this descriptor.
// The bytes it names live in that core's Arena at `arena_offset`.
// The ring does not carry HTTP bodies: a 32-byte descriptor keeps a
// 512-deep ring at 16 KiB, which is half of the 32 KiB L1d on the
// i7-8650U. The other half is the reactor's own hot state.
//
// Numeric values are ABI. Do not renumber.
enum class EventType : std::uint16_t {
  None = 0,
  Accept = 1,
  Readable = 2,
  Writable = 3,
  Close = 4,
  CacheLookup = 5,
  CacheHit = 6,
  CacheMiss = 7,
  UpstreamChunk = 8,
  Shutdown = 9,
};

inline constexpr std::uint16_t kEventFlagBorrowed = 1u << 0;
inline constexpr std::uint16_t kEventFlagEndOfStream = 1u << 1;
inline constexpr std::uint32_t kNoArenaOffset = 0xFFFFFFFFu;

// 32 bytes, 32-aligned, so an event never straddles a 64-byte line.
// Two events per line.
struct alignas(32) Event {
  std::uint64_t conn_id{0};
  std::uint32_t arena_offset{kNoArenaOffset};
  std::uint32_t byte_length{0};
  EventType type{EventType::None};
  std::uint16_t flags{0};
  std::uint32_t reserved{0};
};

static_assert(std::is_trivially_copyable_v<Event>);
static_assert(std::is_standard_layout_v<Event>);
static_assert(sizeof(Event) == 32);
static_assert(alignof(Event) == 32);
static_assert(offsetof(Event, conn_id) == 0);
static_assert(offsetof(Event, arena_offset) == 8);
static_assert(offsetof(Event, byte_length) == 12);
static_assert(offsetof(Event, type) == 16);
static_assert(offsetof(Event, flags) == 18);
static_assert(offsetof(Event, reserved) == 20);

// 512 * 32 B = 16 KiB = half of L1d. Depth is power-of-two for the ring mask.
inline constexpr std::size_t kReactorRingCapacity = 512;
using ReactorRing = SpscRing<Event, kReactorRingCapacity>;

static_assert(sizeof(Event) * kReactorRingCapacity == 16 * 1024);
static_assert(sizeof(Event) * kReactorRingCapacity <= kL1DataBudgetBytes / 2);
static_assert(ReactorRing::slots_offset() == 2 * kFalseSharingStride);
static_assert(ReactorRing::control_separation() == kFalseSharingStride);

}  // namespace kestrel

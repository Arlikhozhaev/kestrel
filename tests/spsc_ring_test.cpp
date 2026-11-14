#include "kestrel/cpu.hpp"
#include "kestrel/event.hpp"
#include "kestrel/spsc_ring.hpp"
#include "check.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <utility>

namespace {

struct Probe {
  int value;
  static std::atomic<int> live;

  explicit Probe(int v) noexcept : value(v) { live.fetch_add(1, std::memory_order_relaxed); }

  Probe(const Probe&) = delete;
  Probe& operator=(const Probe&) = delete;

  Probe(Probe&& other) noexcept : value(other.value) {
    other.value = -1;
    live.fetch_add(1, std::memory_order_relaxed);
  }

  Probe& operator=(Probe&& other) noexcept {
    value = other.value;
    other.value = -1;
    return *this;
  }

  ~Probe() { live.fetch_sub(1, std::memory_order_relaxed); }
};

std::atomic<int> Probe::live{0};

void test_order_and_full() {
  kestrel::SpscRing<std::uint64_t, 8> ring;
  CHECK(ring.size_approx() == 0);
  for (std::uint64_t i = 0; i < 8; ++i) {
    CHECK(ring.try_push(i));
  }
  CHECK(ring.size_approx() == 8);
  CHECK(!ring.try_push(std::uint64_t{8}));
  std::uint64_t out = 99;
  CHECK(ring.try_pop(out));
  CHECK(out == 0);
  CHECK(ring.try_push(std::uint64_t{8}));
  for (std::uint64_t expect = 1; expect <= 8; ++expect) {
    CHECK(ring.try_pop(out));
    CHECK(out == expect);
  }
  CHECK(!ring.try_pop(out));
  CHECK(out == 8);
  CHECK(ring.size_approx() == 0);
}

void test_batch_and_mask_wrap() {
  kestrel::SpscRing<std::uint64_t, 8> ring;
  std::uint64_t first[3] = {0, 1, 2};
  CHECK(ring.try_push_n(first, 3) == 3);
  std::uint64_t extra[10] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
  CHECK(ring.try_push_n(extra, 10) == 5);
  CHECK(ring.size_approx() == 8);
  CHECK(ring.try_push_n(extra, 1) == 0);

  std::uint64_t got[8] = {};
  CHECK(ring.try_pop_n(got, 3) == 3);
  CHECK(got[0] == 0 && got[1] == 1 && got[2] == 2);
  CHECK(ring.try_pop_n(got, 10) == 5);
  for (std::uint64_t i = 0; i < 5; ++i) {
    CHECK(got[i] == i + 3);
  }
  CHECK(ring.try_pop_n(got, 4) == 0);

  std::uint64_t value = 0;
  for (int lap = 0; lap < 1000; ++lap) {
    CHECK(ring.try_push(value));
    std::uint64_t popped = 1;
    CHECK(ring.try_pop(popped));
    CHECK(popped == value);
    ++value;
  }
}

void test_failed_push_does_not_consume() {
  CHECK(Probe::live.load() == 0);
  {
    kestrel::SpscRing<Probe, 2> ring;
    CHECK(ring.try_emplace(1));
    CHECK(ring.try_push(Probe{2}));
    CHECK(Probe::live.load() == 2);
    Probe extra{3};
    CHECK(Probe::live.load() == 3);
    CHECK(!ring.try_push(std::move(extra)));
    CHECK(extra.value == 3);
    CHECK(!kestrel::push_bounded(ring, std::move(extra), 4));
    CHECK(extra.value == 3);
    CHECK(Probe::live.load() == 3);

    Probe out{0};
    CHECK(ring.try_pop(out));
    CHECK(out.value == 1);
    CHECK(kestrel::push_bounded(ring, std::move(extra), 0));
    CHECK(extra.value == -1);
  }
  CHECK(Probe::live.load() == 0);
}

void test_destructor_destroys_leftovers() {
  CHECK(Probe::live.load() == 0);
  {
    kestrel::SpscRing<Probe, 4> ring;
    CHECK(ring.try_emplace(7));
    CHECK(ring.try_emplace(8));
    CHECK(Probe::live.load() == 2);
  }
  CHECK(Probe::live.load() == 0);
}

void test_reactor_event_ring() {
  kestrel::ReactorRing ring;
  kestrel::Event event;
  event.conn_id = 42;
  event.arena_offset = 128;
  event.byte_length = 4096;
  event.type = kestrel::EventType::CacheMiss;
  event.flags = kestrel::kEventFlagEndOfStream;
  CHECK(ring.try_push(event));
  kestrel::Event out;
  CHECK(ring.try_pop(out));
  CHECK(out.conn_id == 42);
  CHECK(out.arena_offset == 128);
  CHECK(out.byte_length == 4096);
  CHECK(out.type == kestrel::EventType::CacheMiss);
  CHECK(out.flags == kestrel::kEventFlagEndOfStream);
  CHECK(!ring.try_pop(out));
}

void test_cross_core() {
  constexpr std::uint64_t kCount = 10'000'000;
  kestrel::SpscRing<std::uint64_t, 1024> ring;
  kestrel::Topology topology;
  const bool have_topology = kestrel::load_topology(topology);
  CHECK(have_topology);

  std::atomic<bool> producer_ok{true};
  std::thread producer([&] {
    if (have_topology && topology.physical_count >= 1) {
      if (!kestrel::pin_thread_to(topology.reactor_cpus[0])) {
        producer_ok.store(false);
      }
    }
    for (std::uint64_t i = 0; i < kCount; ++i) {
      while (!ring.try_push(i)) {
        kestrel::cpu_relax();
      }
    }
  });
  std::thread consumer([&] {
    if (have_topology && topology.physical_count >= 2) {
      if (!kestrel::pin_thread_to(topology.reactor_cpus[1])) {
        producer_ok.store(false);
      }
    }
    std::uint64_t expect = 0;
    std::uint64_t got = 0;
    while (expect < kCount) {
      if (!ring.try_pop(got)) {
        kestrel::cpu_relax();
        continue;
      }
      if (got != expect) {
        std::fprintf(stderr, "reordered or lost: got %llu expect %llu\n",
                     static_cast<unsigned long long>(got),
                     static_cast<unsigned long long>(expect));
        producer_ok.store(false);
        return;
      }
      ++expect;
    }
  });
  producer.join();
  consumer.join();
  CHECK(producer_ok.load());
  CHECK(ring.size_approx() == 0);
  if (have_topology) {
    CHECK(kestrel::reset_affinity(topology));
  }
}

}  // namespace

// Cache-Line Alignment & Padding Verification
static_assert(kestrel::SpscRing<int, 1024>::control_separation() >= 128);
static_assert(kestrel::SpscRing<int, 1024>::slots_offset() >= 256);
static_assert(alignof(kestrel::SpscRing<int, 1024>) >= 128);

int main() {
  test_order_and_full();
  test_batch_and_mask_wrap();
  test_failed_push_does_not_consume();
  test_destructor_destroys_leftovers();
  test_reactor_event_ring();
  test_cross_core();
  const int failed = kestrel::test::failures.load();
  if (failed == 0) {
    std::puts("spsc_ring_test ok");
  }
  return failed == 0 ? 0 : 1;
}

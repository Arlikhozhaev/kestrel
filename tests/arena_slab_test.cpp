#include "kestrel/arena.hpp"
#include "kestrel/cache_line.hpp"
#include "kestrel/slab.hpp"
#include "check.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>

#include <sys/wait.h>
#include <unistd.h>

namespace {

template <typename Fn>
void expect_abort(Fn&& fn) {
#if defined(__SANITIZE_THREAD__)
  (void)fn;
  return;
#else
  const pid_t pid = ::fork();
  if (pid < 0) {
    CHECK(false);
    return;
  }
  if (pid == 0) {
    if (std::freopen("/dev/null", "w", stderr) == nullptr) {
      fn();
      std::_Exit(0);
    }
    fn();
    std::_Exit(0);
  }
  int status = 0;
  const pid_t got = ::waitpid(pid, &status, 0);
  CHECK(got == pid);
  CHECK(WIFSIGNALED(status) != 0);
  CHECK(WTERMSIG(status) == SIGABRT);
#endif
}

void test_arena() {
  kestrel::Arena arena(1000);
  CHECK(arena.ok());
  CHECK(arena.capacity() >= 1000);
  CHECK(arena.capacity() % 4096 == 0);
  CHECK(arena.used() == 0);

  void* first = arena.allocate(0, 1);
  void* second = arena.allocate(0, 1);
  CHECK(first != nullptr);
  CHECK(second != nullptr);
  CHECK(first != second);

  void* aligned = arena.allocate(1, 64);
  CHECK(aligned != nullptr);
  CHECK(reinterpret_cast<std::uintptr_t>(aligned) % 64 == 0);
  CHECK(arena.allocate(8, 3) == nullptr);

  const std::size_t watermark = arena.mark();
  void* rolled = arena.allocate(32, 8);
  CHECK(rolled != nullptr);
  CHECK(arena.rewind(watermark));
  CHECK(arena.used() == watermark);
  CHECK(!arena.rewind(arena.used() + 1));
  CHECK(arena.used() == watermark);

  int* value = arena.create<int>(42);
  CHECK(value != nullptr);
  CHECK(*value == 42);

  int allocations = 0;
  while (arena.allocate(1024, 8) != nullptr) {
    ++allocations;
  }
  CHECK(allocations > 0);
  CHECK(arena.allocate(1024, 8) == nullptr);
  // A tail smaller than 1024 can remain. Drain it, then the next byte must fail.
  if (arena.capacity() > arena.used()) {
    const std::size_t tail = arena.capacity() - arena.used();
    CHECK(tail < 1024);
    CHECK(arena.allocate(tail, 1) != nullptr);
  }
  CHECK(arena.allocate(1, 1) == nullptr);
  arena.reset();
  CHECK(arena.used() == 0);
  void* after = arena.allocate(1024, 8);
  CHECK(after != nullptr);
  std::memset(after, 0xAB, 1024);

  const bool locked = arena.lock_pages();
  void* locked_block = arena.allocate(8, 8);
  CHECK(locked_block != nullptr);
  std::memset(locked_block, 0xCD, 8);
  if (locked) {
    arena.unlock_pages();
  }

  arena.discard();
  CHECK(arena.used() == 0);
  void* revived = arena.allocate(8, 8);
  CHECK(revived != nullptr);
  std::memset(revived, 0xEF, 8);

  kestrel::Arena moved(std::move(arena));
  CHECK(moved.ok());
  CHECK(!arena.ok());
  CHECK(arena.allocate(8, 8) == nullptr);
  CHECK(moved.allocate(8, 8) != nullptr);
}

void test_slab() {
  kestrel::Slab bad(0, 4, 8);
  CHECK(!bad.ok());
  CHECK(bad.allocate() == nullptr);

  kestrel::Slab slab(24, 4, kestrel::kCacheLineBytes);
  CHECK(slab.ok());
  CHECK(slab.stride() >= 24);
  CHECK(slab.stride() % kestrel::kCacheLineBytes == 0);
  CHECK(slab.capacity() == 4);
  CHECK(slab.live() == 0);

  void* blocks[4] = {};
  for (int i = 0; i < 4; ++i) {
    blocks[i] = slab.allocate();
    CHECK(blocks[i] != nullptr);
    CHECK(slab.owns(blocks[i]));
    CHECK(reinterpret_cast<std::uintptr_t>(blocks[i]) % kestrel::kCacheLineBytes == 0);
    std::memset(blocks[i], 0x11, slab.stride());
  }
  CHECK(slab.live() == 4);
  CHECK(slab.allocate() == nullptr);
  CHECK(!slab.owns(static_cast<const char*>(blocks[0]) + 1));

  slab.deallocate(nullptr);
  slab.deallocate(blocks[2]);
  CHECK(slab.live() == 3);
  void* reused = slab.allocate();
  CHECK(reused == blocks[2]);
  CHECK(slab.live() == 4);

  int stack = 0;
  expect_abort([&] { slab.deallocate(&stack); });

#if !defined(NDEBUG)
  expect_abort([&] {
    void* block = blocks[0];
    slab.deallocate(block);
    slab.deallocate(block);
  });
#endif

  kestrel::Slab taken(std::move(slab));
  CHECK(taken.ok());
  CHECK(!slab.ok());
  CHECK(slab.allocate() == nullptr);
  CHECK(taken.live() == taken.capacity());
}

}  // namespace

int main() {
  test_arena();
  test_slab();
  const int failed = kestrel::test::failures.load();
  if (failed == 0) {
    std::puts("arena_slab_test ok");
  }
  return failed == 0 ? 0 : 1;
}

# Kestrel

C++20 self-hosted semantic cache and OpenAI-compatible inference gateway.
Shared-nothing cores, no external database, no third-party proxy.

**Status: Phase 0 of 4.** This tree is the in-process foundation. It does not
listen on a socket, it does not cache a prompt, and it publishes no latency
numbers. Those claims start when a test measures them.

## Start here

| | |
| --- | --- |
| Memory layout, wire format, failure rules | [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) |
| SPSC ring | [include/kestrel/spsc_ring.hpp](include/kestrel/spsc_ring.hpp) |
| Frame v1 and CRC-32C | [include/kestrel/frame.hpp](include/kestrel/frame.hpp) |

## What is in this tree

- Cache-line-isolated SPSC ring. Producer and consumer indices are 128 bytes apart (Kaby Lake adjacent-line prefetcher). Release/acquire only.
- Core-local arena (bump pointer, `mmap`, write-prefaulted) and fixed-stride slab.
- 32-byte `Event`, the descriptor the ring carries. Payload bytes stay in the arena.
- Version-1 inter-node frame, 32-byte little-endian header, CRC-32C (the SSE4.2 polynomial).
- Reactor pin set: one logical CPU per physical core, read from sysfs. On the development host (i7-8650U) that is `0, 2, 4, 6`.

## What is not in this tree

| Phase | Contents | State |
| --- | --- | --- |
| 0 | Ring, arena, slab, event, frame | this tree |
| 1 | `io_uring` or epoll, HTTP/SSE, HNSW, WAL | not started |
| 2 | Consistent hashing, SWIM, replication, circuit breaker | not started |
| 3 | HDR histograms, traces, chaos UI | not started |
| 4 | `wrk2`/`k6` numbers, Distroless image | not started |

`kestreld` and `kestrel-cli` are the planned binary names. Neither exists yet.
`liburing` is detected at configure time and is not linked.

## Build

Requires CMake 3.20, g++ with C++20, and pthreads. Development host: g++ 13.3,
Linux 6.6. Release flags: `-O3 -march=native -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti`.
A container image that must run on other AVX2 machines configures `-DKESTREL_MARCH=x86-64-v3`.

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j
ctest --test-dir build/release --output-on-failure

cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DKESTREL_SANITIZER=address
cmake --build build/asan -j
ctest --test-dir build/asan --output-on-failure

cmake -S . -B build/tsan -DCMAKE_BUILD_TYPE=Debug -DKESTREL_SANITIZER=thread
cmake --build build/tsan -j
ctest --test-dir build/tsan --output-on-failure
```

`address` is AddressSanitizer plus UndefinedBehaviorSanitizer. ThreadSanitizer is a separate build; the two do not compose. On this WSL2 kernel, TSan aborts with `unexpected memory mapping` unless the test process disables ASLR. The TSan ctest target runs under `setarch -R`, which sets `ADDR_NO_RANDOMIZE` and does not change the instrumentation.

GitHub Actions (`.github/workflows/ci.yml`) runs the same three builds on Ubuntu.

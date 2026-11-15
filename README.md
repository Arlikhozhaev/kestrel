# Kestrel

C++20 self-hosted semantic cache and OpenAI-compatible inference gateway.
Shared-nothing cores, no external database, no third-party proxy.

**Status: Phase 2 of 4.** One process serves an identical prompt from the on-core
cache and proxies a miss to an IPv4 model server. With `--peer-port` and
`--peers`, two hashed owners store each entry. Any other process asks an owner
once, keeps the copy, and then hits locally. Only an owner calls the model.
The embedder is a feature hash, not a model: paraphrases do not hit. This tree
publishes no latency numbers.

## Start here

| | |
| --- | --- |
| Memory layout, wire format, failure rules | [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) |
| SPSC ring | [include/kestrel/spsc_ring.hpp](include/kestrel/spsc_ring.hpp) |
| Frame v1 and CRC-32C | [include/kestrel/frame.hpp](include/kestrel/frame.hpp) |
| Single-node engine | [include/kestrel/server.hpp](include/kestrel/server.hpp) |

## What is in this tree

- Cache-line-isolated SPSC ring. Producer and consumer indices are 128 bytes apart (Kaby Lake adjacent-line prefetcher). Release/acquire only.
- Core-local arena (bump pointer, `mmap`, write-prefaulted) and fixed-stride slab.
- 32-byte `Event`, the descriptor the ring carries. Payload bytes stay in the arena.
- Version-1 inter-node frame, 32-byte little-endian header, CRC-32C (the SSE4.2 polynomial).
- Reactor pin set: one logical CPU per physical core, read from sysfs. On the development host (i7-8650U) that is `0, 2, 4, 6`.
- One reactor thread per core (`io_uring`, or level-triggered epoll). Each core has its own HNSW and `core-N.wal`. A hit does not sync the log.
- OpenAI-compatible `POST /v1/chat/completions` over HTTP/1.1. Cache hits are a finite JSON or SSE body with `Content-Length`.
- Optional cluster: consistent hash, SWIM, async replication to two owners, and a local copy after the first fetch. Off unless `--peer-port` is set.

## What is not in this tree

| Phase | Contents | State |
| --- | --- | --- |
| 0 | Ring, arena, slab, event, frame | this tree |
| 1 | `io_uring` or epoll, HTTP/SSE, HNSW, WAL | this tree |
| 2 | Consistent hashing, SWIM, replication, circuit breaker | this tree |
| 3 | HDR histograms, traces, chaos UI | not started |
| 4 | `wrk2`/`k6` numbers, Distroless image | not started |

`kestrel-cli` is not written yet. `liburing` is linked when pkg-config finds it; otherwise the reactor uses epoll.

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

```bash
./build/release/kestreld --bind 127.0.0.1 --port 8080 --wal /tmp/kestrel-wal --upstream 127.0.0.1:8000
```

`--epoll` forces the epoll reactor. `--upstream` is `IPv4:PORT`. A miss with no upstream is a 502. `--peer-port` turns the cluster on. `--peers` is `IPv4:PORT,IPv4:PORT` and requires `--peer-port`. `--advertise` is the IPv4 address other processes put in their peer list. `--help` prints the flags. Unknown flags exit 2.

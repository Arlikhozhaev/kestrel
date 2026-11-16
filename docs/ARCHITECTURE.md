# Kestrel phase 0 architecture

Phase 0 freezes the memory layout and the two wire images the rest of the system is built on. Phase 1 is the single-node engine on top of those layouts: one reactor thread per physical core, an HNSW index, and a WAL. Phase 2 adds hashed owners, SWIM, and a local copy after the first fetch. Phase 3 records request time in a per-core HDR histogram and exposes it as a Prometheus scrape. The layouts below do not move.

Host this was measured on, and that the defaults match:

| | |
| --- | --- |
| CPU | Intel Core i7-8650U @ 1.90 GHz, Kaby Lake-R, x86_64 |
| SMT | 4 physical cores, 8 logical. Pairs `(0,1) (2,3) (4,5) (6,7)` |
| NUMA | one node, cpus 0–7 |
| Caches | L1d 32 KiB 8-way, L2 256 KiB, L3 8 MiB, coherency line 64 B |
| SIMD | AVX2, FMA, BMI1/2, SSE4.2. No AVX-512. `-march=native` does not emit AVX-512 |
| Pages | 4096 B. `HugePages_Total` is 0, so the allocators do not request huge pages |
| Toolchain | g++ 13.3.0, CMake, liburing 2.5 linked when pkg-config finds it |
| Kernel | 6.6.87 WSL2. `io_uring` symbols are in the kernel image; Phase 1 still has to probe `io_uring_queue_init` and fall back to epoll |

Guest `pthread_setaffinity_np` works here. The hypervisor can still move the VM's vCPUs, so a latency number taken on this box is a development number, not the launch `p99.9` record.

## What is locked

- One reactor thread per physical core. `load_topology()` returns the lowest logical CPU of each core. On this host that is `0, 2, 4, 6`. Two hot threads on an SMT pair share a 32 KiB L1d and the same execution ports; that is a tail-latency failure, not extra capacity.
- The SPSC index blocks are 128 bytes apart, not 64. Kaby Lake's adjacent-line prefetcher completes 128-byte aligned pairs, so `alignas(64)` between the producer and consumer indices still false-shares.
- Index atomics are `std::uint64_t` and `is_always_lock_free`. The protocol is release on the publisher and acquire on the observer. Nothing on this path uses `memory_order_seq_cst`. On x86 TSO a seq_cst store is an `mfence`, and the ring does not need one.
- The ring carries a 32-byte `Event`. The bytes it names live in the owning core's arena. A 512-deep ring is 16 KiB, half of L1d. The other half is the reactor's own hot state.
- A slot array larger than L2 (default 256 KiB, `-DKESTREL_L2_BYTES=` to override) is a compile error. A ring that misses L2 is the `p99.9` cliff.
- Checksums are CRC-32C (Castagnoli), the polynomial of the SSE4.2 `crc32` instruction. ISO-HDLC CRC-32 is a different polynomial and has no instruction on this CPU.
- Core-local structures are single-threaded. Cross-core handoff is an SPSC ring per ordered pair, not a mutex queue and not an MPSC. A core that hears from N peers polls N inbound rings.

## SPSC ring

`kestrel::SpscRing<T, Capacity>` in `include/kestrel/spsc_ring.hpp`.

`Capacity` is a power of two, at least 2, at most 2^32. `T` is nothrow move-constructible, nothrow move-assignable, and nothrow destructible. The object is stationary: copy and move are deleted, because the other thread holds the address.

```
offset 0     ProducerLine          128 bytes, 128-aligned
             +0   atomic<uint64> write_pos     producer store-release
                                               consumer load-acquire
             +8   uint64 cached_read           producer only
offset 128   ConsumerLine          128 bytes
             +0   atomic<uint64> read_pos      consumer store-release
                                               producer load-acquire
             +8   uint64 cached_write          consumer only
offset 256   Slot[Capacity]        packed at sizeof(T)
                                   first slot aligned to max(64, alignof(T))
```

Slot index is `pos & (Capacity - 1)`. Positions are monotonic `uint64` counters, not masked in the atomic. Unsigned distance stays `<= Capacity`, including across wrap, which at 1e9 ops/sec is about 584 years out.

Fast path, ring not full and not empty:

1. Relaxed load of this thread's own index. That line is already in L1.
2. Compare against the cached remote index. No coherency traffic.
3. Move the element into or out of the slot.
4. One release store of the new index.

The remote atomic is loaded only when the cached index says the ring might be full or empty. That load is an L3 hop on this die and is the tail contributor. `try_push_n` / `try_pop_n` construct or consume a prefix and publish once, so a burst pays for the index line once. The slot bytes themselves are true sharing (producer writes, consumer reads) and cannot be removed.

`try_push` reserves the slot before it moves the source. A `false` return leaves the source intact. This is the bug to not reintroduce: a by-value push that moves before the full-check destroys the element on the failure path.

### Failure modes

| Condition | Result | What the caller does |
| --- | --- | --- |
| Full | `try_push` returns false, source unchanged | Stop ingesting. Do not drop the event. Do not block inside the ring |
| Empty | `try_pop` returns false, destination unchanged | Park. Phase 1 parks on io_uring or epoll, not in the ring |
| Bounded spin | `push_bounded(ring, value, pauses)` | `pauses` is the number of `pause` instructions, not an infinite retry. Budget 0 is one try. On false, stop reading the socket |
| Second producer or second consumer | Debug builds abort. Release builds are a data race | One ring, one direction, two threads |
| Destroy while a thread is inside | Undefined | Join both sides first. The destructor then destroys leftovers in order |
| `size_approx()` | Exact only when quiescent. May exceed capacity under race | Metrics only. Never a push/pop decision |

There is no futex, no allocation, and no drop. Backpressure is a `false` return in a handful of instructions. A stalled consumer becomes a full ring, which becomes a stopped socket read, which becomes TCP window pressure. An unbounded spin in front of a stalled consumer is a `p99.9` outage that looks like a busy CPU.

## Arena and slab

Both are core-local. Neither takes a lock.

`Arena` is a bump pointer over one `mmap(MAP_PRIVATE|MAP_ANONYMOUS)` region, rounded up to the page size and write-prefaulted at construction. The first request must not take a minor fault; that fault is a tail spike. `reset()` rewinds the cursor and keeps the pages. `discard()` is the idle path (`MADV_DONTNEED`) and invalidates every pointer. `rewind(mark)` rolls back a request that failed mid-parse; a mark past `used()` is rejected and the cursor stays. `allocate` returns `nullptr` when the region is exhausted or the alignment is not a power of two. That `nullptr` is the same backpressure signal as a full ring.

The arena does not run destructors. A `std::string` or anything else that owns a resource does not belong in it.

`Slab` is a fixed-stride pool for objects that outlive one request (connection state). The stride is at least `sizeof(void*)` and a multiple of the caller's alignment, so a free block can hold an intrusive freelist link. Pass `kCacheLineBytes` (64) when two live objects must not share a line. Exhaustion returns `nullptr`. `deallocate(nullptr)` is a no-op. A pointer the slab did not hand out aborts in every build, because ignoring it corrupts the freelist. Debug builds also abort on double-free and on a second thread. A bad size or a failed `mmap` leaves `ok() == false` and prints the reason; that is startup failure, not a hot-path `nullptr`.

`lock_pages()` is optional `mlock`. It fails closed when `RLIMIT_MEMLOCK` refuses, and the memory stays usable. It is not on by default.

## In-process event

`kestrel::Event` in `include/kestrel/event.hpp`. 32 bytes, 32-byte aligned, so it never straddles a 64-byte line. Two events per line. Trivially copyable. Numeric values are ABI.

```
offset  0  u64 conn_id          0 means unassigned
offset  8  u32 arena_offset     0xFFFFFFFF (kNoArenaOffset) means no payload
offset 12  u32 byte_length
offset 16  u16 type             EventType, values below
offset 18  u16 flags
offset 20  u32 reserved
```

| EventType | Value |
| --- | --- |
| None | 0 |
| Accept | 1 |
| Readable | 2 |
| Writable | 3 |
| Close | 4 |
| CacheLookup | 5 |
| CacheHit | 6 |
| CacheMiss | 7 |
| UpstreamChunk | 8 |
| Shutdown | 9 |

Flags: bit 0 `kEventFlagBorrowed` (payload is not owned by this arena generation), bit 1 `kEventFlagEndOfStream`.

`ReactorRing` is `SpscRing<Event, 512>`. 512 × 32 B = 16 KiB.

## Who owns the bytes an Event names

The arena is a generation, not a heap. Phase 1 follows these rules or it will read freed request bytes:

- `mark()` is taken before a request is parsed. `rewind(mark)` is legal only when every Event carved from bytes past that mark is still unpublished, or has been dropped on this same thread before another thread observed it.
- After `try_push` returns true, the producer does not `rewind` or `reset` across that `arena_offset` until the consumer has popped the Event and finished the `byte_length` bytes.
- `reset()` is legal only when the ring is empty and no live Event names this arena.
- `kEventFlagBorrowed` means the offset belongs to a longer-lived object (a connection buffer in the slab). Rewind does not release it, and the producer does not reuse it as scratch.
- The consumer finishes or copies those bytes before it parks. It does not store the pointer into another core's arena.

## Inter-node frame

`include/kestrel/frame.hpp`. Little-endian, version 1, 32-byte header. The codec does not allocate and does not open a socket. The payload span returned by decode points into the caller's buffer.

```
offset  0  u32 magic           'KSTR' = 0x5254534B
offset  4  u16 version         1
offset  6  u16 flags           unknown bits are preserved, not rejected
offset  8  u32 header_bytes    32
offset 12  u32 payload_bytes
offset 16  u32 crc32c          CRC-32C of (header with this field zero || payload)
offset 20  u64 stream_id
offset 28  u16 type            FrameType, values below
offset 30  u16 reserved        encode writes 0; decode rejects non-zero
```

| FrameType | Value |
| --- | --- |
| Hello | 1 |
| CacheEntry | 2 |
| CacheInvalidate | 3 |
| Ping | 4 |
| PingAck | 5 |
| PingReq | 6 |
| Member | 7 |
| Fetch | 8 |
| FetchMiss | 9 |
| FetchFail | 10 |

Unknown type values decode successfully. The codec is not a dispatcher.

Partial I/O is a status, not a short read inside the function:

| Status | When |
| --- | --- |
| NeedMore | Fewer than 32 bytes, or fewer than 32 + a legal `payload_bytes` |
| BadMagic | 32 bytes present, magic is not `KSTR` |
| BadVersion | version is not 1 |
| BadLength | `header_bytes != 32`, `reserved != 0`, or `payload_bytes > 16 MiB` |
| BadCrc | the bytes are all present and the checksum does not match |
| Ok | header and payload checked |

A declared payload above 16 MiB is `BadLength` even when the buffer is short. A hostile length must not make the caller wait for it. `encode_frame` writes nothing it will not finish: a short destination or a payload that overlaps the header region returns false with the destination unchanged. A payload that already sits at `dest + 32` is not copied.

CRC-32C parameters: init `0xFFFFFFFF`, xorout `0xFFFFFFFF`, reflected polynomial `0x82F63B78`. Empty input is `0`. The check vector `"123456789"` is `0xE3069283`. On this CPU `crc32c()` uses `_mm_crc32_u64` / `u32` / `u16` / `u8`. `crc32c_software()` is the reference and the fallback when SSE4.2 is absent; the tests require them to agree.

## What phase 0 deliberately does not do

Phase 0 shipped none of the following. Phase 1 added the first three. Hugepages are still absent: this kernel has none configured, and a failed `MAP_HUGETLB` is the wrong startup surprise.

- No `io_uring`, no `epoll`, no HTTP parser. The ring's `false` return is the hook those block on.
- No HNSW and no SIMD distance kernel. AVX2 is the compile target so the distance kernel can use 256-bit loads.
- No WAL. The frame CRC is the checksum the WAL reuses.

## Phase 1 single-node engine

Each core binds the same port with `SO_REUSEPORT`, pins itself to `reactor_cpus[i]`, and owns a private HNSW plus `wal_dir/core-N.wal`. A record this process stores is copied to its other cores. The hit path does not take that hop. The default is one core. Port 0 is resolved by the first socket; the rest bind the port `getsockname` returned.

The poller is `io_uring` when `io_uring_queue_init` succeeds, otherwise level-triggered epoll. `--epoll` forces the fallback. io_uring sockets stay blocking: a non-blocking `recv` completes with `-EAGAIN` and the thread spins. epoll sockets are non-blocking and a ready fd is read until `EAGAIN`. One operation is in flight per direction. The completion already holds the bytes; the handler does not call `recv` again. Accept stops when the connection slab is empty and resumes when a connection is recycled. `SIGPIPE` is ignored and sends use `MSG_NOSIGNAL`.

Hit path: parse the request in the connection buffer, feature-hash the prompt, HNSW search, write the cached text. It does not allocate and it does not sync the WAL. A miss proxies to an IPv4 upstream configured at startup. A hostname is rejected there; the request path does not resolve DNS. The fill appends one WAL record (`msync` of the touched pages, then `fdatasync`) and inserts the vector. If the log or the index refuses the record, the client still gets the upstream response.

The embedder is a token feature hash. Identical text produces the same unit vector. Empty text is the zero vector and does not hit. Paraphrases are not near each other. The default hit threshold is 0.97 so only an identical prompt clears it. Replacing the embedder does not change the index: the index stores floats.

HNSW is single-writer and has no lock. Dimension is padded up to a multiple of 8, capped at 1024. Vectors are 32-byte aligned and the distance kernel is AVX2 `vfmadd` (or NEON, or a scalar tail). Insert L2-normalizes. The search score is the dot product, which is cosine for those vectors. `insert` returns `kNilNode` (`0xffffffff`) when the table is full. The node table, links, and vectors are reserved up front.

A WAL record is a version-1 frame with type `CacheEntry`. The payload is little-endian `u32 dimension`, `u32 text_len`, `u32 response_len`, `f32 vector[dimension]`, then the text and the response bytes. `recover` replays every good record. `NeedMore`, `BadCrc`, `BadMagic`, `BadLength`, and `BadVersion` truncate the tail to the last good offset and stop. A record whose dimension does not match the index is skipped and logged. Views returned by `read` point into the mapping and die at the next grow (`mremap` may move it) and at destruction.

Client HTTP is Content-Length only. Chunked transfer is `Bad`: the clients this gateway is built for send a length, and a chunked body is not one buffer. `POST`/`PUT`/`PATCH` without a length is `Bad`. A message that cannot fit in the 64 KiB connection buffer is `TooLarge` and the server answers 413. Views point into that buffer. "Zero-copy" here means the parser does not allocate a string for the request. `recv` still copies out of the kernel; this is not registered-buffer DMA. A cache hit is a finite JSON or SSE body with `Content-Length`, so keep-alive does not depend on closing the socket. Upstream chunked responses are forwarded raw and de-chunked only into the cache scratch. The scratch is capped; past that the response is still forwarded and is not cached.

## Phase 2 owners and a local copy

A process is a SWIM member. Cores are not. Membership is a static `--peers` list of `IPv4:PORT`. There is no multicast. The peer port is not the HTTP port. Both sides send Hello and then use the address in that Hello, so both rings name the same endpoint. TCP close drops the socket and fails that peer's circuit. It does not mark the member dead. SWIM does, if the process stays gone.

The key is FNV-1a of the exact prompt bytes, not the feature-hash vector. 128 vnodes per member, at most 32 members. `owners` returns `min(2, member count)` distinct members, walking from the first vnode at or after the key. Dead members leave the ring. Suspected members stay.

The primary owner is the only process that calls the model while it is usable. The secondary asks the primary first. If that peer is down or its circuit is open, the secondary fills. A non-owner fetches the primary, then the secondary, stores the answer, and the next identical prompt on that process is the Phase 1 hit path. A non-owner never calls the model. `FetchMiss` means ask the other owner. `FetchFail` means the fill failed. The client is answered before any peer has the record. A peer can miss until the push arrives. The process that stored the entry still `fdatasync`s it.

Inside one process the copy rides an SPSC ring of buffer indices, capacity 2, with two payload buffers and an ack ring beside it. The body does not sit in the slot array. The ring is touched on a store and on a cross-core miss, not on a hit. A copy fetched from another process is broadcast to the local cores and is not pushed back to the owner it came from.

`CacheEntry` on the peer wire uses `stream_id` 0 for a replica push and a non-zero id for the fetch it answers. The payload is the WAL body. Types 6 through 10 were appended. They are not renumbered.

The circuit breaker is per upstream, on each core, and per peer, on core 0. Five failures open it for 1000 ms. Half-open allows one trial. `allow` consumes that trial.

Cluster defaults off, so a single process behaves as Phase 1. `--peer-port` turns it on and the CLI rejects port 0. `--peers` without `--peer-port` is an error. One extra upstream fill is accepted when membership changes and the ring moves a key. There is no migration protocol.

SWIM runs on core 0, one probe at a time: a direct ping, one indirect ping, then suspected, then dead. Period, timeout, and suspect are 200 ms, 200 ms, and 1000 ms. A higher incarnation wins. A Hello whose incarnation is at or below a dead member's is taken as one past that incarnation, so a restart is not stuck.

## Phase 3 observation

Each core keeps four HDR histograms: hit, fill, copy, and error. Two significant figures, one microsecond units, up to 60 seconds. The clock is `CLOCK_MONOTONIC`. The sample is server time from the chat request being recognized to the response being queued, or to the upstream message being finished. It is not client RTT. A local hit is `hit`. An owner fill of a real client is `fill`. A copy fetched from another process and served here is `copy`. A generated 4xx or 5xx after that clock started is `error`. Health, metrics, and debug are not samples. The synthetic connection an owner opens to its own upstream is not a sample; the client that waited on it is.

The histogram is private to the reactor thread. `record` is one plain increment and a dirty flag. At the start of the next loop, if the flag is set, the core publishes a seqlock copy: an odd sequence, an acquire-release fence, relaxed stores of the counters, then a release store of the even sequence. `GET /metrics` waits out an odd sequence, copies every kind, and checks the sequence again after an acquire fence. It merges the cores that stayed stable. A core that stays odd across the wait is omitted for that scrape. The text is a classic Prometheus histogram, coarser than the HDR. A bucket is added to a `le` only when the whole HDR range sits at or below that bound, so a sample is never reported as smaller than it was. The sixteen most recent samples on a core are a flight recorder. They are printed on the debug page and are not Prometheus labels.

`GET /metrics` is always on. `GET /debug` and `POST /chaos` exist only with `--chaos`. The page has no script. The body is one of `upstream=drop`, `upstream=ok`, `peers=drop`, `peers=ok`. Dropping the upstream fails the fill the same way an open circuit does, and it does not consume the half-open trial. Dropping peers makes every peer unusable, so a secondary owner may fill. Cluster behavior is otherwise unchanged. The feature hash is unchanged. This tree still publishes no latency number.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "kestrel/server.hpp"

#include "kestrel/circuit.hpp"
#include "kestrel/cluster_wire.hpp"
#include "kestrel/core_mail.hpp"
#include "kestrel/cpu.hpp"
#include "kestrel/feature_hash.hpp"
#include "kestrel/frame.hpp"
#include "kestrel/hash_ring.hpp"
#include "kestrel/hnsw.hpp"
#include "kestrel/http.hpp"
#include "kestrel/metrics.hpp"
#include "kestrel/simd.hpp"
#include "kestrel/swim.hpp"
#include "kestrel/wal.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <thread>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef KESTREL_HAS_LIBURING
#include <liburing.h>
#endif

namespace kestrel {

struct Server::Impl {
  ServerConfig config{};
  char bind_host[64]{};
  char wal_dir[256]{};
  char upstream_host[64]{};
  char advertise_host[64]{};
  char peers_text[1200]{};
  sockaddr_in upstream_addr{};
  int listen_fds[64]{};
  int wake_fd[64]{};
  int cpus[64]{};
  int core_count = 0;
  int thread_count = 0;
  std::uint16_t port = 0;
  std::uint16_t peer_port = 0;
  std::uint32_t self_addr = 0;
  int peer_listen = -1;
  bool use_uring = false;
  bool started = false;
  std::atomic<bool> stop{false};
  std::atomic<bool> failed{false};
  std::atomic<bool> fell_back{false};
  std::atomic<int> ready{0};
  std::atomic<int> members{0};
  CoreMail* mail = nullptr;
  void* cluster_node = nullptr;
  Published* published = nullptr;
  std::atomic<std::uint32_t> chaos_drop_upstream{0};
  std::atomic<std::uint32_t> chaos_drop_peers{0};
  // Musl's pthread default stack is 128 KiB. CoreStats is ~80 KiB and a scrape
  // copies two sets of histograms (~160 KiB) on this same thread.
  struct Launch {
    Server::Impl* self;
    int core_id;
  };
  Launch launch[64]{};
  pthread_t threads[64]{};
};

namespace {

constexpr std::size_t kReactorStackBytes = 8u * 1024u * 1024u;
constexpr std::size_t kBufBytes = 64u * 1024u;
constexpr std::size_t kMaxText = 16u * 1024u;
constexpr std::size_t kHeaderCap = 8u * 1024u;
constexpr std::size_t kBodyCap = 32u * 1024u;
constexpr int kMaxCores = 64;
constexpr std::uint32_t kMaxConns = 512;
constexpr int kAcceptBurst = 32;

struct Conn;

struct Op {
  enum Kind : std::uint8_t {
    Accept,
    Recv,
    Send,
    Connect,
    UpRecv,
    UpSend,
    PeerAccept,
    PeerRecv,
    PeerSend,
    PeerConnect,
    Wake
  };
  Kind kind = Accept;
  Conn* conn = nullptr;
  std::uint16_t slot = 0;
};

constexpr std::uint8_t kWatchConn = 0;
constexpr std::uint8_t kWatchListen = 1;
constexpr std::uint8_t kWatchWake = 2;
constexpr std::uint8_t kWatchPeerListen = 3;
constexpr std::uint8_t kWatchPeer = 4;

struct Watch {
  Conn* conn = nullptr;
  bool upstream = false;
  bool registered = false;
  std::uint32_t events = 0;
  std::uint8_t kind = kWatchConn;
  std::uint16_t slot = 0;
};

enum class Phase : std::uint8_t { Free, Read, Write, Connect, ProxySend, Proxy, AwaitCluster };

struct Conn {
  int fd = -1;
  int upstream = -1;
  Phase phase = Phase::Free;
  bool keep_alive = false;
  bool client_stream = false;
  bool have_prompt = false;
  bool cacheable = false;
  bool headers_done = false;
  bool upstream_done = false;
  bool chunked = false;
  bool broken = false;
  bool closing = false;
  bool recv_on = false;
  bool send_on = false;
  bool up_recv_on = false;
  bool up_send_on = false;
  bool connect_on = false;
  bool internal = false;
  bool observed = false;
  int status = 0;
  int inflight = 0;
  int next_free = -1;
  std::int64_t content_length = -1;
  std::int64_t body_got = 0;
  std::size_t in_len = 0;
  std::size_t out_len = 0;
  std::size_t out_sent = 0;
  std::size_t up_len = 0;
  std::size_t request_total = 0;
  std::size_t request_sent = 0;
  std::size_t hdr_len = 0;
  std::size_t body_len = 0;
  std::size_t prompt_len = 0;
  std::size_t model_len = 0;
  std::uint32_t chunk_need = 0;
  std::uint8_t chunk_state = 0;
  std::uint8_t size_len = 0;
  std::uint8_t trailer_run = 0;
  std::uint64_t key = 0;
  std::uint64_t start_us = 0;
  std::uint32_t epoch = 0;
  sockaddr_in up_addr{};
  Op recv_op{};
  Op send_op{};
  Op up_recv_op{};
  Op up_send_op{};
  Op connect_op{};
  Watch client_watch{};
  Watch up_watch{};
  char prompt[kMaxText]{};
  char model[64]{};
  char hdr[kHeaderCap]{};
  char body[kBodyCap]{};
  char size_line[32]{};
  std::byte* in = nullptr;
  std::byte* outb = nullptr;
  std::byte* up = nullptr;
};

struct Core;

void pump_read(Core& core, Conn& conn);

[[nodiscard]] std::uint64_t mono_ms() noexcept;
[[nodiscard]] bool store_record(Core& core, std::span<const float> vector, std::string_view text,
                               std::string_view response, bool broadcast) noexcept;
void cluster_note_stored(Core& core, std::span<const float> vector, std::string_view text,
                         std::string_view response) noexcept;
void cluster_fill_failed(Core& core, Conn& conn) noexcept;
[[nodiscard]] bool cluster_continue_miss(Core& core, Conn& conn);
void cluster_setup(Core& core) noexcept;
void cluster_on_loop(Core& core) noexcept;
void cluster_shutdown(Core& core) noexcept;
void peer_epoll(Core& core, Watch* watch, std::uint32_t events) noexcept;
#ifdef KESTREL_HAS_LIBURING
void cluster_drive_uring(Core& core) noexcept;
void cluster_on_cqe(Core& core, Op* op, int res) noexcept;
#endif

void recycle(Core& core, Conn& conn);
void rearm(Core& core, Conn& conn);
void pump_read(Core& core, Conn& conn);
void drain_upstream(Core& core, Conn& conn);
void after_client_progress(Core& core, Conn& conn);

struct Core {
  Server::Impl* impl = nullptr;
  int id = 0;
  int listen_fd = -1;
  int ep = -1;
  int free_head = -1;
  std::uint32_t conn_cap = 0;
  bool ring_ready = false;
  bool accept_on = false;
  HnswIndex index;
  Wal wal;
  FeatureHashEmbedder embed;
  Circuit upstream_circuit;
  CoreStats stats;
  Published* published = nullptr;
  Conn* conns = nullptr;
  Watch listen_watch{};
  Watch wake_watch{};
  Op accept_op{};
  sockaddr_storage accept_addr{};
  socklen_t accept_len = 0;
#ifdef KESTREL_HAS_LIBURING
  io_uring ring{};
#endif

  Core(Server::Impl* impl_in, int core_id, const HnswConfig& cfg, const char* path)
      : impl(impl_in),
        id(core_id),
        listen_fd(impl_in->listen_fds[core_id]),
        index(cfg),
        wal(path),
        embed(index.dimension()) {}

  Core(const Core&) = delete;
  Core& operator=(const Core&) = delete;

  ~Core() {
    if (conns != nullptr) {
      for (std::uint32_t i = 0; i < conn_cap; ++i) {
        if (conns[i].fd >= 0) {
          ::close(conns[i].fd);
        }
        if (conns[i].upstream >= 0) {
          ::close(conns[i].upstream);
        }
        delete[] conns[i].in;
        delete[] conns[i].outb;
        delete[] conns[i].up;
      }
      delete[] conns;
    }
#ifdef KESTREL_HAS_LIBURING
    if (ring_ready) {
      io_uring_queue_exit(&ring);
    }
#endif
    if (ep >= 0) {
      ::close(ep);
    }
    if (listen_fd >= 0) {
      ::close(listen_fd);
      listen_fd = -1;
    }
  }
};

[[nodiscard]] bool set_cloexec(int fd) noexcept {
  const int fdflags = ::fcntl(fd, F_GETFD, 0);
  return fdflags >= 0 && ::fcntl(fd, F_SETFD, fdflags | FD_CLOEXEC) == 0;
}

[[nodiscard]] bool set_nonblock_cloexec(int fd) noexcept {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
    return false;
  }
  return set_cloexec(fd);
}

[[nodiscard]] bool clear_nonblock(int fd) noexcept {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  return ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) == 0;
}

[[nodiscard]] bool set_nodelay(int fd) noexcept {
  const int one = 1;
  return ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) == 0;
}

[[nodiscard]] std::size_t out_room(const Conn& conn) noexcept {
  return kBufBytes - conn.out_len;
}

void reclaim_out(Conn& conn) noexcept {
  if (conn.send_on || conn.out_sent == 0) {
    return;
  }
  const std::size_t rest = conn.out_len - conn.out_sent;
  if (rest > 0) {
    std::memmove(conn.outb, conn.outb + conn.out_sent, rest);
  }
  conn.out_len = rest;
  conn.out_sent = 0;
}

void arm_fd(int ep, int fd, Watch& watch, std::uint32_t events) noexcept {
  if (ep < 0 || fd < 0) {
    return;
  }
  if (events == watch.events && watch.registered) {
    return;
  }
  if (events == 0) {
    if (watch.registered) {
      ::epoll_ctl(ep, EPOLL_CTL_DEL, fd, nullptr);
      watch.registered = false;
      watch.events = 0;
    }
    return;
  }
  epoll_event event{};
  event.events = events;
  event.data.ptr = &watch;
  const int op = watch.registered ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
  if (::epoll_ctl(ep, op, fd, &event) != 0) {
    return;
  }
  watch.registered = true;
  watch.events = events;
}

void disarm(Core& core, Conn& conn) noexcept {
  if (core.ep < 0) {
    return;
  }
  if (conn.fd >= 0) {
    arm_fd(core.ep, conn.fd, conn.client_watch, 0);
  }
  if (conn.upstream >= 0) {
    arm_fd(core.ep, conn.upstream, conn.up_watch, 0);
  }
}

void recycle(Core& core, Conn& conn) {
  disarm(core, conn);
  if (conn.fd >= 0) {
    ::close(conn.fd);
    conn.fd = -1;
  }
  if (conn.upstream >= 0) {
    ::close(conn.upstream);
    conn.upstream = -1;
  }
  conn.phase = Phase::Free;
  conn.keep_alive = false;
  conn.client_stream = false;
  conn.have_prompt = false;
  conn.cacheable = false;
  conn.headers_done = false;
  conn.upstream_done = false;
  conn.chunked = false;
  conn.broken = false;
  conn.closing = false;
  conn.recv_on = false;
  conn.send_on = false;
  conn.up_recv_on = false;
  conn.up_send_on = false;
  conn.connect_on = false;
  conn.internal = false;
  conn.observed = false;
  conn.key = 0;
  conn.start_us = 0;
  conn.status = 0;
  conn.content_length = -1;
  conn.body_got = 0;
  conn.in_len = 0;
  conn.out_len = 0;
  conn.out_sent = 0;
  conn.up_len = 0;
  conn.request_total = 0;
  conn.request_sent = 0;
  conn.hdr_len = 0;
  conn.body_len = 0;
  conn.prompt_len = 0;
  conn.model_len = 0;
  conn.chunk_need = 0;
  conn.chunk_state = 0;
  conn.size_len = 0;
  conn.trailer_run = 0;
  const int index = static_cast<int>(&conn - core.conns);
  conn.next_free = core.free_head;
  core.free_head = index;
}

void close_conn(Core& core, Conn& conn) {
  if (conn.closing || conn.phase == Phase::Free) {
    return;
  }
  conn.closing = true;
  conn.phase = Phase::Free;
  disarm(core, conn);
  if (conn.fd >= 0) {
    ::close(conn.fd);
    conn.fd = -1;
  }
  if (conn.upstream >= 0) {
    ::close(conn.upstream);
    conn.upstream = -1;
  }
  if (conn.inflight == 0) {
    recycle(core, conn);
  }
}

[[nodiscard]] Conn* acquire(Core& core) noexcept {
  if (core.free_head < 0) {
    return nullptr;
  }
  Conn& conn = core.conns[core.free_head];
  core.free_head = conn.next_free;
  conn.next_free = -1;
  conn.phase = Phase::Read;
  conn.closing = false;
  ++conn.epoch;
  return &conn;
}

void queue_out(Conn& conn, std::size_t n, bool keep) noexcept {
  conn.out_len = n;
  conn.out_sent = 0;
  conn.keep_alive = keep;
  conn.phase = Phase::Write;
  if (conn.in_len >= conn.request_total) {
    const std::size_t left = conn.in_len - conn.request_total;
    if (left > 0) {
      std::memmove(conn.in, conn.in + conn.request_total, left);
    }
    conn.in_len = left;
  } else {
    conn.in_len = 0;
  }
  conn.request_total = 0;
}

void observe(Core& core, Conn& conn, ObserveKind kind) noexcept {
  if (conn.internal || conn.observed || conn.start_us == 0) {
    return;
  }
  conn.observed = true;
  const std::uint64_t now = steady_us();
  core.stats.record(kind, now > conn.start_us ? now - conn.start_us : 0);
}

void send_status(Core& core, Conn& conn, int status, const char* message) {
  observe(core, conn, ObserveKind::Error);
  const std::size_t n = write_error(status, message, std::span<std::byte>(conn.outb, kBufBytes));
  if (n == 0) {
    close_conn(core, conn);
    return;
  }
  queue_out(conn, n, false);
}

void remember(Core& core, Conn& conn) noexcept {
  if (!conn.cacheable || conn.broken || conn.status != 200 || !conn.have_prompt || conn.prompt_len == 0) {
    return;
  }
  char text[kMaxText];
  std::size_t text_len = 0;
  const ExtractStatus extracted = extract_assistant(
      std::string_view(conn.body, conn.body_len), text, sizeof(text), text_len);
  if (extracted != ExtractStatus::Ok || text_len == 0) {
    return;
  }
  alignas(32) float vec[kHnswMaxDimension];
  const std::size_t dim = core.index.dimension();
  if (dim > kHnswMaxDimension) {
    return;
  }
  if (!core.embed.embed(std::string_view(conn.prompt, conn.prompt_len), std::span<float>(vec, dim))) {
    return;
  }
  if (!store_record(core, std::span<const float>(vec, dim), std::string_view(conn.prompt, conn.prompt_len),
                    std::string_view(text, text_len), true)) {
    return;
  }
  cluster_note_stored(core, std::span<const float>(vec, dim), std::string_view(conn.prompt, conn.prompt_len),
                      std::string_view(text, text_len));
}

void finish_upstream(Core& core, Conn& conn) {
  const bool stored = conn.status == 200 && !conn.broken;
  if (stored) {
    core.upstream_circuit.succeed();
  } else if (conn.upstream >= 0 || conn.broken) {
    core.upstream_circuit.fail(mono_ms());
  }
  remember(core, conn);
  if (conn.upstream >= 0) {
    ::close(conn.upstream);
    conn.upstream = -1;
  }
  if (conn.internal) {
    if (!stored) {
      cluster_fill_failed(core, conn);
    }
    recycle(core, conn);
    return;
  }
  observe(core, conn, stored ? ObserveKind::Fill : ObserveKind::Error);
  if (conn.broken || !conn.keep_alive) {
    close_conn(core, conn);
    return;
  }
  if (conn.in_len >= conn.request_total) {
    const std::size_t left = conn.in_len - conn.request_total;
    if (left > 0) {
      std::memmove(conn.in, conn.in + conn.request_total, left);
    }
    conn.in_len = left;
  } else {
    conn.in_len = 0;
  }
  conn.request_total = 0;
  conn.request_sent = 0;
  conn.out_len = 0;
  conn.out_sent = 0;
  conn.up_len = 0;
  conn.headers_done = false;
  conn.upstream_done = false;
  conn.chunked = false;
  conn.cacheable = false;
  conn.have_prompt = false;
  conn.broken = false;
  conn.hdr_len = 0;
  conn.body_len = 0;
  conn.body_got = 0;
  conn.content_length = -1;
  conn.chunk_state = 0;
  conn.chunk_need = 0;
  conn.size_len = 0;
  conn.trailer_run = 0;
  conn.status = 0;
  conn.phase = Phase::Read;
  pump_read(core, conn);
}

void stash_body(Conn& conn, const char* data, std::size_t n) noexcept {
  if (!conn.cacheable || n == 0) {
    return;
  }
  if (conn.body_len >= kBodyCap || n > kBodyCap - conn.body_len) {
    conn.cacheable = false;
    return;
  }
  std::memcpy(conn.body + conn.body_len, data, n);
  conn.body_len += n;
}

[[nodiscard]] int hex_value(char ch) noexcept {
  if (ch >= '0' && ch <= '9') {
    return ch - '0';
  }
  if (ch >= 'a' && ch <= 'f') {
    return ch - 'a' + 10;
  }
  if (ch >= 'A' && ch <= 'F') {
    return ch - 'A' + 10;
  }
  return -1;
}

// Raw chunked framing. Decoded payload is what we are willing to cache.
// The caller forwards every consumed byte, including chunk sizes.
void take_chunk(Conn& conn, const char* data, std::size_t n, std::size_t& i) noexcept {
  while (i < n && !conn.upstream_done && conn.chunk_state != 9) {
    if (conn.chunk_state == 0) {
      const char ch = data[i++];
      if (conn.size_len + 1u >= sizeof(conn.size_line)) {
        conn.chunk_state = 9;
        return;
      }
      conn.size_line[conn.size_len++] = ch;
      if (ch != '\n') {
        continue;
      }
      std::uint32_t size = 0;
      bool any = false;
      bool bad = false;
      for (std::uint8_t k = 0; k < conn.size_len; ++k) {
        const char h = conn.size_line[k];
        if (h == '\r' || h == '\n') {
          break;
        }
        if (h == ';') {
          break;
        }
        const int value = hex_value(h);
        if (value < 0 || size > 0x0fffffffu) {
          bad = true;
          break;
        }
        any = true;
        size = (size << 4u) | static_cast<std::uint32_t>(value);
      }
      conn.size_len = 0;
      if (bad || !any) {
        conn.chunk_state = 9;
        return;
      }
      if (size == 0) {
        conn.chunk_state = 3;
        conn.trailer_run = 0;
        continue;
      }
      conn.chunk_need = size;
      conn.chunk_state = 1;
      continue;
    }
    if (conn.chunk_state == 1) {
      const std::size_t avail = n - i;
      const std::size_t take = avail < conn.chunk_need ? avail : conn.chunk_need;
      stash_body(conn, data + i, take);
      i += take;
      conn.chunk_need -= static_cast<std::uint32_t>(take);
      if (conn.chunk_need == 0) {
        conn.chunk_state = 2;
        conn.size_len = 0;
      }
      if (take == avail) {
        return;
      }
      continue;
    }
    if (conn.chunk_state == 2) {
      const char ch = data[i++];
      const char expect = conn.size_len == 0 ? '\r' : '\n';
      if (ch != expect) {
        conn.chunk_state = 9;
        return;
      }
      ++conn.size_len;
      if (conn.size_len == 2) {
        conn.size_len = 0;
        conn.chunk_state = 0;
      }
      continue;
    }
    const char ch = data[i++];
    if (ch == '\r') {
      conn.trailer_run = conn.trailer_run == 2 ? 3 : 1;
    } else if (ch == '\n') {
      if (conn.trailer_run == 1) {
        conn.trailer_run = 2;
      } else if (conn.trailer_run == 3) {
        conn.upstream_done = true;
        conn.chunk_state = 4;
      } else {
        conn.trailer_run = 0;
      }
    } else {
      conn.trailer_run = 0;
    }
  }
}

void parse_up_headers(Conn& conn) noexcept {
  const char* begin = conn.hdr;
  const char* end = begin + conn.hdr_len;
  const char* line_end = nullptr;
  for (const char* p = begin; p + 1 < end; ++p) {
    if (p[0] == '\r' && p[1] == '\n') {
      line_end = p;
      break;
    }
  }
  conn.cacheable = conn.cacheable && conn.have_prompt;
  conn.content_length = -1;
  conn.chunked = false;
  if (line_end == nullptr || static_cast<std::size_t>(line_end - begin) < 12 ||
      std::memcmp(begin, "HTTP/1.", 7) != 0) {
    conn.cacheable = false;
    return;
  }
  const char* status = begin + 9;
  if (status + 3 > line_end || status[0] < '0' || status[0] > '9') {
    conn.cacheable = false;
    return;
  }
  conn.status = (status[0] - '0') * 100 + (status[1] - '0') * 10 + (status[2] - '0');
  if (conn.status != 200) {
    conn.cacheable = false;
  }
  const char* cursor = line_end + 2;
  while (cursor + 1 < end) {
    const char* nl = nullptr;
    for (const char* p = cursor; p + 1 < end; ++p) {
      if (p[0] == '\r' && p[1] == '\n') {
        nl = p;
        break;
      }
    }
    if (nl == nullptr || nl == cursor) {
      break;
    }
    const char* colon = nullptr;
    for (const char* p = cursor; p < nl; ++p) {
      if (*p == ':') {
        colon = p;
        break;
      }
    }
    if (colon != nullptr) {
      auto name_is = [&](const char* literal) noexcept {
        const std::size_t n = std::strlen(literal);
        if (static_cast<std::size_t>(colon - cursor) != n) {
          return false;
        }
        for (std::size_t k = 0; k < n; ++k) {
          unsigned char ch = static_cast<unsigned char>(cursor[k]);
          if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<unsigned char>(ch - 'A' + 'a');
          }
          if (ch != static_cast<unsigned char>(literal[k])) {
            return false;
          }
        }
        return true;
      };
      const char* value = colon + 1;
      while (value < nl && (*value == ' ' || *value == '\t')) {
        ++value;
      }
      if (name_is("content-length")) {
        std::int64_t length = 0;
        bool ok = value < nl;
        for (const char* p = value; p < nl && *p != ' ' && *p != '\t'; ++p) {
          if (*p < '0' || *p > '9') {
            ok = false;
            break;
          }
          length = length * 10 + (*p - '0');
        }
        if (ok) {
          conn.content_length = length;
        } else {
          conn.cacheable = false;
        }
      } else if (name_is("transfer-encoding")) {
        for (const char* p = value; p + 7 <= nl; ++p) {
          char tmp[7];
          for (int k = 0; k < 7; ++k) {
            unsigned char ch = static_cast<unsigned char>(p[k]);
            if (ch >= 'A' && ch <= 'Z') {
              ch = static_cast<unsigned char>(ch - 'A' + 'a');
            }
            tmp[k] = static_cast<char>(ch);
          }
          if (std::memcmp(tmp, "chunked", 7) == 0) {
            conn.chunked = true;
          }
        }
      }
    }
    cursor = nl + 2;
  }
  if (conn.chunked) {
    conn.content_length = -1;
    conn.chunk_state = 0;
  }
}

// Returns how many of the pending upstream bytes belong to this response
// and must be copied to the client. Stops at the end of the message.
std::size_t take_upstream(Conn& conn, const char* data, std::size_t n) noexcept {
  std::size_t i = 0;
  while (i < n && !conn.upstream_done) {
    if (!conn.headers_done) {
      if (conn.hdr_len + 1u >= sizeof(conn.hdr)) {
        conn.cacheable = false;
        conn.broken = true;
        conn.upstream_done = true;
        break;
      }
      conn.hdr[conn.hdr_len++] = data[i++];
      if (conn.hdr_len >= 4 && std::memcmp(conn.hdr + conn.hdr_len - 4, "\r\n\r\n", 4) == 0) {
        conn.headers_done = true;
        parse_up_headers(conn);
        if (conn.status == 100) {
          conn.headers_done = false;
          conn.hdr_len = 0;
          conn.status = 0;
          conn.chunked = false;
          conn.content_length = -1;
        } else if (!conn.chunked && conn.content_length == 0) {
          conn.upstream_done = true;
        }
      }
      continue;
    }
    if (conn.chunked) {
      take_chunk(conn, data, n, i);
      if (conn.chunk_state == 9) {
        conn.chunked = false;
        conn.cacheable = false;
        conn.content_length = -1;
        conn.chunk_state = 0;
      }
      continue;
    }
    if (conn.content_length >= 0) {
      const std::int64_t remain = conn.content_length - conn.body_got;
      if (remain <= 0) {
        conn.upstream_done = true;
        break;
      }
      std::size_t take = n - i;
      if (static_cast<std::int64_t>(take) > remain) {
        take = static_cast<std::size_t>(remain);
      }
      stash_body(conn, data + i, take);
      conn.body_got += static_cast<std::int64_t>(take);
      i += take;
      if (conn.body_got >= conn.content_length) {
        conn.upstream_done = true;
      }
      continue;
    }
    stash_body(conn, data + i, n - i);
    conn.body_got += static_cast<std::int64_t>(n - i);
    i = n;
  }
  return i;
}

void drain_upstream(Core& core, Conn& conn) {
  if (conn.phase != Phase::Proxy || conn.up == nullptr) {
    return;
  }
  reclaim_out(conn);
  std::size_t off = 0;
  while (off < conn.up_len && !conn.upstream_done) {
    const std::size_t room = out_room(conn);
    if (room == 0) {
      break;
    }
    const std::size_t budget = conn.up_len - off;
    const std::size_t window = budget < room ? budget : room;
    const std::size_t took = take_upstream(conn, reinterpret_cast<const char*>(conn.up + off), window);
    if (took == 0) {
      break;
    }
    std::memcpy(conn.outb + conn.out_len, conn.up + off, took);
    conn.out_len += took;
    off += took;
  }
  if (off > 0 && off < conn.up_len) {
    std::memmove(conn.up, conn.up + off, conn.up_len - off);
  }
  if (off > 0) {
    conn.up_len -= off;
  }
  if (conn.upstream_done) {
    conn.up_len = 0;
  }
  if (conn.upstream_done && conn.out_sent == conn.out_len) {
    finish_upstream(core, conn);
  }
}

void on_upstream_eof(Core& core, Conn& conn) {
  if (!conn.headers_done || (conn.chunked && !conn.upstream_done) ||
      (conn.content_length >= 0 && conn.body_got < conn.content_length)) {
    conn.broken = true;
    conn.cacheable = false;
  }
  conn.upstream_done = true;
  if (conn.upstream >= 0) {
    ::close(conn.upstream);
    conn.upstream = -1;
  }
  drain_upstream(core, conn);
  if (conn.phase == Phase::Proxy && conn.upstream_done && conn.out_sent == conn.out_len && conn.up_len == 0) {
    finish_upstream(core, conn);
  }
}

void begin_proxy(Core& core, Conn& conn);

void on_connected(Core& core, Conn& conn) {
  conn.connect_on = false;
  conn.phase = Phase::ProxySend;
  conn.request_sent = 0;
  rearm(core, conn);
}

void fail_connect(Core& core, Conn& conn) {
  if (conn.upstream >= 0) {
    ::close(conn.upstream);
    conn.upstream = -1;
  }
  conn.connect_on = false;
  core.upstream_circuit.fail(mono_ms());
  if (conn.internal) {
    cluster_fill_failed(core, conn);
    recycle(core, conn);
    return;
  }
  if (conn.out_len == conn.out_sent) {
    send_status(core, conn, 502, "upstream connect failed");
    rearm(core, conn);
    return;
  }
  close_conn(core, conn);
}

void pump_read(Core& core, Conn& conn) {
  if (conn.phase != Phase::Read || conn.closing) {
    return;
  }
  HttpMessage message;
  const ParseStatus status = parse_http(std::span<const std::byte>(conn.in, conn.in_len), message);
  if (status == ParseStatus::NeedMore) {
    return;
  }
  if (status == ParseStatus::TooLarge) {
    send_status(core, conn, 413, "request exceeds 64KiB");
    return;
  }
  if (status != ParseStatus::Ok) {
    send_status(core, conn, 400, "malformed http");
    return;
  }
  conn.request_total = message.total;
  conn.keep_alive = message.keep_alive;
  if (message.method == "GET" && message.target == "/metrics") {
    char body[48 * 1024];
    const std::size_t n = write_prometheus(core.impl->published, core.impl->core_count,
                                          std::span<std::byte>(reinterpret_cast<std::byte*>(body), sizeof(body)));
    char head[160];
    const int h = std::snprintf(head, sizeof(head),
                                "HTTP/1.1 200 OK\r\nContent-Type: text/plain; version=0.0.4\r\n"
                                "Content-Length: %zu\r\nConnection: %s\r\n\r\n",
                                n, conn.keep_alive ? "keep-alive" : "close");
    if (n == 0 || h <= 0 || static_cast<std::size_t>(h) + n > kBufBytes) {
      send_status(core, conn, 502, "metrics failed");
      return;
    }
    std::memcpy(conn.outb, head, static_cast<std::size_t>(h));
    std::memcpy(conn.outb + h, body, n);
    queue_out(conn, static_cast<std::size_t>(h) + n, conn.keep_alive);
    return;
  }
  if (message.method == "GET" && message.target == "/debug") {
    if (!core.impl->config.chaos) {
      send_status(core, conn, 404, "chaos is off");
      return;
    }
    char body[56 * 1024];
    const bool drop_up = core.impl->chaos_drop_upstream.load(std::memory_order_relaxed) != 0;
    const bool drop_peers = core.impl->chaos_drop_peers.load(std::memory_order_relaxed) != 0;
    const std::size_t n = write_debug(core.impl->published, core.impl->core_count, drop_up, drop_peers,
                                      std::span<std::byte>(reinterpret_cast<std::byte*>(body), sizeof(body)));
    char head[160];
    const int h = std::snprintf(head, sizeof(head),
                                "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                                "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                                n);
    if (n == 0 || h <= 0 || static_cast<std::size_t>(h) + n > kBufBytes) {
      send_status(core, conn, 502, "debug failed");
      return;
    }
    std::memcpy(conn.outb, head, static_cast<std::size_t>(h));
    std::memcpy(conn.outb + h, body, n);
    queue_out(conn, static_cast<std::size_t>(h) + n, false);
    return;
  }
  if (message.method == "POST" && message.target == "/chaos") {
    if (!core.impl->config.chaos) {
      send_status(core, conn, 404, "chaos is off");
      return;
    }
    const std::string_view body = message.body;
    if (body == "upstream=drop") {
      core.impl->chaos_drop_upstream.store(1, std::memory_order_relaxed);
    } else if (body == "upstream=ok") {
      core.impl->chaos_drop_upstream.store(0, std::memory_order_relaxed);
    } else if (body == "peers=drop") {
      core.impl->chaos_drop_peers.store(1, std::memory_order_relaxed);
    } else if (body == "peers=ok") {
      core.impl->chaos_drop_peers.store(0, std::memory_order_relaxed);
    } else {
      send_status(core, conn, 400, "unknown chaos");
      return;
    }
    const char* ok = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 3\r\n"
                     "Connection: close\r\n\r\nok\n";
    const std::size_t n = std::strlen(ok);
    std::memcpy(conn.outb, ok, n);
    queue_out(conn, n, false);
    return;
  }
  if (message.method == "GET" && (message.target == "/health" || message.target == "/healthz")) {
    const char* body = conn.keep_alive
                           ? "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 3\r\n"
                             "Connection: keep-alive\r\n\r\nok\n"
                           : "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 3\r\n"
                             "Connection: close\r\n\r\nok\n";
    const std::size_t n = std::strlen(body);
    std::memcpy(conn.outb, body, n);
    queue_out(conn, n, conn.keep_alive);
    return;
  }
  if (message.method != "POST") {
    send_status(core, conn, 400, "only POST is served");
    return;
  }
  conn.start_us = steady_us();
  ChatFields fields;
  char prompt[kMaxText];
  std::size_t prompt_len = 0;
  const ExtractStatus extracted =
      extract_json_contents(message.body, &fields, prompt, sizeof(prompt), prompt_len);
  conn.client_stream = fields.stream;
  conn.model_len = fields.model_len < sizeof(conn.model) ? fields.model_len : sizeof(conn.model) - 1;
  std::memcpy(conn.model, fields.model, conn.model_len);
  conn.model[conn.model_len] = '\0';
  conn.have_prompt = false;
  conn.cacheable = false;
  if (extracted == ExtractStatus::Ok && prompt_len > 0) {
    std::memcpy(conn.prompt, prompt, prompt_len);
    conn.prompt_len = prompt_len;
    conn.have_prompt = true;
    conn.cacheable = true;
    alignas(32) float vec[kHnswMaxDimension];
    const std::size_t dim = core.index.dimension();
    if (core.embed.embed(std::string_view(conn.prompt, conn.prompt_len), std::span<float>(vec, dim))) {
      HnswHit hit;
      const std::size_t found = core.index.search(std::span<const float>(vec, dim), std::span<HnswHit>(&hit, 1));
      if (found == 1 && hit.score >= core.impl->config.hit_threshold) {
        WalRecord record;
        if (core.wal.read(hit.payload, record)) {
          const std::string_view model = conn.model_len == 0 ? std::string_view("kestrel")
                                                            : std::string_view(conn.model, conn.model_len);
          const std::size_t n = conn.client_stream
                                    ? write_chat_sse(model, record.response, std::span<std::byte>(conn.outb, kBufBytes))
                                    : write_chat_json(model, record.response, std::span<std::byte>(conn.outb, kBufBytes));
          if (n > 0) {
            observe(core, conn, ObserveKind::Hit);
            queue_out(conn, n, conn.keep_alive);
            return;
          }
        }
      }
    }
  } else if (extracted == ExtractStatus::Bad) {
    send_status(core, conn, 400, "malformed json");
    return;
  }
  if (core.impl->config.cluster || (core.impl->mail != nullptr && core.id != 0)) {
    if (!cluster_continue_miss(core, conn)) {
      return;
    }
  }
  if (core.impl->config.upstream_port == 0) {
    send_status(core, conn, 502, "cache miss");
    return;
  }
  begin_proxy(core, conn);
}

void begin_proxy(Core& core, Conn& conn) {
  if (core.impl->config.chaos && core.impl->chaos_drop_upstream.load(std::memory_order_relaxed) != 0) {
    if (conn.internal) {
      cluster_fill_failed(core, conn);
      recycle(core, conn);
      return;
    }
    send_status(core, conn, 502, "upstream unavailable");
    return;
  }
  if (!core.upstream_circuit.allow(mono_ms())) {
    if (conn.internal) {
      cluster_fill_failed(core, conn);
      recycle(core, conn);
      return;
    }
    send_status(core, conn, 502, "upstream unavailable");
    return;
  }
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  const bool socket_ok = fd >= 0 && set_nodelay(fd) &&
                         (core.ring_ready ? set_cloexec(fd) : set_nonblock_cloexec(fd));
  if (!socket_ok) {
    if (fd >= 0) {
      ::close(fd);
    }
    send_status(core, conn, 502, "upstream socket failed");
    return;
  }
  conn.upstream = fd;
  conn.up_addr = core.impl->upstream_addr;
  conn.headers_done = false;
  conn.upstream_done = false;
  conn.chunked = false;
  conn.broken = false;
  conn.hdr_len = 0;
  conn.body_len = 0;
  conn.body_got = 0;
  conn.content_length = -1;
  conn.up_len = 0;
  conn.request_sent = 0;
  conn.chunk_state = 0;
  conn.status = 0;
#ifdef KESTREL_HAS_LIBURING
  if (core.ring_ready) {
    conn.phase = Phase::Connect;
    return;
  }
#endif
  const int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&conn.up_addr), sizeof(conn.up_addr));
  if (rc == 0) {
    on_connected(core, conn);
    return;
  }
  if (errno == EINPROGRESS) {
    conn.phase = Phase::Connect;
    return;
  }
  fail_connect(core, conn);
}

void after_client_progress(Core& core, Conn& conn) {
  reclaim_out(conn);
  if (conn.out_sent < conn.out_len) {
    return;
  }
  if (conn.phase == Phase::Write) {
    if (!conn.keep_alive) {
      close_conn(core, conn);
      return;
    }
    conn.out_len = 0;
    conn.out_sent = 0;
    conn.phase = Phase::Read;
    pump_read(core, conn);
    return;
  }
  if (conn.phase == Phase::Proxy) {
    drain_upstream(core, conn);
  }
}

void note_client_bytes(Core& core, Conn& conn, std::size_t n) {
  conn.in_len += n;
  pump_read(core, conn);
}

void note_client_sent(Core& core, Conn& conn, std::size_t n) {
  conn.out_sent += n;
  after_client_progress(core, conn);
}

void note_upstream_bytes(Core& core, Conn& conn, std::size_t n) {
  conn.up_len += n;
  drain_upstream(core, conn);
}

void note_upstream_sent(Core&, Conn& conn, std::size_t n) {
  conn.request_sent += n;
  if (conn.request_sent >= conn.request_total) {
    conn.phase = Phase::Proxy;
  }
}

[[nodiscard]] std::uint32_t client_interest(const Conn& conn) noexcept {
  std::uint32_t events = 0;
  if (conn.phase == Phase::Read && conn.in_len < kBufBytes && !conn.recv_on) {
    events |= EPOLLIN;
  }
  if (conn.out_len > conn.out_sent && !conn.send_on) {
    events |= EPOLLOUT;
  }
  return events;
}

[[nodiscard]] std::uint32_t upstream_interest(const Conn& conn) noexcept {
  std::uint32_t events = 0;
  if (conn.upstream < 0) {
    return 0;
  }
  if ((conn.phase == Phase::Connect || conn.phase == Phase::ProxySend) && !conn.connect_on && !conn.up_send_on) {
    events |= EPOLLOUT;
  }
  if (conn.phase == Phase::Proxy && !conn.upstream_done && !conn.up_recv_on && out_room(conn) > 0 &&
      conn.up_len < kBufBytes) {
    events |= EPOLLIN;
  }
  return events;
}

void rearm(Core& core, Conn& conn) {
  if (core.ring_ready || conn.closing || conn.phase == Phase::Free) {
    if (conn.phase == Phase::Free || conn.closing) {
      disarm(core, conn);
    }
    return;
  }
  arm_fd(core.ep, conn.fd, conn.client_watch, client_interest(conn));
  arm_fd(core.ep, conn.upstream, conn.up_watch, upstream_interest(conn));
}

void arm_listen(Core& core) noexcept {
  if (core.ring_ready || core.ep < 0) {
    return;
  }
  const std::uint32_t events = core.free_head >= 0 ? static_cast<std::uint32_t>(EPOLLIN) : 0u;
  arm_fd(core.ep, core.listen_fd, core.listen_watch, events);
}

void adopt(Core& core, int fd) {
  // io_uring submits the read. A non-blocking socket completes that read
  // with -EAGAIN and the reactor spins. epoll needs the opposite.
  const bool ready = core.ring_ready ? set_cloexec(fd) : set_nonblock_cloexec(fd);
  if (!ready || !set_nodelay(fd)) {
    ::close(fd);
    return;
  }
  Conn* conn = acquire(core);
  if (conn == nullptr) {
    ::close(fd);
    return;
  }
  conn->fd = fd;
  conn->phase = Phase::Read;
  rearm(core, *conn);
}

void accept_ready(Core& core) {
  for (int n = 0; n < kAcceptBurst; ++n) {
    if (core.free_head < 0) {
      break;
    }
    const int fd = ::accept(core.listen_fd, nullptr, nullptr);
    if (fd < 0) {
      if (errno == EINTR || errno == ECONNABORTED) {
        continue;
      }
      break;
    }
    adopt(core, fd);
  }
  arm_listen(core);
}

void client_readable(Core& core, Conn& conn) {
  while (conn.phase == Phase::Read && conn.fd >= 0 && conn.in_len < kBufBytes) {
    const ssize_t n = ::recv(conn.fd, conn.in + conn.in_len, kBufBytes - conn.in_len, 0);
    if (n > 0) {
      note_client_bytes(core, conn, static_cast<std::size_t>(n));
      continue;
    }
    if (n == 0) {
      close_conn(core, conn);
      return;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      break;
    }
    close_conn(core, conn);
    return;
  }
}

void client_writable(Core& core, Conn& conn) {
  while (conn.fd >= 0 && conn.out_sent < conn.out_len) {
    const ssize_t n = ::send(conn.fd, conn.outb + conn.out_sent, conn.out_len - conn.out_sent, MSG_NOSIGNAL);
    if (n > 0) {
      note_client_sent(core, conn, static_cast<std::size_t>(n));
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      break;
    }
    close_conn(core, conn);
    return;
  }
}

void upstream_writable(Core& core, Conn& conn) {
  if (conn.phase == Phase::Connect) {
    int err = 0;
    socklen_t len = sizeof(err);
    if (::getsockopt(conn.upstream, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
      fail_connect(core, conn);
      return;
    }
    on_connected(core, conn);
  }
  while (conn.phase == Phase::ProxySend && conn.upstream >= 0 && conn.request_sent < conn.request_total) {
    const ssize_t n = ::send(conn.upstream, conn.in + conn.request_sent, conn.request_total - conn.request_sent,
                             MSG_NOSIGNAL);
    if (n > 0) {
      note_upstream_sent(core, conn, static_cast<std::size_t>(n));
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      break;
    }
    conn.broken = true;
    conn.cacheable = false;
    on_upstream_eof(core, conn);
    return;
  }
}

void upstream_readable(Core& core, Conn& conn) {
  while (conn.phase == Phase::Proxy && conn.upstream >= 0 && !conn.upstream_done && out_room(conn) > 0 &&
         conn.up_len < kBufBytes) {
    const ssize_t n = ::recv(conn.upstream, conn.up + conn.up_len, kBufBytes - conn.up_len, 0);
    if (n > 0) {
      note_upstream_bytes(core, conn, static_cast<std::size_t>(n));
      continue;
    }
    if (n == 0) {
      on_upstream_eof(core, conn);
      return;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      break;
    }
    conn.broken = true;
    conn.cacheable = false;
    on_upstream_eof(core, conn);
    return;
  }
}

void handle_epoll(Core& core, const epoll_event& event) {
  auto* watch = static_cast<Watch*>(event.data.ptr);
  if (watch == nullptr) {
    return;
  }
  if (watch->kind == kWatchWake) {
    cluster_on_loop(core);
    return;
  }
  if (watch->kind == kWatchPeerListen || watch->kind == kWatchPeer) {
    cluster_on_loop(core);
    peer_epoll(core, watch, event.events);
    return;
  }
  if (watch->conn == nullptr) {
    if (event.events & (EPOLLERR | EPOLLHUP)) {
      return;
    }
    accept_ready(core);
    return;
  }
  Conn& conn = *watch->conn;
  if (conn.closing || conn.phase == Phase::Free) {
    return;
  }
  if (event.events & EPOLLERR) {
    if (watch->upstream && conn.phase == Phase::Connect) {
      upstream_writable(core, conn);
      rearm(core, conn);
      return;
    }
    close_conn(core, conn);
    arm_listen(core);
    return;
  }
  if (watch->upstream) {
    if (event.events & EPOLLOUT) {
      upstream_writable(core, conn);
    }
    if (conn.phase != Phase::Free && (event.events & (EPOLLIN | EPOLLHUP))) {
      upstream_readable(core, conn);
    }
  } else {
    if (event.events & EPOLLIN) {
      client_readable(core, conn);
    }
    if (conn.phase != Phase::Free && (event.events & EPOLLOUT)) {
      client_writable(core, conn);
    }
    if (conn.phase != Phase::Free && (event.events & EPOLLHUP) && conn.phase != Phase::Write &&
        conn.phase != Phase::Proxy && conn.phase != Phase::ProxySend) {
      close_conn(core, conn);
    }
  }
  if (conn.phase != Phase::Free && !conn.closing) {
    rearm(core, conn);
  }
  arm_listen(core);
}

void run_epoll(Core& core) {
  core.ep = ::epoll_create1(EPOLL_CLOEXEC);
  if (core.ep < 0) {
    std::fprintf(stderr, "kestrel: epoll_create1 failed: %s\n", std::strerror(errno));
    core.impl->failed.store(true, std::memory_order_release);
    core.impl->ready.fetch_add(1, std::memory_order_release);
    return;
  }
  core.listen_watch.conn = nullptr;
  core.listen_watch.kind = kWatchListen;
  arm_listen(core);
  cluster_setup(core);
  core.impl->ready.fetch_add(1, std::memory_order_release);
  epoll_event events[64];
  while (!core.impl->stop.load(std::memory_order_acquire)) {
    cluster_on_loop(core);
    const int n = ::epoll_wait(core.ep, events, 64, 100);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    for (int i = 0; i < n; ++i) {
      handle_epoll(core, events[i]);
    }
  }
  cluster_shutdown(core);
}

#ifdef KESTREL_HAS_LIBURING
[[nodiscard]] io_uring_sqe* next_sqe(Core& core) noexcept {
  io_uring_sqe* sqe = io_uring_get_sqe(&core.ring);
  if (sqe == nullptr) {
    io_uring_submit(&core.ring);
    sqe = io_uring_get_sqe(&core.ring);
  }
  return sqe;
}

void submit_accept(Core& core) noexcept {
  if (core.accept_on || core.free_head < 0 || core.impl->stop.load(std::memory_order_relaxed)) {
    return;
  }
  io_uring_sqe* sqe = next_sqe(core);
  if (sqe == nullptr) {
    return;
  }
  core.accept_len = sizeof(core.accept_addr);
  io_uring_prep_accept(sqe, core.listen_fd, reinterpret_cast<sockaddr*>(&core.accept_addr), &core.accept_len,
                       SOCK_CLOEXEC);
  io_uring_sqe_set_data(sqe, &core.accept_op);
  core.accept_on = true;
}

void submit_conn(Core& core, Conn& conn) noexcept {
  if (conn.closing || conn.phase == Phase::Free) {
    return;
  }
  if (conn.phase == Phase::Read && !conn.recv_on && conn.in_len < kBufBytes) {
    io_uring_sqe* sqe = next_sqe(core);
    if (sqe != nullptr) {
      io_uring_prep_recv(sqe, conn.fd, conn.in + conn.in_len, kBufBytes - conn.in_len, 0);
      io_uring_sqe_set_data(sqe, &conn.recv_op);
      conn.recv_on = true;
      ++conn.inflight;
    }
  }
  if (conn.out_len > conn.out_sent && !conn.send_on && conn.fd >= 0) {
    io_uring_sqe* sqe = next_sqe(core);
    if (sqe != nullptr) {
      io_uring_prep_send(sqe, conn.fd, conn.outb + conn.out_sent, conn.out_len - conn.out_sent, MSG_NOSIGNAL);
      io_uring_sqe_set_data(sqe, &conn.send_op);
      conn.send_on = true;
      ++conn.inflight;
    }
  }
  if (conn.phase == Phase::Connect && !conn.connect_on && conn.upstream >= 0) {
    io_uring_sqe* sqe = next_sqe(core);
    if (sqe != nullptr) {
      io_uring_prep_connect(sqe, conn.upstream, reinterpret_cast<sockaddr*>(&conn.up_addr), sizeof(conn.up_addr));
      io_uring_sqe_set_data(sqe, &conn.connect_op);
      conn.connect_on = true;
      ++conn.inflight;
    }
  }
  if (conn.phase == Phase::ProxySend && !conn.up_send_on && conn.upstream >= 0 &&
      conn.request_sent < conn.request_total) {
    io_uring_sqe* sqe = next_sqe(core);
    if (sqe != nullptr) {
      io_uring_prep_send(sqe, conn.upstream, conn.in + conn.request_sent, conn.request_total - conn.request_sent,
                         MSG_NOSIGNAL);
      io_uring_sqe_set_data(sqe, &conn.up_send_op);
      conn.up_send_on = true;
      ++conn.inflight;
    }
  }
  if (conn.phase == Phase::Proxy && !conn.up_recv_on && !conn.upstream_done && conn.upstream >= 0 &&
      out_room(conn) > 0 && conn.up_len < kBufBytes) {
    io_uring_sqe* sqe = next_sqe(core);
    if (sqe != nullptr) {
      io_uring_prep_recv(sqe, conn.upstream, conn.up + conn.up_len, kBufBytes - conn.up_len, 0);
      io_uring_sqe_set_data(sqe, &conn.up_recv_op);
      conn.up_recv_on = true;
      ++conn.inflight;
    }
  }
}

void drive_uring(Core& core) noexcept {
  submit_accept(core);
  cluster_drive_uring(core);
  for (std::uint32_t i = 0; i < core.conn_cap; ++i) {
    if (core.conns[i].phase != Phase::Free && !core.conns[i].closing) {
      submit_conn(core, core.conns[i]);
    }
  }
}

void finish_op(Core& core, Conn& conn) {
  if (conn.closing && conn.inflight == 0) {
    recycle(core, conn);
  }
}

void on_cqe(Core& core, io_uring_cqe* cqe) {
  auto* op = static_cast<Op*>(io_uring_cqe_get_data(cqe));
  if (op == nullptr) {
    return;
  }
  const int res = cqe->res;
  if (op->kind == Op::Wake || op->kind == Op::PeerAccept || op->kind == Op::PeerRecv || op->kind == Op::PeerSend ||
      op->kind == Op::PeerConnect) {
    cluster_on_cqe(core, op, res);
    return;
  }
  if (op->kind == Op::Accept) {
    core.accept_on = false;
    if (res >= 0) {
      adopt(core, res);
    }
    return;
  }
  if (op->conn == nullptr) {
    return;
  }
  Conn& conn = *op->conn;
  if (conn.inflight > 0) {
    --conn.inflight;
  }
  if (op->kind == Op::Recv) {
    conn.recv_on = false;
  } else if (op->kind == Op::Send) {
    conn.send_on = false;
  } else if (op->kind == Op::Connect) {
    conn.connect_on = false;
  } else if (op->kind == Op::UpRecv) {
    conn.up_recv_on = false;
  } else if (op->kind == Op::UpSend) {
    conn.up_send_on = false;
  }
  if (conn.closing) {
    finish_op(core, conn);
    return;
  }
  if (op->kind == Op::Recv) {
    if (res > 0) {
      note_client_bytes(core, conn, static_cast<std::size_t>(res));
    } else if (res == 0 || (res < 0 && res != -EAGAIN && res != -EINTR)) {
      close_conn(core, conn);
    }
  } else if (op->kind == Op::Send) {
    if (res > 0) {
      note_client_sent(core, conn, static_cast<std::size_t>(res));
    } else if (res < 0 && res != -EAGAIN && res != -EINTR) {
      close_conn(core, conn);
    }
  } else if (op->kind == Op::Connect) {
    if (res == 0) {
      on_connected(core, conn);
    } else {
      fail_connect(core, conn);
    }
  } else if (op->kind == Op::UpSend) {
    if (res > 0) {
      note_upstream_sent(core, conn, static_cast<std::size_t>(res));
    } else if (res < 0 && res != -EAGAIN && res != -EINTR) {
      conn.broken = true;
      conn.cacheable = false;
      on_upstream_eof(core, conn);
    }
  } else if (op->kind == Op::UpRecv) {
    if (res > 0) {
      note_upstream_bytes(core, conn, static_cast<std::size_t>(res));
    } else if (res == 0 || (res < 0 && res != -EAGAIN && res != -EINTR)) {
      conn.broken = res < 0;
      if (res < 0) {
        conn.cacheable = false;
      }
      on_upstream_eof(core, conn);
    }
  }
}

void run_uring(Core& core) {
  if (io_uring_queue_init(512, &core.ring, 0) != 0) {
    core.impl->fell_back.store(true, std::memory_order_release);
    run_epoll(core);
    return;
  }
  core.ring_ready = true;
  core.accept_op.kind = Op::Accept;
  core.accept_op.conn = nullptr;
  cluster_setup(core);
  core.impl->ready.fetch_add(1, std::memory_order_release);
  while (!core.impl->stop.load(std::memory_order_acquire)) {
    cluster_on_loop(core);
    drive_uring(core);
    io_uring_submit(&core.ring);
    io_uring_cqe* cqe = nullptr;
    __kernel_timespec ts{};
    ts.tv_sec = 0;
    ts.tv_nsec = 100000000L;
    const int rc = io_uring_wait_cqe_timeout(&core.ring, &cqe, &ts);
    // liburing reports the wait timeout as -ETIME. Some kernels surface
    // -ETIMEDOUT for the same wait. Either one means "loop again".
    if (rc < 0 && rc != -ETIME && rc != -ETIMEDOUT && rc != -EINTR) {
      std::fprintf(stderr, "kestrel: io_uring wait failed: %d\n", rc);
      break;
    }
    unsigned head = 0;
    unsigned done = 0;
    io_uring_for_each_cqe(&core.ring, head, cqe) {
      on_cqe(core, cqe);
      ++done;
    }
    if (done > 0) {
      io_uring_cq_advance(&core.ring, done);
    }
  }
  cluster_shutdown(core);
}
#endif

[[nodiscard]] bool alloc_conns(Core& core, std::uint32_t count) {
  core.conns = new (std::nothrow) Conn[count];
  if (core.conns == nullptr) {
    return false;
  }
  core.conn_cap = count;
  for (std::uint32_t i = 0; i < count; ++i) {
    Conn& conn = core.conns[i];
    conn.in = new (std::nothrow) std::byte[kBufBytes];
    conn.outb = new (std::nothrow) std::byte[kBufBytes];
    conn.up = new (std::nothrow) std::byte[kBufBytes];
    if (conn.in == nullptr || conn.outb == nullptr || conn.up == nullptr) {
      return false;
    }
    conn.recv_op.kind = Op::Recv;
    conn.recv_op.conn = &conn;
    conn.send_op.kind = Op::Send;
    conn.send_op.conn = &conn;
    conn.connect_op.kind = Op::Connect;
    conn.connect_op.conn = &conn;
    conn.up_recv_op.kind = Op::UpRecv;
    conn.up_recv_op.conn = &conn;
    conn.up_send_op.kind = Op::UpSend;
    conn.up_send_op.conn = &conn;
    conn.client_watch.conn = &conn;
    conn.up_watch.conn = &conn;
    conn.up_watch.upstream = true;
    conn.next_free = static_cast<int>(i + 1u);
    conn.phase = Phase::Free;
  }
  core.conns[count - 1u].next_free = -1;
  core.free_head = 0;
  return true;
}

void recover_wal(Core& core) noexcept {
  if (!core.wal.ok() || !core.index.ok()) {
    return;
  }
  (void)core.wal.recover([&core](const WalRecord& record) noexcept {
    if (record.vector.size() != core.index.dimension()) {
      std::fprintf(stderr, "kestrel: core %d skipped a WAL record of dimension %zu\n", core.id,
                   record.vector.size());
      return true;
    }
    if (core.index.insert(record.vector, record.payload_offset) == kNilNode) {
      std::fprintf(stderr, "kestrel: core %d index is full during recovery\n", core.id);
      return false;
    }
    return true;
  });
}

void core_main(Server::Impl* impl, int core_id) {
  if (!pin_thread_to(impl->cpus[core_id])) {
    std::fprintf(stderr, "kestrel: core %d did not pin to cpu %d\n", core_id, impl->cpus[core_id]);
  }
  char path[512];
  const int wrote = std::snprintf(path, sizeof(path), "%s/core-%d.wal", impl->wal_dir, core_id);
  if (wrote <= 0 || static_cast<std::size_t>(wrote) >= sizeof(path)) {
    ::close(impl->listen_fds[core_id]);
    impl->listen_fds[core_id] = -1;
    impl->failed.store(true, std::memory_order_release);
    impl->ready.fetch_add(1, std::memory_order_release);
    return;
  }
  HnswConfig cfg;
  cfg.dimension = impl->config.dimension;
  cfg.max_nodes = impl->config.max_nodes == 0 ? 10000u : impl->config.max_nodes;
  cfg.ef_construction = 64;
  cfg.ef_search = 64;
  Core core(impl, core_id, cfg, path);
  if (impl->published != nullptr) {
    core.published = &impl->published[core_id];
  }
  std::uint32_t conns = impl->config.max_connections == 0 ? 128u : impl->config.max_connections;
  if (conns > kMaxConns) {
    conns = kMaxConns;
  }
  if (!core.index.ok() || !core.wal.ok() || !alloc_conns(core, conns)) {
    impl->failed.store(true, std::memory_order_release);
    impl->ready.fetch_add(1, std::memory_order_release);
    return;
  }
  recover_wal(core);
#ifdef KESTREL_HAS_LIBURING
  if (impl->use_uring) {
    run_uring(core);
    return;
  }
#endif
  run_epoll(core);
}

void* core_launch(void* raw) noexcept {
  auto* launch = static_cast<Server::Impl::Launch*>(raw);
  core_main(launch->self, launch->core_id);
  return nullptr;
}

#include "server_cluster.inc"

}  // namespace

namespace {

[[nodiscard]] bool copy_text(char* dest, std::size_t cap, const char* src) noexcept {
  if (src == nullptr) {
    src = "";
  }
  const std::size_t n = std::strlen(src);
  if (n + 1u > cap) {
    return false;
  }
  std::memcpy(dest, src, n + 1u);
  return true;
}

[[nodiscard]] bool prepare_listeners(Server::Impl& impl) noexcept {
  std::uint16_t port = impl.config.port;
  in_addr bind_addr{};
  if (::inet_pton(AF_INET, impl.bind_host, &bind_addr) != 1) {
    std::fprintf(stderr, "kestrel: bind host must be an IPv4 address\n");
    return false;
  }
  for (int i = 0; i < impl.core_count; ++i) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0 || !set_nonblock_cloexec(fd)) {
      if (fd >= 0) {
        ::close(fd);
      }
      return false;
    }
    const int one = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0 ||
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) != 0) {
      ::close(fd);
      return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr = bind_addr;
    addr.sin_port = htons(port);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      std::fprintf(stderr, "kestrel: bind %s:%u failed: %s\n", impl.bind_host, static_cast<unsigned>(port),
                   std::strerror(errno));
      ::close(fd);
      return false;
    }
    if (impl.config.port == 0 && i == 0) {
      sockaddr_in actual{};
      socklen_t len = sizeof(actual);
      if (::getsockname(fd, reinterpret_cast<sockaddr*>(&actual), &len) != 0) {
        ::close(fd);
        return false;
      }
      port = ntohs(actual.sin_port);
    }
    if (::listen(fd, 128) != 0) {
      ::close(fd);
      return false;
    }
    impl.listen_fds[i] = fd;
  }
  impl.port = port;
  return true;
}

[[nodiscard]] bool bind_peer(Server::Impl& impl) noexcept {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0 || !set_nonblock_cloexec(fd)) {
    if (fd >= 0) {
      ::close(fd);
    }
    return false;
  }
  const int one = 1;
  if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0) {
    ::close(fd);
    return false;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(impl.config.peer_port);
  addr.sin_addr.s_addr = htonl(impl.self_addr);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(fd, 128) != 0) {
    std::fprintf(stderr, "kestrel: peer bind %s:%u failed: %s\n", impl.advertise_host,
                 static_cast<unsigned>(impl.config.peer_port), std::strerror(errno));
    ::close(fd);
    return false;
  }
  sockaddr_in actual{};
  socklen_t len = sizeof(actual);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&actual), &len) != 0) {
    ::close(fd);
    return false;
  }
  impl.peer_listen = fd;
  impl.peer_port = ntohs(actual.sin_port);
  impl.self_addr = ntohl(actual.sin_addr.s_addr);
  return true;
}

}  // namespace

Server::Server(const ServerConfig& config) : impl_(new (std::nothrow) Impl) {
  if (impl_ == nullptr) {
    return;
  }
  impl_->config = config;
  for (int i = 0; i < kMaxCores; ++i) {
    impl_->listen_fds[i] = -1;
    impl_->wake_fd[i] = -1;
  }
  if (!copy_text(impl_->bind_host, sizeof(impl_->bind_host), config.bind_host) ||
      !copy_text(impl_->wal_dir, sizeof(impl_->wal_dir), config.wal_dir)) {
    impl_->failed.store(true, std::memory_order_relaxed);
  }
  if (config.upstream_host != nullptr && config.upstream_host[0] != '\0') {
    if (!copy_text(impl_->upstream_host, sizeof(impl_->upstream_host), config.upstream_host) ||
        ::inet_pton(AF_INET, impl_->upstream_host, &impl_->upstream_addr.sin_addr) != 1 ||
        config.upstream_port == 0) {
      std::fprintf(stderr, "kestrel: upstream must be IPv4 host and port\n");
      impl_->failed.store(true, std::memory_order_relaxed);
    } else {
      impl_->upstream_addr.sin_family = AF_INET;
      impl_->upstream_addr.sin_port = htons(config.upstream_port);
      impl_->config.upstream_port = config.upstream_port;
    }
  }
  if (config.cluster) {
    const char* advertise = config.advertise_host;
    if (advertise == nullptr || advertise[0] == '\0') {
      advertise = std::strcmp(impl_->bind_host, "0.0.0.0") == 0 ? "127.0.0.1" : impl_->bind_host;
    }
    in_addr addr{};
    if (!copy_text(impl_->advertise_host, sizeof(impl_->advertise_host), advertise) ||
        ::inet_pton(AF_INET, impl_->advertise_host, &addr) != 1) {
      std::fprintf(stderr, "kestrel: --advertise must be an IPv4 address\n");
      impl_->failed.store(true, std::memory_order_relaxed);
    } else {
      impl_->self_addr = ntohl(addr.s_addr);
    }
    if (config.peers != nullptr && config.peers[0] != '\0' &&
        !copy_text(impl_->peers_text, sizeof(impl_->peers_text), config.peers)) {
      std::fprintf(stderr, "kestrel: --peers is too long\n");
      impl_->failed.store(true, std::memory_order_relaxed);
    }
  }
}

Server::~Server() {
  if (impl_ == nullptr) {
    return;
  }
  stop();
  delete impl_;
  impl_ = nullptr;
}

bool Server::start() {
  if (impl_ == nullptr || impl_->started || impl_->failed.load(std::memory_order_relaxed)) {
    return false;
  }
  ::signal(SIGPIPE, SIG_IGN);
  if (impl_->config.dimension == 0 || impl_->config.dimension > kHnswMaxDimension ||
      !(impl_->config.hit_threshold > 0.f && impl_->config.hit_threshold <= 1.f)) {
    return false;
  }
  if (::mkdir(impl_->wal_dir, 0755) != 0 && errno != EEXIST) {
    std::fprintf(stderr, "kestrel: mkdir %s failed: %s\n", impl_->wal_dir, std::strerror(errno));
    return false;
  }
  int cores = impl_->config.cores < 1 ? 1 : impl_->config.cores;
  if (cores > kMaxCores) {
    cores = kMaxCores;
  }
  Topology topology;
  if (load_topology(topology) && topology.physical_count > 0) {
    if (cores > topology.physical_count) {
      cores = topology.physical_count;
    }
    for (int i = 0; i < cores; ++i) {
      impl_->cpus[i] = topology.reactor_cpus[i];
    }
  } else {
    for (int i = 0; i < cores; ++i) {
      impl_->cpus[i] = i;
    }
  }
  impl_->core_count = cores;
  if (!prepare_listeners(*impl_)) {
    for (int i = 0; i < impl_->core_count; ++i) {
      if (impl_->listen_fds[i] >= 0) {
        ::close(impl_->listen_fds[i]);
        impl_->listen_fds[i] = -1;
      }
    }
    return false;
  }
  const auto release_sockets = [&]() noexcept {
    if (impl_->peer_listen >= 0) {
      ::close(impl_->peer_listen);
      impl_->peer_listen = -1;
    }
    for (int i = 0; i < impl_->core_count; ++i) {
      if (impl_->listen_fds[i] >= 0) {
        ::close(impl_->listen_fds[i]);
        impl_->listen_fds[i] = -1;
      }
      if (impl_->wake_fd[i] >= 0) {
        ::close(impl_->wake_fd[i]);
        impl_->wake_fd[i] = -1;
      }
    }
    delete impl_->mail;
    impl_->mail = nullptr;
    delete[] impl_->published;
    impl_->published = nullptr;
  };
  impl_->use_uring = false;
#ifdef KESTREL_HAS_LIBURING
  if (!impl_->config.force_epoll) {
    io_uring probe;
    if (io_uring_queue_init(8, &probe, 0) == 0) {
      io_uring_queue_exit(&probe);
      impl_->use_uring = true;
    }
  }
#else
  (void)impl_->config.force_epoll;
#endif
  if (impl_->use_uring) {
    for (int i = 0; i < impl_->core_count; ++i) {
      if (!clear_nonblock(impl_->listen_fds[i])) {
        std::fprintf(stderr, "kestrel: listen socket stayed non-blocking\n");
      }
    }
  }
  if (impl_->config.cluster && !bind_peer(*impl_)) {
    release_sockets();
    return false;
  }
  if (impl_->use_uring && impl_->peer_listen >= 0) {
    if (!clear_nonblock(impl_->peer_listen)) {
      std::fprintf(stderr, "kestrel: peer socket stayed non-blocking\n");
    }
  }
  if (impl_->core_count > 1) {
    const auto dim = static_cast<std::uint32_t>(padded_dimension(impl_->config.dimension));
    impl_->mail = new (std::nothrow) CoreMail(static_cast<std::uint32_t>(impl_->core_count), dim);
    if (impl_->mail == nullptr || !impl_->mail->ok()) {
      std::fprintf(stderr, "kestrel: core mail failed\n");
      release_sockets();
      return false;
    }
    for (int i = 0; i < impl_->core_count; ++i) {
      impl_->wake_fd[i] = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
      if (impl_->wake_fd[i] < 0) {
        std::fprintf(stderr, "kestrel: eventfd failed: %s\n", std::strerror(errno));
        release_sockets();
        return false;
      }
    }
  }
  impl_->stop.store(false, std::memory_order_relaxed);
  impl_->failed.store(false, std::memory_order_relaxed);
  impl_->fell_back.store(false, std::memory_order_relaxed);
  impl_->ready.store(0, std::memory_order_relaxed);
  impl_->published = new (std::nothrow) Published[static_cast<std::size_t>(impl_->core_count)];
  if (impl_->published == nullptr) {
    std::fprintf(stderr, "kestrel: metrics failed\n");
    release_sockets();
    return false;
  }
  pthread_attr_t attr;
  if (::pthread_attr_init(&attr) != 0) {
    std::fprintf(stderr, "kestrel: reactor stack failed\n");
    release_sockets();
    return false;
  }
  if (::pthread_attr_setstacksize(&attr, kReactorStackBytes) != 0) {
    std::fprintf(stderr, "kestrel: reactor stack failed\n");
    ::pthread_attr_destroy(&attr);
    release_sockets();
    return false;
  }
  for (int i = 0; i < impl_->core_count; ++i) {
    impl_->launch[i].self = impl_;
    impl_->launch[i].core_id = i;
    const int rc = ::pthread_create(&impl_->threads[i], &attr, core_launch, &impl_->launch[i]);
    if (rc != 0) {
      std::fprintf(stderr, "kestrel: reactor thread failed: %s\n", std::strerror(rc));
      ::pthread_attr_destroy(&attr);
      impl_->stop.store(true, std::memory_order_release);
      for (int j = 0; j < impl_->thread_count; ++j) {
        ::pthread_join(impl_->threads[j], nullptr);
      }
      impl_->thread_count = 0;
      release_sockets();
      return false;
    }
    impl_->thread_count = i + 1;
  }
  ::pthread_attr_destroy(&attr);
  while (impl_->ready.load(std::memory_order_acquire) < impl_->core_count) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (impl_->failed.load(std::memory_order_acquire)) {
    stop();
    return false;
  }
  impl_->started = true;
  return true;
}

void Server::stop() noexcept {
  if (impl_ == nullptr) {
    return;
  }
  impl_->stop.store(true, std::memory_order_release);
  for (int i = 0; i < impl_->thread_count; ++i) {
    ::pthread_join(impl_->threads[i], nullptr);
  }
  impl_->thread_count = 0;
  impl_->started = false;
  if (impl_->peer_listen >= 0) {
    ::close(impl_->peer_listen);
    impl_->peer_listen = -1;
  }
  delete impl_->mail;
  impl_->mail = nullptr;
  delete[] impl_->published;
  impl_->published = nullptr;
  for (int i = 0; i < kMaxCores; ++i) {
    if (impl_->wake_fd[i] >= 0) {
      ::close(impl_->wake_fd[i]);
      impl_->wake_fd[i] = -1;
    }
    impl_->listen_fds[i] = -1;
  }
}

std::uint16_t Server::port() const noexcept {
  return impl_ == nullptr ? 0 : impl_->port;
}

std::uint16_t Server::peer_port() const noexcept {
  return impl_ == nullptr ? 0 : impl_->peer_port;
}

int Server::members() const noexcept {
  if (impl_ == nullptr) {
    return 0;
  }
  const int n = impl_->members.load(std::memory_order_acquire);
  return n == 0 ? (impl_->config.cluster ? 1 : 0) : n;
}

const char* Server::poller_name() const noexcept {
  if (impl_ == nullptr || !impl_->use_uring || impl_->fell_back.load(std::memory_order_acquire)) {
    return "epoll";
  }
  return "io_uring";
}

}  // namespace kestrel

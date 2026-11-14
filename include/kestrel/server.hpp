#pragma once

#include <cstdint>
#include <cstddef>

namespace kestrel {

// One process, N cores. Each core binds the same port with SO_REUSEPORT,
// pins itself to its own physical core, and owns a private HNSW plus WAL.
// A hit on core 0 is invisible to core 1. That split is the Phase 2
// replication problem. The default is one core, so a single node has one cache.
//
// The hit path is: parse in place, feature-hash the prompt, HNSW search,
// write the cached text. It does not allocate and it does not sync the WAL.
// A miss optionally proxies to an IPv4 upstream. The fill path appends one
// WAL record and inserts the vector. Disk failure on that path still returns
// the upstream response.
struct ServerConfig {
  const char* bind_host = "0.0.0.0";
  std::uint16_t port = 8080;
  int cores = 1;
  std::size_t dimension = 64;
  float hit_threshold = 0.97f;
  const char* wal_dir = "kestrel-wal";
  const char* upstream_host = nullptr;
  std::uint16_t upstream_port = 0;
  bool force_epoll = false;
  std::uint32_t max_nodes = 10000;
  std::uint32_t max_connections = 128;
};

class Server {
 public:
  explicit Server(const ServerConfig& config);
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;
  ~Server();

  [[nodiscard]] bool start();
  void stop() noexcept;

  [[nodiscard]] std::uint16_t port() const noexcept;
  [[nodiscard]] const char* poller_name() const noexcept;

  // Defined in server.cpp. Public so the reactor can name it; the type is
  // incomplete here, and its members stay in that translation unit.
  struct Impl;

 private:
  Impl* impl_;
};

}  // namespace kestrel

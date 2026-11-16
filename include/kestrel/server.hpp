#pragma once

#include <cstdint>
#include <cstddef>

namespace kestrel {

// One process, N cores. Each core binds the same port with SO_REUSEPORT,
// pins itself to its own physical core, and owns a private HNSW plus WAL.
// A record stored on this process is copied to the other cores over SPSC.
// The hit path does not take that hop. Cluster mode is off unless configured:
// two hashed owners store the entry, and any other process asks an owner
// once, keeps the copy, and then hits locally. Only an owner calls the model.
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
  // Cluster is off unless set. peer_port 0 with cluster asks the kernel for a port.
  // Cores inside the process still replicate over SPSC when there is more than one.
  bool cluster = false;
  std::uint16_t peer_port = 0;
  const char* advertise_host = nullptr;
  const char* peers = nullptr;
  // Off unless set. GET /debug and POST /chaos exist only then.
  bool chaos = false;
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
  [[nodiscard]] std::uint16_t peer_port() const noexcept;
  [[nodiscard]] int members() const noexcept;
  [[nodiscard]] const char* poller_name() const noexcept;

  // Defined in server.cpp. Public so the reactor can name it; the type is
  // incomplete here, and its members stay in that translation unit.
  struct Impl;

 private:
  Impl* impl_;
};

}  // namespace kestrel

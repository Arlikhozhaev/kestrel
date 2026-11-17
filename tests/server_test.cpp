#include "kestrel/feature_hash.hpp"
#include "kestrel/hash_ring.hpp"
#include "kestrel/server.hpp"
#include "kestrel/simd.hpp"
#include "kestrel/wal.hpp"
#include "check.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

[[nodiscard]] bool write_all(int fd, const char* data, std::size_t n) {
  std::size_t off = 0;
  while (off < n) {
    const ssize_t wrote = ::send(fd, data + off, n - off, MSG_NOSIGNAL);
    if (wrote < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    off += static_cast<std::size_t>(wrote);
  }
  return true;
}

[[nodiscard]] bool read_some(int fd, std::string& out) {
  char buf[1024];
  for (;;) {
    const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      return false;
    }
    out.append(buf, static_cast<std::size_t>(n));
    return true;
  }
}

[[nodiscard]] bool read_response(int fd, std::string& response) {
  response.clear();
  while (response.find("\r\n\r\n") == std::string::npos) {
    if (!read_some(fd, response)) {
      return false;
    }
    if (response.empty()) {
      return false;
    }
  }
  const std::size_t header_end = response.find("\r\n\r\n");
  const std::string headers = response.substr(0, header_end);
  const std::size_t key = headers.find("Content-Length:");
  if (key == std::string::npos) {
    return false;
  }
  const int length = std::atoi(headers.c_str() + key + std::strlen("Content-Length:"));
  const std::size_t total = header_end + 4u + static_cast<std::size_t>(length);
  while (response.size() < total) {
    if (!read_some(fd, response)) {
      return false;
    }
  }
  response.resize(total);
  return length >= 0;
}

[[nodiscard]] int connect_port(std::uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return -1;
  }
  timeval tv{};
  tv.tv_sec = 2;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  return fd;
}

[[nodiscard]] std::string chat_request(const char* content, bool keep) {
  const std::string json =
      std::string("{\"model\":\"m\",\"messages\":[{\"role\":\"user\",\"content\":\"") + content + "\"}]}";
  std::string req = "POST /v1/chat/completions HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: ";
  req += std::to_string(json.size());
  req += keep ? "\r\nConnection: keep-alive\r\n\r\n" : "\r\nConnection: close\r\n\r\n";
  req += json;
  return req;
}

struct Upstream {
  std::atomic<int> accepts{0};
  std::atomic<bool> run{true};
  std::atomic<std::uint16_t> port{0};
  std::thread thread;

  void start() {
    thread = std::thread([this] {
      const int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
      int one = 1;
      ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = 0;
      ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
      CHECK(::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
      CHECK(::listen(listen_fd, 16) == 0);
      sockaddr_in actual{};
      socklen_t len = sizeof(actual);
      CHECK(::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&actual), &len) == 0);
      port.store(ntohs(actual.sin_port));
      timeval tv{};
      tv.tv_sec = 0;
      tv.tv_usec = 100000;
      ::setsockopt(listen_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
      while (run.load()) {
        const int fd = ::accept(listen_fd, nullptr, nullptr);
        if (fd < 0) {
          continue;
        }
        accepts.fetch_add(1);
        std::string req;
        while (req.find("\r\n\r\n") == std::string::npos) {
          if (!read_some(fd, req)) {
            break;
          }
        }
        const char* body = "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"pong\"}}]}";
        char header[256];
        const int n = std::snprintf(header, sizeof(header),
                                    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                                    "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                                    std::strlen(body));
        if (n > 0) {
          (void)write_all(fd, header, static_cast<std::size_t>(n));
          (void)write_all(fd, body, std::strlen(body));
        }
        ::close(fd);
      }
      ::close(listen_fd);
    });
    while (port.load() == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  void stop() {
    run.store(false);
    if (port.load() != 0) {
      const int fd = connect_port(port.load());
      if (fd >= 0) {
        ::close(fd);
      }
    }
    if (thread.joinable()) {
      thread.join();
    }
  }
};

void test_seed_hit(bool force_epoll) {
  char dir[] = "/tmp/kestrel-seed-XXXXXX";
  CHECK(::mkdtemp(dir) != nullptr);
  char path[256];
  std::snprintf(path, sizeof(path), "%s/core-0.wal", dir);
  {
    kestrel::FeatureHashEmbedder embed(64);
    alignas(32) float vec[64];
    CHECK(embed.embed("ping", vec));
    kestrel::Wal wal(path);
    CHECK(wal.ok());
    CHECK(wal.append(vec, "ping", "cached-pong") != UINT64_MAX);
  }
  kestrel::ServerConfig cfg;
  cfg.bind_host = "127.0.0.1";
  cfg.port = 0;
  cfg.cores = 1;
  cfg.dimension = 64;
  cfg.wal_dir = dir;
  cfg.force_epoll = force_epoll;
  cfg.max_nodes = 128;
  cfg.max_connections = 8;
  kestrel::Server server(cfg);
  CHECK(server.start());
  CHECK(server.port() != 0);
  const int fd = connect_port(server.port());
  CHECK(fd >= 0);
  const std::string req = chat_request("ping", false);
  CHECK(write_all(fd, req.data(), req.size()));
  std::string response;
  CHECK(read_response(fd, response));
  CHECK(response.find(" 200 ") != std::string::npos);
  CHECK(response.find("cached-pong") != std::string::npos);
  ::close(fd);
  server.stop();
}

void test_proxy_then_hit(bool force_epoll) {
  char dir[] = "/tmp/kestrel-proxy-XXXXXX";
  CHECK(::mkdtemp(dir) != nullptr);
  Upstream upstream;
  upstream.start();
  kestrel::ServerConfig cfg;
  cfg.bind_host = "127.0.0.1";
  cfg.port = 0;
  cfg.cores = 1;
  cfg.dimension = 64;
  cfg.wal_dir = dir;
  cfg.upstream_host = "127.0.0.1";
  cfg.upstream_port = upstream.port.load();
  cfg.force_epoll = force_epoll;
  cfg.max_nodes = 128;
  cfg.max_connections = 8;
  kestrel::Server server(cfg);
  CHECK(server.start());
  const int fd = connect_port(server.port());
  CHECK(fd >= 0);
  const std::string req = chat_request("ping", true);
  CHECK(write_all(fd, req.data(), req.size()));
  std::string first;
  CHECK(read_response(fd, first));
  CHECK(first.find("pong") != std::string::npos);
  CHECK(first.find("chatcmpl-kestrel") == std::string::npos);
  CHECK(write_all(fd, req.data(), req.size()));
  std::string second;
  CHECK(read_response(fd, second));
  CHECK(second.find("pong") != std::string::npos);
  CHECK(second.find("chatcmpl-kestrel") != std::string::npos);
  CHECK(upstream.accepts.load() == 1);
  ::close(fd);
  server.stop();
  upstream.stop();
}

[[nodiscard]] bool wait_members(kestrel::Server& server, int want) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    if (server.members() >= want) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return server.members() >= want;
}

[[nodiscard]] bool wal_has(const char* path, const char* text) {
  // Read-only. Wal::recover truncates a torn tail, and this file is still mapped by the server.
  const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  char buf[65536];
  const ssize_t n = ::read(fd, buf, sizeof(buf));
  ::close(fd);
  if (n <= 0) {
    return false;
  }
  const std::size_t needle = std::strlen(text);
  if (needle == 0 || static_cast<std::size_t>(n) < needle) {
    return false;
  }
  for (std::size_t i = 0; i + needle <= static_cast<std::size_t>(n); ++i) {
    if (std::memcmp(buf + i, text, needle) == 0) {
      return true;
    }
  }
  return false;
}

void test_cores(bool force_epoll) {
  char dir[] = "/tmp/kestrel-cores-XXXXXX";
  CHECK(mkdtemp(dir) != nullptr);
  Upstream upstream;
  upstream.start();
  kestrel::ServerConfig cfg;
  cfg.bind_host = "127.0.0.1";
  cfg.port = 0;
  cfg.cores = 2;
  cfg.wal_dir = dir;
  cfg.upstream_host = "127.0.0.1";
  cfg.upstream_port = upstream.port.load();
  cfg.force_epoll = force_epoll;
  cfg.max_nodes = 128;
  cfg.max_connections = 8;
  kestrel::Server server(cfg);
  CHECK(server.start());
  const int fd = connect_port(server.port());
  CHECK(fd >= 0);
  const std::string req = chat_request("ping", false);
  CHECK(write_all(fd, req.data(), req.size()));
  std::string response;
  CHECK(read_response(fd, response));
  CHECK(response.find("pong") != std::string::npos);
  ::close(fd);
  char path0[256];
  char path1[256];
  std::snprintf(path0, sizeof(path0), "%s/core-0.wal", dir);
  std::snprintf(path1, sizeof(path1), "%s/core-1.wal", dir);
  bool both = false;
  for (int i = 0; i < 20 && !both; ++i) {
    both = wal_has(path0, "ping") && wal_has(path1, "ping");
    if (!both) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
  CHECK(both);
  CHECK(upstream.accepts.load() == 1);
  server.stop();
  upstream.stop();
}

void test_cluster(bool force_epoll) {
  char dir_a[] = "/tmp/kestrel-ca-XXXXXX";
  char dir_b[] = "/tmp/kestrel-cb-XXXXXX";
  char dir_c[] = "/tmp/kestrel-cc-XXXXXX";
  CHECK(mkdtemp(dir_a) != nullptr);
  CHECK(mkdtemp(dir_b) != nullptr);
  CHECK(mkdtemp(dir_c) != nullptr);
  Upstream upstream;
  upstream.start();

  kestrel::ServerConfig cfg_a;
  cfg_a.bind_host = "127.0.0.1";
  cfg_a.advertise_host = "127.0.0.1";
  cfg_a.port = 0;
  cfg_a.cluster = true;
  cfg_a.peer_port = 0;
  cfg_a.cores = 1;
  cfg_a.wal_dir = dir_a;
  cfg_a.upstream_host = "127.0.0.1";
  cfg_a.upstream_port = upstream.port.load();
  cfg_a.force_epoll = force_epoll;
  cfg_a.max_nodes = 128;
  cfg_a.max_connections = 8;
  kestrel::Server server_a(cfg_a);
  CHECK(server_a.start());

  char peers_b[64];
  std::snprintf(peers_b, sizeof(peers_b), "127.0.0.1:%u", static_cast<unsigned>(server_a.peer_port()));
  kestrel::ServerConfig cfg_b = cfg_a;
  cfg_b.wal_dir = dir_b;
  cfg_b.peers = peers_b;
  kestrel::Server server_b(cfg_b);
  CHECK(server_b.start());

  char peers_c[128];
  std::snprintf(peers_c, sizeof(peers_c), "127.0.0.1:%u,127.0.0.1:%u", static_cast<unsigned>(server_a.peer_port()),
                static_cast<unsigned>(server_b.peer_port()));
  kestrel::ServerConfig cfg_c = cfg_a;
  cfg_c.wal_dir = dir_c;
  cfg_c.peers = peers_c;
  kestrel::Server server_c(cfg_c);
  CHECK(server_c.start());

  CHECK(wait_members(server_a, 3));
  CHECK(wait_members(server_b, 3));
  CHECK(wait_members(server_c, 3));

  in_addr addr{};
  CHECK(::inet_pton(AF_INET, "127.0.0.1", &addr) == 1);
  const std::uint32_t ip = ntohl(addr.s_addr);
  kestrel::MemberEndpoint members[3] = {
      {ip, server_a.peer_port()},
      {ip, server_b.peer_port()},
      {ip, server_c.peer_port()},
  };
  kestrel::HashRing ring;
  CHECK(ring.set_members(std::span<const kestrel::MemberEndpoint>(members, 3)));
  kestrel::MemberEndpoint owners[2]{};
  CHECK(ring.owners(kestrel::HashRing::key_of("ping"), owners) == 2);
  int primary = -1;
  int outsider = -1;
  for (int i = 0; i < 3; ++i) {
    if (members[i] == owners[0]) {
      primary = i;
    }
    if (!(members[i] == owners[0]) && !(members[i] == owners[1])) {
      outsider = i;
    }
  }
  CHECK(primary >= 0);
  CHECK(outsider >= 0);

  const std::uint16_t http_port[3] = {server_a.port(), server_b.port(), server_c.port()};
  const int first = connect_port(http_port[primary]);
  CHECK(first >= 0);
  const std::string req = chat_request("ping", false);
  CHECK(write_all(first, req.data(), req.size()));
  std::string body;
  CHECK(read_response(first, body));
  CHECK(body.find("pong") != std::string::npos);
  ::close(first);
  CHECK(upstream.accepts.load() == 1);

  const int second = connect_port(http_port[outsider]);
  CHECK(second >= 0);
  CHECK(write_all(second, req.data(), req.size()));
  std::string fetched;
  CHECK(read_response(second, fetched));
  CHECK(fetched.find("pong") != std::string::npos);
  CHECK(fetched.find("chatcmpl-kestrel") != std::string::npos);
  ::close(second);
  CHECK(upstream.accepts.load() == 1);

  const int third = connect_port(http_port[outsider]);
  CHECK(third >= 0);
  CHECK(write_all(third, req.data(), req.size()));
  std::string again;
  CHECK(read_response(third, again));
  CHECK(again.find("chatcmpl-kestrel") != std::string::npos);
  ::close(third);
  CHECK(upstream.accepts.load() == 1);

  server_c.stop();
  server_b.stop();
  server_a.stop();
  upstream.stop();
}

[[nodiscard]] std::string http_get(std::uint16_t port, const char* path) {
  const int fd = connect_port(port);
  CHECK(fd >= 0);
  std::string req = "GET ";
  req += path;
  req += " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
  CHECK(write_all(fd, req.data(), req.size()));
  std::string response;
  CHECK(read_response(fd, response));
  ::close(fd);
  return response;
}

void test_metrics(bool force_epoll) {
  char dir[] = "/tmp/kestrel-metrics-XXXXXX";
  CHECK(mkdtemp(dir) != nullptr);
  Upstream upstream;
  upstream.start();
  kestrel::ServerConfig cfg;
  cfg.bind_host = "127.0.0.1";
  cfg.port = 0;
  cfg.cores = 1;
  cfg.wal_dir = dir;
  cfg.upstream_host = "127.0.0.1";
  cfg.upstream_port = upstream.port.load();
  cfg.force_epoll = force_epoll;
  cfg.max_nodes = 128;
  cfg.max_connections = 8;
  kestrel::Server server(cfg);
  CHECK(server.start());
  const std::string hidden = http_get(server.port(), "/debug");
  CHECK(hidden.find("404") != std::string::npos);
  const std::string before = http_get(server.port(), "/metrics");
  CHECK(before.find("kestrel_request_duration_seconds_count{result=\"hit\"} 0\n") != std::string::npos);
  const int fd = connect_port(server.port());
  CHECK(fd >= 0);
  const std::string req = chat_request("ping", false);
  CHECK(write_all(fd, req.data(), req.size()));
  std::string response;
  CHECK(read_response(fd, response));
  CHECK(response.find("pong") != std::string::npos);
  ::close(fd);
  const std::string after = http_get(server.port(), "/metrics");
  CHECK(after.find("kestrel_request_duration_seconds_count{result=\"fill\"} 1\n") != std::string::npos);
  CHECK(upstream.accepts.load() == 1);
  const int kept = connect_port(server.port());
  CHECK(kept >= 0);
  const std::string again = chat_request("ping", true);
  CHECK(write_all(kept, again.data(), again.size()));
  std::string hit_one;
  CHECK(read_response(kept, hit_one));
  CHECK(write_all(kept, again.data(), again.size()));
  std::string hit_two;
  CHECK(read_response(kept, hit_two));
  ::close(kept);
  const std::string kept_metrics = http_get(server.port(), "/metrics");
  CHECK(kept_metrics.find("kestrel_request_duration_seconds_count{result=\"hit\"} 2\n") != std::string::npos);
  server.stop();
  upstream.stop();
}

void test_chaos(bool force_epoll) {
  char dir[] = "/tmp/kestrel-chaos-XXXXXX";
  CHECK(mkdtemp(dir) != nullptr);
  Upstream upstream;
  upstream.start();
  kestrel::ServerConfig cfg;
  cfg.bind_host = "127.0.0.1";
  cfg.port = 0;
  cfg.cores = 1;
  cfg.wal_dir = dir;
  cfg.upstream_host = "127.0.0.1";
  cfg.upstream_port = upstream.port.load();
  cfg.force_epoll = force_epoll;
  cfg.chaos = true;
  cfg.max_nodes = 128;
  cfg.max_connections = 8;
  kestrel::Server server(cfg);
  CHECK(server.start());
  const std::string off = http_get(server.port(), "/debug");
  CHECK(off.find("drop upstream") != std::string::npos);
  const int fd = connect_port(server.port());
  CHECK(fd >= 0);
  const std::string drop = "POST /chaos HTTP/1.1\r\nHost: localhost\r\nContent-Length: 13\r\nConnection: close\r\n\r\nupstream=drop";
  CHECK(write_all(fd, drop.data(), drop.size()));
  std::string dropped;
  CHECK(read_response(fd, dropped));
  CHECK(dropped.find("200") != std::string::npos);
  ::close(fd);
  const int miss = connect_port(server.port());
  CHECK(miss >= 0);
  const std::string req = chat_request("ping", false);
  CHECK(write_all(miss, req.data(), req.size()));
  std::string denied;
  CHECK(read_response(miss, denied));
  CHECK(denied.find("502") != std::string::npos);
  ::close(miss);
  CHECK(upstream.accepts.load() == 0);
  server.stop();
  upstream.stop();
}

}  // namespace

int main() {
  test_seed_hit(false);
  test_seed_hit(true);
  test_proxy_then_hit(false);
  test_proxy_then_hit(true);
  test_cores(true);
  test_cluster(true);
  test_cores(false);
  test_cluster(false);
  test_metrics(true);
  test_chaos(true);
  test_metrics(false);
  test_chaos(false);
  return kestrel::test::failures.load() == 0 ? 0 : 1;
}

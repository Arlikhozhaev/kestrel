#include "kestrel/feature_hash.hpp"
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
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
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

}  // namespace

int main() {
  test_seed_hit(false);
  test_seed_hit(true);
  test_proxy_then_hit(false);
  test_proxy_then_hit(true);
  return kestrel::test::failures.load() == 0 ? 0 : 1;
}

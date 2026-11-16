#include "kestrel/server.hpp"
#include "kestrel/version.hpp"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <signal.h>
#include <unistd.h>

namespace {

std::atomic<int> g_stop{0};

void on_signal(int) noexcept { g_stop.store(1, std::memory_order_release); }

void usage() noexcept {
  std::fputs(
      "usage: kestreld [--bind ADDR] [--port N] [--cores N] [--dim N]\n"
      "                [--threshold F] [--wal DIR] [--upstream ADDR:PORT]\n"
      "                [--peer-port N] [--advertise ADDR] [--peers ADDR:PORT,...] [--epoll] [--chaos]\n",
      stderr);
}

[[nodiscard]] bool parse_u16(const char* text, std::uint16_t& out) noexcept {
  if (text == nullptr || text[0] == '\0') {
    return false;
  }
  char* end = nullptr;
  errno = 0;
  const unsigned long value = std::strtoul(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || value > 65535ul) {
    return false;
  }
  out = static_cast<std::uint16_t>(value);
  return true;
}

[[nodiscard]] bool parse_i(const char* text, int& out) noexcept {
  if (text == nullptr || text[0] == '\0') {
    return false;
  }
  char* end = nullptr;
  errno = 0;
  const long value = std::strtol(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || value > 1000000L || value < 0) {
    return false;
  }
  out = static_cast<int>(value);
  return true;
}

[[nodiscard]] bool split_upstream(const char* text, const char*& host, std::uint16_t& port) noexcept {
  if (text == nullptr) {
    return false;
  }
  const char* colon = std::strrchr(text, ':');
  if (colon == nullptr || colon == text || colon[1] == '\0') {
    return false;
  }
  static char host_buf[64];
  const std::size_t n = static_cast<std::size_t>(colon - text);
  if (n + 1u > sizeof(host_buf)) {
    return false;
  }
  std::memcpy(host_buf, text, n);
  host_buf[n] = '\0';
  host = host_buf;
  return parse_u16(colon + 1, port);
}

}  // namespace

int main(int argc, char** argv) {
  kestrel::ServerConfig config;
  for (int i = 1; i < argc; ++i) {
    const char* arg = argv[i];
    auto need = [&](const char* flag) noexcept -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "kestrel: %s needs a value\n", flag);
        usage();
        std::exit(2);
      }
      ++i;
      return argv[i];
    };
    if (std::strcmp(arg, "--bind") == 0) {
      config.bind_host = need("--bind");
    } else if (std::strcmp(arg, "--port") == 0) {
      if (!parse_u16(need("--port"), config.port)) {
        std::fputs("kestrel: --port is not a port\n", stderr);
        return 2;
      }
    } else if (std::strcmp(arg, "--cores") == 0) {
      if (!parse_i(need("--cores"), config.cores) || config.cores < 1) {
        std::fputs("kestrel: --cores is not a positive integer\n", stderr);
        return 2;
      }
    } else if (std::strcmp(arg, "--dim") == 0) {
      int dim = 0;
      if (!parse_i(need("--dim"), dim) || dim < 1) {
        std::fputs("kestrel: --dim is not a positive integer\n", stderr);
        return 2;
      }
      config.dimension = static_cast<std::size_t>(dim);
    } else if (std::strcmp(arg, "--threshold") == 0) {
      const char* text = need("--threshold");
      char* end = nullptr;
      errno = 0;
      config.hit_threshold = std::strtof(text, &end);
      if (errno != 0 || end == text || *end != '\0') {
        std::fputs("kestrel: --threshold is not a float\n", stderr);
        return 2;
      }
    } else if (std::strcmp(arg, "--wal") == 0) {
      config.wal_dir = need("--wal");
    } else if (std::strcmp(arg, "--upstream") == 0) {
      if (!split_upstream(need("--upstream"), config.upstream_host, config.upstream_port)) {
        std::fputs("kestrel: --upstream must be IPv4:PORT\n", stderr);
        return 2;
      }
    } else if (std::strcmp(arg, "--epoll") == 0) {
      config.force_epoll = true;
    } else if (std::strcmp(arg, "--chaos") == 0) {
      config.chaos = true;
    } else if (std::strcmp(arg, "--peer-port") == 0) {
      if (!parse_u16(need("--peer-port"), config.peer_port) || config.peer_port == 0) {
        std::fputs("kestrel: --peer-port is not a port\n", stderr);
        return 2;
      }
      config.cluster = true;
    } else if (std::strcmp(arg, "--advertise") == 0) {
      config.advertise_host = need("--advertise");
    } else if (std::strcmp(arg, "--peers") == 0) {
      config.peers = need("--peers");
      config.cluster = true;
    } else if (std::strcmp(arg, "--help") == 0) {
      usage();
      return 0;
    } else {
      std::fprintf(stderr, "kestrel: unknown argument %s\n", arg);
      usage();
      return 2;
    }
  }

  if (config.peers != nullptr && !config.cluster) {
    std::fputs("kestrel: --peers needs --peer-port\n", stderr);
    return 2;
  }
  if (config.cluster && config.peer_port == 0) {
    std::fputs("kestrel: --peer-port is required\n", stderr);
    return 2;
  }

  kestrel::Server server(config);
  if (!server.start()) {
    std::fputs("kestrel: failed to start\n", stderr);
    return 1;
  }
  std::fprintf(stderr, "kestrel %d.%d.%d listening on %s:%u (%s)\n", kestrel::kVersionMajor,
               kestrel::kVersionMinor, kestrel::kVersionPatch, config.bind_host,
               static_cast<unsigned>(server.port()), server.poller_name());

  struct sigaction action {};
  action.sa_handler = on_signal;
  sigemptyset(&action.sa_mask);
  sigaction(SIGINT, &action, nullptr);
  sigaction(SIGTERM, &action, nullptr);
  while (g_stop.load(std::memory_order_acquire) == 0) {
    ::pause();
  }
  server.stop();
  return 0;
}

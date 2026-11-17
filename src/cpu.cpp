#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "kestrel/cpu.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <limits>

#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

namespace kestrel {
namespace {

bool read_file(const char* path, char* buffer, std::size_t buffer_bytes) noexcept {
  if (buffer == nullptr || buffer_bytes < 2) {
    return false;
  }
  const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  std::size_t used = 0;
  while (used + 1 < buffer_bytes) {
    const ssize_t got = ::read(fd, buffer + used, buffer_bytes - 1 - used);
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      ::close(fd);
      return false;
    }
    if (got == 0) {
      break;
    }
    used += static_cast<std::size_t>(got);
  }
  ::close(fd);
  buffer[used] = '\0';
  return used > 0;
}

bool parse_i32(const char* text, int& out) noexcept {
  if (text == nullptr) {
    return false;
  }
  errno = 0;
  char* end = nullptr;
  const long value = std::strtol(text, &end, 10);
  if (end == text || errno != 0) {
    return false;
  }
  if (value > std::numeric_limits<int>::max() || value < std::numeric_limits<int>::min()) {
    return false;
  }
  out = static_cast<int>(value);
  return true;
}

// Accepts "0-7", "0,2,4", "0-3,6", with trailing whitespace.
bool parse_cpu_list(const char* spec, int* out, int& count, int max_count) noexcept {
  count = 0;
  if (spec == nullptr || out == nullptr || max_count <= 0) {
    return false;
  }
  const char* cursor = spec;
  while (*cursor != '\0') {
    if (*cursor == ',' || *cursor == ' ' || *cursor == '\n' || *cursor == '\t') {
      ++cursor;
      continue;
    }
    errno = 0;
    char* end = nullptr;
    const long start = std::strtol(cursor, &end, 10);
    if (end == cursor || errno != 0) {
      return false;
    }
    cursor = end;
    long last = start;
    if (*cursor == '-') {
      ++cursor;
      errno = 0;
      last = std::strtol(cursor, &end, 10);
      if (end == cursor || errno != 0 || last < start) {
        return false;
      }
      cursor = end;
    }
    if (start < 0) {
      return false;
    }
    for (long cpu = start; cpu <= last; ++cpu) {
      if (cpu >= kMaxLogicalCpus || cpu > std::numeric_limits<int>::max()) {
        return false;
      }
      if (count >= max_count) {
        return false;
      }
      out[count] = static_cast<int>(cpu);
      ++count;
    }
  }
  return count > 0;
}

[[nodiscard]] bool cpu_allowed(int cpu, const cpu_set_t& mask, bool have_mask) noexcept {
  if (!have_mask) {
    return true;
  }
  if (cpu < 0 || cpu >= CPU_SETSIZE) {
    return false;
  }
  return CPU_ISSET(cpu, &mask) != 0;
}

bool read_cpu_file(int cpu, const char* leaf, char* buffer, std::size_t buffer_bytes) noexcept {
  char path[160];
  const int written = std::snprintf(path, sizeof(path),
                                    "/sys/devices/system/cpu/cpu%d/topology/%s", cpu, leaf);
  if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(path)) {
    return false;
  }
  return read_file(path, buffer, buffer_bytes);
}

}  // namespace

bool load_topology(Topology& out) noexcept {
  out = Topology{};
  char online[1024];
  if (!read_file("/sys/devices/system/cpu/online", online, sizeof(online))) {
    return false;
  }
  int online_ids[kMaxLogicalCpus];
  int online_count = 0;
  if (!parse_cpu_list(online, online_ids, online_count, kMaxLogicalCpus)) {
    return false;
  }
  cpu_set_t allowed;
  CPU_ZERO(&allowed);
  const bool have_mask = ::sched_getaffinity(0, sizeof(allowed), &allowed) == 0;

  int logical[kMaxLogicalCpus];
  int logical_count = 0;
  for (int i = 0; i < online_count; ++i) {
    if (!cpu_allowed(online_ids[i], allowed, have_mask)) {
      continue;
    }
    logical[logical_count] = online_ids[i];
    ++logical_count;
  }
  if (logical_count == 0) {
    return false;
  }

  int group_core[kMaxLogicalCpus];
  int group_primary[kMaxLogicalCpus];
  int group_count = 0;

  for (int i = 0; i < logical_count; ++i) {
    const int cpu = logical[i];
    out.logical_ids[i] = cpu;

    char text[256];
    if (!read_cpu_file(cpu, "core_id", text, sizeof(text))) {
      return false;
    }
    int core_id = -1;
    if (!parse_i32(text, core_id)) {
      return false;
    }

    int group = -1;
    for (int g = 0; g < group_count; ++g) {
      if (group_core[g] == core_id) {
        group = g;
        break;
      }
    }
    if (group < 0) {
      if (group_count >= kMaxLogicalCpus) {
        return false;
      }
      group = group_count;
      group_core[group] = core_id;
      group_primary[group] = cpu;
      ++group_count;
    } else if (cpu < group_primary[group]) {
      group_primary[group] = cpu;
    }

    if (!read_cpu_file(cpu, "thread_siblings_list", text, sizeof(text))) {
      return false;
    }
    int siblings[kMaxLogicalCpus];
    int sibling_count = 0;
    if (!parse_cpu_list(text, siblings, sibling_count, kMaxLogicalCpus)) {
      return false;
    }
    int sibling = -1;
    for (int s = 0; s < sibling_count; ++s) {
      if (siblings[s] == cpu || !cpu_allowed(siblings[s], allowed, have_mask)) {
        continue;
      }
      if (sibling < 0 || siblings[s] < sibling) {
        sibling = siblings[s];
      }
    }
    out.sibling_of[cpu] = sibling;
  }

  // Lowest logical id in each physical core, sorted, is the reactor pin set.
  for (int i = 1; i < group_count; ++i) {
    const int core = group_core[i];
    const int primary = group_primary[i];
    int slot = i;
    while (slot > 0 && group_primary[slot - 1] > primary) {
      group_primary[slot] = group_primary[slot - 1];
      group_core[slot] = group_core[slot - 1];
      --slot;
    }
    group_primary[slot] = primary;
    group_core[slot] = core;
  }

  out.logical_count = logical_count;
  out.physical_count = group_count;
  for (int g = 0; g < group_count; ++g) {
    out.reactor_cpus[g] = group_primary[g];
  }
  return true;
}

bool pin_thread_to(int cpu) noexcept {
  if (cpu < 0 || cpu >= CPU_SETSIZE) {
    return false;
  }
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  return ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set) == 0;
}

bool reset_affinity(const Topology& topology) noexcept {
  if (topology.logical_count <= 0) {
    return false;
  }
  cpu_set_t set;
  CPU_ZERO(&set);
  for (int i = 0; i < topology.logical_count; ++i) {
    const int cpu = topology.logical_ids[i];
    if (cpu < 0 || cpu >= CPU_SETSIZE) {
      return false;
    }
    CPU_SET(cpu, &set);
  }
  return ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set) == 0;
}

int current_cpu() noexcept { return ::sched_getcpu(); }

}  // namespace kestrel

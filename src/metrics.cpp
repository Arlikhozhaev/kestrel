#include "kestrel/metrics.hpp"

#include <cstring>

namespace kestrel {
namespace {

constexpr char kKindName[kObserveKinds][6] = {"hit", "fill", "copy", "error"};

struct Bound {
  std::uint64_t us;
  const char* le;
};

// Cumulative Prometheus buckets. The HDR stays finer than this; the scrape coarsens it.
constexpr Bound kBounds[] = {
    {500, "0.0005"},     {1000, "0.001"},     {2000, "0.002"},     {5000, "0.005"},
    {10000, "0.01"},     {20000, "0.02"},     {50000, "0.05"},     {100000, "0.1"},
    {250000, "0.25"},    {500000, "0.5"},     {1000000, "1"},      {2500000, "2.5"},
};

struct Out {
  std::byte* dest = nullptr;
  std::size_t cap = 0;
  std::size_t len = 0;
  bool ok = true;

  void add(const char* text) noexcept {
    if (!ok || text == nullptr) {
      return;
    }
    const std::size_t n = std::strlen(text);
    if (len + n > cap) {
      ok = false;
      return;
    }
    std::memcpy(dest + len, text, n);
    len += n;
  }

  void u64(std::uint64_t value) noexcept {
    char buf[32];
    char* end = buf + sizeof(buf);
    char* p = end;
    do {
      --p;
      *p = static_cast<char>('0' + (value % 10ull));
      value /= 10ull;
    } while (value != 0 && p > buf);
    if (!ok || static_cast<std::size_t>(end - p) > cap - len) {
      ok = false;
      return;
    }
    std::memcpy(dest + len, p, static_cast<std::size_t>(end - p));
    len += static_cast<std::size_t>(end - p);
  }
};

// GCC rejects atomic_thread_fence under -fsanitize=thread. An acq_rel update of a
// private atomic is the same local order: earlier loads stay above it, and the
// sequence check stays below it.
[[nodiscard]] bool same_seq(const Published& src, std::uint64_t seq) noexcept {
  std::atomic<std::uint8_t> order{0};
  order.fetch_add(0, std::memory_order_acq_rel);
  return seq == src.seq.load(std::memory_order_relaxed);
}

void relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  asm volatile("pause" ::: "memory");
#else
  asm volatile("" ::: "memory");
#endif
}

// The writer holds the seq odd for one copy of the private histogram. Wait that
// out, then copy every kind under the same even seq. A failed attempt drops
// this core for this scrape; the next scrape reads it.
[[nodiscard]] bool wait_even(const Published& src, std::uint64_t& seq) noexcept {
  for (int i = 0; i < 100000; ++i) {
    seq = src.seq.load(std::memory_order_acquire);
    if ((seq & 1ull) == 0) {
      return true;
    }
    relax();
  }
  return false;
}

[[nodiscard]] bool load_core(const Published& src, Hdr* kinds) noexcept {
  for (int attempt = 0; attempt < 4; ++attempt) {
    std::uint64_t seq = 0;
    if (!wait_even(src, seq)) {
      continue;
    }
    for (int kind = 0; kind < kObserveKinds; ++kind) {
      kinds[kind].total = src.total[kind].load(std::memory_order_relaxed);
      kinds[kind].sum_us = src.sum_us[kind].load(std::memory_order_relaxed);
      for (int i = 0; i < kHdrLen; ++i) {
        kinds[kind].counts[i] = src.counts[kind][i].load(std::memory_order_relaxed);
      }
    }
    if (same_seq(src, seq)) {
      return true;
    }
  }
  return false;
}

void write_histogram(Out& out, const Hdr& hdr, const char* kind) noexcept {
  std::uint64_t cumulative = 0;
  int cursor = 0;
  for (const Bound& bound : kBounds) {
    while (cursor < kHdrLen && hdr_lowest(cursor + 1) <= bound.us) {
      cumulative += hdr.counts[cursor];
      ++cursor;
    }
    out.add("kestrel_request_duration_seconds_bucket{result=\"");
    out.add(kind);
    out.add("\",le=\"");
    out.add(bound.le);
    out.add("\"} ");
    out.u64(cumulative);
    out.add("\n");
  }
  out.add("kestrel_request_duration_seconds_bucket{result=\"");
  out.add(kind);
  out.add("\",le=\"+Inf\"} ");
  out.u64(hdr.total);
  out.add("\n");
  out.add("kestrel_request_duration_seconds_sum{result=\"");
  out.add(kind);
  out.add("\"} ");
  out.u64(hdr.sum_us / 1000000ull);
  out.add(".");
  std::uint64_t frac = hdr.sum_us % 1000000ull;
  char digits[6];
  for (int i = 5; i >= 0; --i) {
    digits[i] = static_cast<char>('0' + (frac % 10ull));
    frac /= 10ull;
  }
  if (out.ok && out.len + 6 <= out.cap) {
    std::memcpy(out.dest + out.len, digits, 6);
    out.len += 6;
  } else {
    out.ok = false;
  }
  out.add("\n");
  out.add("kestrel_request_duration_seconds_count{result=\"");
  out.add(kind);
  out.add("\"} ");
  out.u64(hdr.total);
  out.add("\n");
}

}  // namespace

void publish_stats(CoreStats& src, Published& dst) noexcept {
  if (!src.dirty) {
    return;
  }
  // Odd value: a scrape will retry. acq_rel keeps the payload stores below
  // this update. The release increment publishes them with the even value.
  dst.seq.fetch_add(1, std::memory_order_acq_rel);
  for (int kind = 0; kind < kObserveKinds; ++kind) {
    dst.total[kind].store(src.hdr[kind].total, std::memory_order_relaxed);
    dst.sum_us[kind].store(src.hdr[kind].sum_us, std::memory_order_relaxed);
    for (int i = 0; i < kHdrLen; ++i) {
      dst.counts[kind][i].store(src.hdr[kind].counts[i], std::memory_order_relaxed);
    }
  }
  for (int i = 0; i < kTraceSlots; ++i) {
    dst.trace[i].store(src.trace[i], std::memory_order_relaxed);
  }
  dst.seq.fetch_add(1, std::memory_order_release);
  src.dirty = false;
}

std::size_t write_prometheus(const Published* slots, int cores, std::span<std::byte> dest) noexcept {
  Out out;
  out.dest = dest.data();
  out.cap = dest.size();
  out.add("# TYPE kestrel_request_duration_seconds histogram\n");
  Hdr merged[kObserveKinds];
  for (int core = 0; core < cores; ++core) {
    Hdr one[kObserveKinds];
    if (!load_core(slots[core], one)) {
      continue;
    }
    for (int kind = 0; kind < kObserveKinds; ++kind) {
      merged[kind].add(one[kind]);
    }
  }
  for (int kind = 0; kind < kObserveKinds; ++kind) {
    write_histogram(out, merged[kind], kKindName[kind]);
  }
  return out.ok ? out.len : 0;
}

std::size_t write_debug(const Published* slots, int cores, bool drop_upstream, bool drop_peers,
                       std::span<std::byte> dest) noexcept {
  char metrics[48 * 1024];
  const std::size_t n = write_prometheus(slots, cores, std::span<std::byte>(reinterpret_cast<std::byte*>(metrics), sizeof(metrics)));
  if (n == 0 || n >= sizeof(metrics)) {
    return 0;
  }
  metrics[n] = '\0';
  Out page;
  page.dest = dest.data();
  page.cap = dest.size();
  page.add("<!doctype html><meta charset=utf-8><title>kestrel</title><pre>");
  page.add(metrics);
  page.add("\n");
  for (int core = 0; core < cores; ++core) {
    std::uint64_t trace[kTraceSlots]{};
    bool loaded = false;
    for (int attempt = 0; attempt < 4 && !loaded; ++attempt) {
      std::uint64_t seq = 0;
      if (!wait_even(slots[core], seq)) {
        continue;
      }
      for (int i = 0; i < kTraceSlots; ++i) {
        trace[i] = slots[core].trace[i].load(std::memory_order_relaxed);
      }
      loaded = same_seq(slots[core], seq);
    }
    if (!loaded) {
      continue;
    }
    for (int i = 0; i < kTraceSlots; ++i) {
      if (trace[i] == 0) {
        continue;
      }
      const auto kind = static_cast<unsigned>(trace[i] & 0xffull);
      page.add("core ");
      page.u64(static_cast<std::uint64_t>(core));
      page.add(" ");
      page.add(kind < static_cast<unsigned>(kObserveKinds) ? kKindName[kind] : "error");
      page.add(" ");
      page.u64(trace[i] >> 8);
      page.add("us\n");
    }
  }
  page.add("</pre><p>upstream ");
  page.add(drop_upstream ? "dropped" : "open");
  page.add(", peers ");
  page.add(drop_peers ? "dropped" : "open");
  page.add("</p>"
           "<form method=POST action=/chaos><button name=upstream value=drop>drop upstream</button></form>"
           "<form method=POST action=/chaos><button name=upstream value=ok>restore upstream</button></form>"
           "<form method=POST action=/chaos><button name=peers value=drop>drop peers</button></form>"
           "<form method=POST action=/chaos><button name=peers value=ok>restore peers</button></form>");
  return page.ok ? page.len : 0;
}

}  // namespace kestrel

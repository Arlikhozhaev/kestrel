#include "kestrel/wal.hpp"
#include "check.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {

[[nodiscard]] bool write_at(const char* path, std::uint64_t offset, const void* data, std::size_t n) {
  const int fd = ::open(path, O_RDWR);
  if (fd < 0) {
    return false;
  }
  const ssize_t wrote = ::pwrite(fd, data, n, static_cast<off_t>(offset));
  ::close(fd);
  return wrote == static_cast<ssize_t>(n);
}

void test_round_trip() {
  const char* path = "/tmp/kestrel-wal-roundtrip.wal";
  ::unlink(path);
  {
    kestrel::Wal wal(path);
    CHECK(wal.ok());
    const float a[] = {1.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
    const float b[] = {0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
    const float c[] = {0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 0.f};
    CHECK(wal.append(a, "one", "alpha") != UINT64_MAX);
    CHECK(wal.append(b, "two", "beta") != UINT64_MAX);
    CHECK(wal.append(c, "three", "gamma") != UINT64_MAX);
  }
  kestrel::Wal wal(path);
  CHECK(wal.ok());
  std::vector<std::string> texts;
  const std::size_t n = wal.recover([&](const kestrel::WalRecord& record) {
    texts.emplace_back(record.text);
    CHECK(record.vector.size() == 8u);
    return true;
  });
  CHECK(n == 3u);
  CHECK(texts.size() == 3u);
  CHECK(texts[0] == "one");
  CHECK(texts[1] == "two");
  CHECK(texts[2] == "three");
  ::unlink(path);
}

void test_torn_and_crc() {
  const char* path = "/tmp/kestrel-wal-torn.wal";
  ::unlink(path);
  std::uint64_t second = 0;
  std::uint64_t end = 0;
  {
    kestrel::Wal wal(path);
    const float a[] = {1.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
    CHECK(wal.append(a, "keep", "yes") != UINT64_MAX);
    second = wal.append(a, "drop", "no");
    CHECK(second != UINT64_MAX);
    end = wal.end();
    CHECK(end > second);
  }
  const unsigned char junk = 0x7f;
  CHECK(write_at(path, second, &junk, 1));
  {
    kestrel::Wal wal(path);
    int seen = 0;
    const std::size_t n = wal.recover([&](const kestrel::WalRecord& record) {
      CHECK(record.text == "keep");
      ++seen;
      return true;
    });
    CHECK(n == 1u);
    CHECK(seen == 1);
    const float b[] = {0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
    CHECK(wal.append(b, "after", "recovered") != UINT64_MAX);
  }
  {
    kestrel::Wal wal(path);
    std::vector<std::string> texts;
    wal.recover([&](const kestrel::WalRecord& record) {
      texts.emplace_back(record.text);
      return true;
    });
    CHECK(texts.size() == 2u);
    CHECK(texts[0] == "keep");
    CHECK(texts[1] == "after");
  }
  ::unlink(path);
}

}  // namespace

int main() {
  test_round_trip();
  test_torn_and_crc();
  return kestrel::test::failures.load() == 0 ? 0 : 1;
}

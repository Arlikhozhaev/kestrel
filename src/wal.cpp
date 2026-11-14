#include "kestrel/wal.hpp"

#include "kestrel/frame.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace kestrel {
namespace {

constexpr std::uint64_t kInitialBytes = 1u << 20;

void store_u32(std::byte* dest, std::uint32_t value) noexcept {
  dest[0] = static_cast<std::byte>(value & 0xffu);
  dest[1] = static_cast<std::byte>((value >> 8) & 0xffu);
  dest[2] = static_cast<std::byte>((value >> 16) & 0xffu);
  dest[3] = static_cast<std::byte>((value >> 24) & 0xffu);
}

std::uint32_t load_u32(const std::byte* src) noexcept {
  return static_cast<std::uint32_t>(static_cast<unsigned char>(src[0])) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(src[1])) << 8) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(src[2])) << 16) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(src[3])) << 24);
}

}  // namespace

Wal::Wal(const char* path) noexcept {
  if (path == nullptr || path[0] == '\0') {
    std::fputs("kestrel: WAL path is empty\n", stderr);
    return;
  }
  const long page = ::sysconf(_SC_PAGESIZE);
  page_ = page > 0 ? static_cast<std::size_t>(page) : 4096;
  fd_ = ::open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (fd_ < 0) {
    std::fprintf(stderr, "kestrel: open %s failed: %s\n", path, std::strerror(errno));
    return;
  }
  const off_t size = ::lseek(fd_, 0, SEEK_END);
  if (size < 0) {
    std::fprintf(stderr, "kestrel: lseek %s failed: %s\n", path, std::strerror(errno));
    ::close(fd_);
    fd_ = -1;
    return;
  }
  std::uint64_t bytes = static_cast<std::uint64_t>(size);
  if (bytes < kInitialBytes) {
    if (::ftruncate(fd_, static_cast<off_t>(kInitialBytes)) != 0) {
      std::fprintf(stderr, "kestrel: ftruncate %s failed: %s\n", path, std::strerror(errno));
      ::close(fd_);
      fd_ = -1;
      return;
    }
    bytes = kInitialBytes;
  }
  void* mapped = ::mmap(nullptr, static_cast<std::size_t>(bytes), PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
  if (mapped == MAP_FAILED) {
    std::fprintf(stderr, "kestrel: mmap WAL failed: %s\n", std::strerror(errno));
    ::close(fd_);
    fd_ = -1;
    return;
  }
  map_ = static_cast<std::byte*>(mapped);
  mapped_ = bytes;
  end_ = static_cast<std::uint64_t>(size);
  if (end_ > mapped_) {
    end_ = mapped_;
  }
}

Wal::~Wal() {
  if (map_ != nullptr) {
    ::munmap(map_, static_cast<std::size_t>(mapped_));
  }
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

bool Wal::ensure(std::uint64_t need) noexcept {
  if (need <= mapped_) {
    return true;
  }
  std::uint64_t next = mapped_ == 0 ? kInitialBytes : mapped_;
  while (next < need) {
    if (next > (UINT64_MAX / 2u)) {
      return false;
    }
    next *= 2u;
  }
  if (::ftruncate(fd_, static_cast<off_t>(next)) != 0) {
    std::fprintf(stderr, "kestrel: WAL grow failed: %s\n", std::strerror(errno));
    return false;
  }
  void* moved = ::mremap(map_, static_cast<std::size_t>(mapped_), static_cast<std::size_t>(next), MREMAP_MAYMOVE);
  if (moved == MAP_FAILED) {
    std::fprintf(stderr, "kestrel: WAL remap failed: %s\n", std::strerror(errno));
    return false;
  }
  map_ = static_cast<std::byte*>(moved);
  mapped_ = next;
  return true;
}

bool Wal::sync_range(std::uint64_t begin, std::uint64_t end) noexcept {
  if (end <= begin || map_ == nullptr) {
    return true;
  }
  const std::uint64_t page = page_;
  const std::uint64_t aligned = begin & ~(page - 1u);
  const std::uint64_t length = end - aligned;
  if (::msync(map_ + aligned, static_cast<std::size_t>(length), MS_SYNC) != 0) {
    std::fprintf(stderr, "kestrel: WAL msync failed: %s\n", std::strerror(errno));
    return false;
  }
  if (::fdatasync(fd_) != 0) {
    std::fprintf(stderr, "kestrel: WAL fdatasync failed: %s\n", std::strerror(errno));
    return false;
  }
  return true;
}

void Wal::truncate_to(std::uint64_t size) noexcept {
  if (size > end_) {
    return;
  }
  end_ = size;
  if (::ftruncate(fd_, static_cast<off_t>(mapped_)) != 0) {
    std::fprintf(stderr, "kestrel: WAL truncate failed: %s\n", std::strerror(errno));
  }
  if (size < mapped_) {
    std::memset(map_ + size, 0, static_cast<std::size_t>(mapped_ - size > 4096 ? 4096 : mapped_ - size));
  }
}

std::uint64_t Wal::append(std::span<const float> vector, std::string_view text,
                          std::string_view response) noexcept {
  if (!ok() || vector.empty()) {
    return UINT64_MAX;
  }
  if (vector.size() > 0xffffffffu || text.size() > 0xffffffffu || response.size() > 0xffffffffu) {
    return UINT64_MAX;
  }
  const std::uint64_t payload =
      12u + static_cast<std::uint64_t>(vector.size()) * sizeof(float) + text.size() + response.size();
  if (payload > kMaxFramePayload) {
    return UINT64_MAX;
  }
  const std::uint64_t total = kFrameHeaderBytes + payload;
  if (!ensure(end_ + total)) {
    return UINT64_MAX;
  }
  const std::uint64_t record = end_;
  std::byte* dest = map_ + record;
  std::byte* body = dest + kFrameHeaderBytes;
  store_u32(body + 0, static_cast<std::uint32_t>(vector.size()));
  store_u32(body + 4, static_cast<std::uint32_t>(text.size()));
  store_u32(body + 8, static_cast<std::uint32_t>(response.size()));
  std::memcpy(body + 12, vector.data(), vector.size() * sizeof(float));
  if (!text.empty()) {
    std::memcpy(body + 12 + vector.size() * sizeof(float), text.data(), text.size());
  }
  if (!response.empty()) {
    std::memcpy(body + 12 + vector.size() * sizeof(float) + text.size(), response.data(), response.size());
  }
  FrameFields fields;
  fields.type = FrameType::CacheEntry;
  fields.stream_id = record + kFrameHeaderBytes;
  if (!encode_frame(std::span<std::byte>(dest, static_cast<std::size_t>(total)), fields,
                    std::span<const std::byte>(body, static_cast<std::size_t>(payload)))) {
    return UINT64_MAX;
  }
  end_ = record + total;
  if (!sync_range(record, end_)) {
    end_ = record;
    return UINT64_MAX;
  }
  return fields.stream_id;
}

bool Wal::read(std::uint64_t payload_offset, WalRecord& out) const noexcept {
  out = WalRecord{};
  if (!ok() || payload_offset < kFrameHeaderBytes || payload_offset + 12u > end_) {
    return false;
  }
  const std::byte* body = map_ + payload_offset;
  const std::uint32_t dimension = load_u32(body + 0);
  const std::uint32_t text_len = load_u32(body + 4);
  const std::uint32_t response_len = load_u32(body + 8);
  const std::uint64_t bytes =
      12u + static_cast<std::uint64_t>(dimension) * sizeof(float) + text_len + response_len;
  if (payload_offset + bytes > end_) {
    return false;
  }
  const auto* floats = reinterpret_cast<const float*>(body + 12);
  const char* text = reinterpret_cast<const char*>(body + 12 + static_cast<std::size_t>(dimension) * sizeof(float));
  const char* response = text + text_len;
  out.payload_offset = payload_offset;
  out.vector = std::span<const float>(floats, dimension);
  out.text = std::string_view(text, text_len);
  out.response = std::string_view(response, response_len);
  return true;
}

}  // namespace kestrel

#include "kestrel/crc32c.hpp"
#include "kestrel/frame.hpp"
#include "check.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>

namespace {

void test_crc_vector() {
  const char* text = "123456789";
  const auto bytes = std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(text), 9);
  CHECK(kestrel::crc32c_software(bytes) == 0xE3069283u);
  CHECK(kestrel::crc32c(bytes) == 0xE3069283u);
  CHECK(kestrel::crc32c(std::span<const std::byte>{}) == 0u);
  CHECK(kestrel::crc32c_software(std::span<const std::byte>{}) == 0u);

  std::byte buffer[128];
  for (std::size_t n = 0; n < sizeof(buffer); ++n) {
    buffer[n] = static_cast<std::byte>(n * 17u + 3u);
    const auto view = std::span<const std::byte>(buffer, n);
    CHECK(kestrel::crc32c(view) == kestrel::crc32c_software(view));

    if (n == 0) {
      continue;
    }
    kestrel::Crc32c split;
    split.update(std::span<const std::byte>(buffer, 1));
    split.update(std::span<const std::byte>(buffer + 1, n - 1));
    CHECK(split.finish() == kestrel::crc32c_software(view));
  }
}

void test_roundtrip() {
  std::byte payload_bytes[2] = {std::byte{0xAA}, std::byte{0xBB}};
  kestrel::FrameFields fields;
  fields.flags = 0x3;
  fields.stream_id = 0x0102030405060708ull;
  fields.type = kestrel::FrameType::CacheEntry;

  std::byte dest[64] = {};
  CHECK(kestrel::encode_frame(dest, fields, payload_bytes));
  CHECK(dest[0] == std::byte{0x4B});
  CHECK(dest[1] == std::byte{0x53});
  CHECK(dest[2] == std::byte{0x54});
  CHECK(dest[3] == std::byte{0x52});
  CHECK(dest[4] == std::byte{1});
  CHECK(dest[5] == std::byte{0});
  CHECK(dest[8] == std::byte{32});
  CHECK(dest[12] == std::byte{2});
  CHECK(dest[30] == std::byte{0});
  CHECK(dest[31] == std::byte{0});

  kestrel::DecodedFrame decoded;
  CHECK(kestrel::decode_frame(std::span<const std::byte>(dest, 34), decoded) ==
        kestrel::DecodeStatus::Ok);
  CHECK(decoded.version == 1);
  CHECK(decoded.flags == 0x3);
  CHECK(decoded.stream_id == fields.stream_id);
  CHECK(decoded.type == kestrel::FrameType::CacheEntry);
  CHECK(decoded.payload.size() == 2);
  CHECK(decoded.payload[0] == std::byte{0xAA});
  CHECK(decoded.payload[1] == std::byte{0xBB});

  std::byte flipped[64];
  std::memcpy(flipped, dest, 34);
  flipped[33] = std::byte{0x00};
  CHECK(kestrel::decode_frame(std::span<const std::byte>(flipped, 34), decoded) ==
        kestrel::DecodeStatus::BadCrc);
}

void test_partial_and_reject() {
  kestrel::DecodedFrame decoded;
  std::byte scratch[64] = {};
  CHECK(kestrel::decode_frame({}, decoded) == kestrel::DecodeStatus::NeedMore);
  CHECK(kestrel::decode_frame(std::span<const std::byte>(scratch, 31), decoded) ==
        kestrel::DecodeStatus::NeedMore);

  kestrel::FrameFields fields;
  fields.type = kestrel::FrameType::Ping;
  CHECK(kestrel::encode_frame(scratch, fields, {}));
  CHECK(kestrel::decode_frame(std::span<const std::byte>(scratch, 32), decoded) ==
        kestrel::DecodeStatus::Ok);
  CHECK(decoded.payload.empty());
  CHECK(decoded.type == kestrel::FrameType::Ping);

  std::byte payload[10] = {};
  std::byte framed[64] = {};
  CHECK(kestrel::encode_frame(framed, fields, payload));
  CHECK(kestrel::decode_frame(std::span<const std::byte>(framed, 32 + 5), decoded) ==
        kestrel::DecodeStatus::NeedMore);
  CHECK(kestrel::decode_frame(std::span<const std::byte>(framed, 42), decoded) ==
        kestrel::DecodeStatus::Ok);

  framed[0] = std::byte{0};
  CHECK(kestrel::decode_frame(std::span<const std::byte>(framed, 42), decoded) ==
        kestrel::DecodeStatus::BadMagic);

  CHECK(kestrel::encode_frame(framed, fields, payload));
  framed[4] = std::byte{2};
  CHECK(kestrel::decode_frame(std::span<const std::byte>(framed, 42), decoded) ==
        kestrel::DecodeStatus::BadVersion);

  CHECK(kestrel::encode_frame(framed, fields, payload));
  framed[8] = std::byte{16};
  CHECK(kestrel::decode_frame(std::span<const std::byte>(framed, 42), decoded) ==
        kestrel::DecodeStatus::BadLength);

  CHECK(kestrel::encode_frame(framed, fields, payload));
  framed[12] = std::byte{0xFF};
  framed[13] = std::byte{0xFF};
  framed[14] = std::byte{0xFF};
  framed[15] = std::byte{0x7F};
  CHECK(kestrel::decode_frame(std::span<const std::byte>(framed, 42), decoded) ==
        kestrel::DecodeStatus::BadLength);

  CHECK(kestrel::encode_frame(framed, fields, payload));
  framed[30] = std::byte{1};
  CHECK(kestrel::decode_frame(std::span<const std::byte>(framed, 42), decoded) ==
        kestrel::DecodeStatus::BadLength);

  std::byte tiny[8] = {};
  CHECK(!kestrel::encode_frame(tiny, fields, {}));

  std::byte overlap[64] = {};
  CHECK(!kestrel::encode_frame(std::span<std::byte>(overlap, 64), fields,
                               std::span<const std::byte>(overlap, 4)));
  CHECK(overlap[0] == std::byte{0});
}

void test_inplace_payload() {
  std::byte dest[48] = {};
  dest[32] = std::byte{0x10};
  dest[33] = std::byte{0x20};
  dest[34] = std::byte{0x30};
  kestrel::FrameFields fields;
  fields.stream_id = 7;
  fields.type = kestrel::FrameType::PingAck;
  const auto payload = std::span<const std::byte>(dest + 32, 3);
  CHECK(kestrel::encode_frame(dest, fields, payload));
  CHECK(dest[32] == std::byte{0x10});
  CHECK(dest[33] == std::byte{0x20});
  CHECK(dest[34] == std::byte{0x30});
  kestrel::DecodedFrame decoded;
  CHECK(kestrel::decode_frame(std::span<const std::byte>(dest, 35), decoded) ==
        kestrel::DecodeStatus::Ok);
  CHECK(decoded.stream_id == 7);
  CHECK(decoded.type == kestrel::FrameType::PingAck);
  CHECK(decoded.payload.size() == 3);
  CHECK(decoded.payload[2] == std::byte{0x30});
}

}  // namespace

int main() {
  test_crc_vector();
  test_roundtrip();
  test_partial_and_reject();
  test_inplace_payload();
  const int failed = kestrel::test::failures.load();
  if (failed == 0) {
    std::puts("frame_test ok");
  }
  return failed == 0 ? 0 : 1;
}

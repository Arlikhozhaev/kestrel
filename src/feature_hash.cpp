#include "kestrel/feature_hash.hpp"

#include "kestrel/simd.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace kestrel {
namespace {

[[nodiscard]] bool is_token(unsigned char c) noexcept {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

[[nodiscard]] unsigned char lower_ascii(unsigned char c) noexcept {
  if (c >= 'A' && c <= 'Z') {
    return static_cast<unsigned char>(c + ('a' - 'A'));
  }
  return c;
}

// Eight coordinates from mixed high bits. hash % dimension and one sign bit
// only distinguish 2 * dimension vectors, so different tokens shared one.
void project(float* out, std::size_t dimension, std::uint64_t hash) noexcept {
  std::uint64_t mixed = hash;
  for (std::uint32_t i = 0; i < 8u; ++i) {
    mixed ^= mixed >> 33;
    mixed *= 0x9E3779B185EBCA87ull;
    const std::size_t bucket = static_cast<std::size_t>((mixed >> 32) % dimension);
    out[bucket] += ((mixed >> 17) & 1ull) != 0ull ? 1.f : -1.f;
  }
}

}  // namespace

bool FeatureHashEmbedder::embed(std::string_view text, std::span<float> out) const noexcept {
  if (dimension_ == 0 || out.size() != dimension_ || out.data() == nullptr) {
    return false;
  }
  std::memset(out.data(), 0, dimension_ * sizeof(float));
  if (text.empty()) {
    return true;
  }
  const char* cursor = text.data();
  const char* end = cursor + text.size();
  std::uint64_t previous = 0;
  bool have_previous = false;
  while (cursor < end) {
    while (cursor < end && !is_token(static_cast<unsigned char>(*cursor))) {
      ++cursor;
    }
    const char* start = cursor;
    while (cursor < end && is_token(static_cast<unsigned char>(*cursor))) {
      ++cursor;
    }
    if (start == cursor) {
      break;
    }
    std::uint64_t hash = 14695981039346656037ull;
    for (const char* p = start; p < cursor; ++p) {
      hash ^= lower_ascii(static_cast<unsigned char>(*p));
      hash *= 1099511628211ull;
    }
    project(out.data(), dimension_, hash);
    if (have_previous && dimension_ > 1) {
      const std::uint64_t bigram = previous ^ (hash + 0x9E3779B97F4A7C15ull);
      out[static_cast<std::size_t>(bigram % dimension_)] += 0.5f;
    }
    previous = hash;
    have_previous = true;
  }
  float squared = 0.f;
  for (std::size_t i = 0; i < dimension_; ++i) {
    squared += out[i] * out[i];
  }
  if (squared > 0.f) {
    scale_inplace(out, 1.f / std::sqrt(squared));
  }
  return true;
}

}  // namespace kestrel

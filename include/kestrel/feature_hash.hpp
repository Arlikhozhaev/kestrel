#pragma once

#include <cstddef>
#include <span>
#include <string_view>

namespace kestrel {

// Deterministic feature-hash embedder. Identical text produces an identical
// unit vector, which is what the cache hit path needs before a model embedder
// is configured. It does not place paraphrases next to each other.
// Swap the call site when a real embedder exists. The index only sees floats.
class FeatureHashEmbedder {
 public:
  explicit FeatureHashEmbedder(std::size_t dimension) noexcept : dimension_(dimension) {}

  [[nodiscard]] std::size_t dimension() const noexcept { return dimension_; }

  // `out.size()` must equal dimension(). Empty text yields a zero vector.
  [[nodiscard]] bool embed(std::string_view text, std::span<float> out) const noexcept;

 private:
  std::size_t dimension_ = 0;
};

}  // namespace kestrel

#pragma once

#include <cstddef>
#include <span>

namespace kestrel {

// Dimension used by the index. AVX2 consumes 8 floats per instruction.
[[nodiscard]] constexpr std::size_t padded_dimension(std::size_t dimension) noexcept {
  if (dimension == 0) {
    return 0;
  }
  return (dimension + 7u) & ~std::size_t{7};
}

// a and b are 32-byte aligned, n is a multiple of 8. The hot distance kernel.
// Defined in simd.cpp. Inlining the NEON loads onto a fixed stack array makes
// GCC 13 report a stringop over-read, and -Werror stops the arm64 image.
[[nodiscard]] float dot_product(const float* a, const float* b, std::size_t n) noexcept;

void scale_inplace(std::span<float> values, float factor) noexcept;

}  // namespace kestrel

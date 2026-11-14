#pragma once

#include <cstddef>
#include <span>

#if defined(__AVX2__)
#include <immintrin.h>
#elif defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace kestrel {

// Dimension used by the index. AVX2 consumes 8 floats per instruction.
[[nodiscard]] constexpr std::size_t padded_dimension(std::size_t dimension) noexcept {
  if (dimension == 0) {
    return 0;
  }
  return (dimension + 7u) & ~std::size_t{7};
}

// a and b are 32-byte aligned, n is a multiple of 8. The hot distance kernel.
[[nodiscard]] inline float dot_product(const float* a, const float* b, std::size_t n) noexcept {
  float sum = 0.f;
  std::size_t i = 0;
#if defined(__AVX2__)
  __m256 acc = _mm256_setzero_ps();
  for (; i + 8 <= n; i += 8) {
    const __m256 va = _mm256_load_ps(a + i);
    const __m256 vb = _mm256_load_ps(b + i);
    acc = _mm256_fmadd_ps(va, vb, acc);
  }
  alignas(32) float lanes[8];
  _mm256_store_ps(lanes, acc);
  sum = lanes[0] + lanes[1] + lanes[2] + lanes[3] + lanes[4] + lanes[5] + lanes[6] + lanes[7];
#elif defined(__ARM_NEON)
  float32x4_t acc = vdupq_n_f32(0.f);
  for (; i + 4 <= n; i += 4) {
    const float32x4_t va = vld1q_f32(a + i);
    const float32x4_t vb = vld1q_f32(b + i);
    acc = vfmaq_f32(acc, va, vb);
  }
  sum = vaddvq_f32(acc);
#endif
  for (; i < n; ++i) {
    sum += a[i] * b[i];
  }
  return sum;
}

inline void scale_inplace(std::span<float> values, float factor) noexcept {
  std::size_t i = 0;
  float* data = values.data();
  const std::size_t n = values.size();
#if defined(__AVX2__)
  const __m256 factor_v = _mm256_set1_ps(factor);
  for (; i + 8 <= n; i += 8) {
    __m256 v = _mm256_loadu_ps(data + i);
    v = _mm256_mul_ps(v, factor_v);
    _mm256_storeu_ps(data + i, v);
  }
#elif defined(__ARM_NEON)
  const float32x4_t factor_v = vdupq_n_f32(factor);
  for (; i + 4 <= n; i += 4) {
    float32x4_t v = vld1q_f32(data + i);
    v = vmulq_f32(v, factor_v);
    vst1q_f32(data + i, v);
  }
#endif
  for (; i < n; ++i) {
    data[i] *= factor;
  }
}

}  // namespace kestrel

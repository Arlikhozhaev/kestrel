#include "kestrel/simd.hpp"

#include <cstring>

#if defined(__AVX2__)
#include <immintrin.h>
#elif defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace kestrel {

float dot_product(const float* a, const float* b, std::size_t n) noexcept {
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
    float32x4_t va;
    float32x4_t vb;
    std::memcpy(&va, a + i, sizeof(va));
    std::memcpy(&vb, b + i, sizeof(vb));
    acc = vfmaq_f32(acc, va, vb);
  }
  float lanes[4];
  std::memcpy(lanes, &acc, sizeof(lanes));
  sum = lanes[0] + lanes[1] + lanes[2] + lanes[3];
#endif
  for (; i < n; ++i) {
    sum += a[i] * b[i];
  }
  return sum;
}

void scale_inplace(std::span<float> values, float factor) noexcept {
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
    float32x4_t v;
    std::memcpy(&v, data + i, sizeof(v));
    v = vmulq_f32(v, factor_v);
    std::memcpy(data + i, &v, sizeof(v));
  }
#endif
  for (; i < n; ++i) {
    data[i] *= factor;
  }
}

}  // namespace kestrel

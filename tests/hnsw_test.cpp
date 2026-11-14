#include "kestrel/feature_hash.hpp"
#include "kestrel/hnsw.hpp"
#include "kestrel/simd.hpp"
#include "check.hpp"

#include <cmath>
#include <cstring>

namespace {

void fill(float* dest, std::size_t n, float value, std::size_t at) {
  std::memset(dest, 0, n * sizeof(float));
  if (at < n) {
    dest[at] = value;
  }
}

void test_neighbors() {
  kestrel::HnswConfig cfg;
  cfg.dimension = 8;
  cfg.max_nodes = 16;
  cfg.m = 4;
  cfg.m0 = 8;
  cfg.ef_construction = 16;
  cfg.ef_search = 16;
  kestrel::HnswIndex index(cfg);
  CHECK(index.ok());
  CHECK(index.dimension() == 8);

  alignas(32) float a[8];
  alignas(32) float b[8];
  alignas(32) float c[8];
  fill(a, 8, 1.f, 0);
  fill(b, 8, 1.f, 1);
  fill(c, 8, 1.f, 0);
  CHECK(index.insert(a, 10) == 0u);
  CHECK(index.insert(b, 20) == 1u);
  CHECK(index.insert(c, 30) == 2u);

  kestrel::HnswHit hits[2];
  const std::size_t found = index.search(a, hits);
  CHECK(found >= 1);
  CHECK(hits[0].score > 0.99f);
  CHECK(hits[0].payload == 10u || hits[0].payload == 30u);

  const std::size_t other = index.search(b, hits);
  CHECK(other >= 1);
  CHECK(hits[0].payload == 20u);
  CHECK(hits[0].score > 0.99f);
}

void test_full() {
  kestrel::HnswConfig cfg;
  cfg.dimension = 8;
  cfg.max_nodes = 2;
  kestrel::HnswIndex index(cfg);
  alignas(32) float v[8];
  fill(v, 8, 1.f, 0);
  CHECK(index.insert(v, 1) != kestrel::kNilNode);
  CHECK(index.insert(v, 2) != kestrel::kNilNode);
  CHECK(index.insert(v, 3) == kestrel::kNilNode);
}

void test_feature_hash() {
  kestrel::FeatureHashEmbedder embed(64);
  alignas(32) float a[64];
  alignas(32) float b[64];
  alignas(32) float c[64];
  CHECK(embed.embed("ping", a));
  CHECK(embed.embed("ping", b));
  CHECK(embed.embed("pong", c));
  CHECK(embed.embed("", a) == true);
  float zeros = 0.f;
  CHECK(embed.embed("", a));
  for (float value : a) {
    zeros += value;
  }
  CHECK(zeros == 0.f);
  CHECK(embed.embed("ping", a));
  CHECK(embed.embed("ping", b));
  const float same = kestrel::dot_product(a, b, 64);
  const float different = kestrel::dot_product(a, c, 64);
  CHECK(same > 0.99f);
  CHECK(different < 0.97f);
}

}  // namespace

int main() {
  test_neighbors();
  test_full();
  test_feature_hash();
  return kestrel::test::failures.load() == 0 ? 0 : 1;
}

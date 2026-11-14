#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace kestrel {

// Single-writer HNSW. One reactor thread owns one index. There is no lock.
// Vectors are L2-normalized on insert. Search score is the dot product,
// which is cosine for those vectors. Higher is nearer.
//
// Memory is reserved up front. insert() returns kNil when the node table
// is full. The hit path does not allocate.
inline constexpr std::uint32_t kNilNode = 0xffffffffu;
inline constexpr std::uint32_t kHnswMaxLevel = 8;
inline constexpr std::size_t kHnswMaxDimension = 1024;

struct HnswConfig {
  std::size_t dimension = 64;
  std::uint32_t max_nodes = 4096;
  std::uint32_t m = 16;
  std::uint32_t m0 = 32;
  std::uint32_t ef_construction = 64;
  std::uint32_t ef_search = 64;
  std::uint32_t seed = 0x4b455354u;
};

struct HnswHit {
  std::uint32_t id = kNilNode;
  float score = 0.f;
  std::uint64_t payload = 0;
};

class HnswIndex {
 public:
  explicit HnswIndex(const HnswConfig& config) noexcept;
  HnswIndex(const HnswIndex&) = delete;
  HnswIndex& operator=(const HnswIndex&) = delete;
  HnswIndex(HnswIndex&&) = delete;
  HnswIndex& operator=(HnswIndex&&) = delete;
  ~HnswIndex();

  [[nodiscard]] bool ok() const noexcept { return vecs_ != nullptr; }
  [[nodiscard]] std::size_t dimension() const noexcept { return dimension_; }
  [[nodiscard]] std::uint32_t size() const noexcept { return count_; }
  [[nodiscard]] std::uint64_t payload(std::uint32_t id) const noexcept;

  // Copies and normalizes `vector`. `vector.size()` must equal dimension().
  [[nodiscard]] std::uint32_t insert(std::span<const float> vector, std::uint64_t payload) noexcept;

  // Writes the nearest hits, best first. Returns how many were written.
  // `query` is normalized internally into a stack temporary.
  [[nodiscard]] std::size_t search(std::span<const float> query, std::span<HnswHit> out) const noexcept;

 private:
  struct Node {
    std::uint32_t level = 0;
    std::uint32_t link_count[kHnswMaxLevel]{};
  };

  [[nodiscard]] float* vector_at(std::uint32_t id) noexcept;
  [[nodiscard]] const float* vector_at(std::uint32_t id) const noexcept;
  [[nodiscard]] float distance(std::uint32_t a, std::uint32_t b) const noexcept;
  [[nodiscard]] float distance_to(const float* query, std::uint32_t id) const noexcept;
  [[nodiscard]] std::uint32_t random_level() noexcept;
  [[nodiscard]] std::uint32_t neighbor_cap(std::uint32_t level) const noexcept;
  [[nodiscard]] std::uint32_t neighbor_at(std::uint32_t id, std::uint32_t level, std::uint32_t slot) const noexcept;
  void set_neighbor(std::uint32_t id, std::uint32_t level, std::uint32_t slot, std::uint32_t other) noexcept;
  void link(std::uint32_t a, std::uint32_t b, std::uint32_t level) noexcept;
  [[nodiscard]] std::uint32_t search_layer(const float* query, std::uint32_t entry, std::uint32_t ef,
                                           std::uint32_t level, std::uint32_t* found,
                                           float* found_dist, std::uint32_t found_cap) const noexcept;
  void mark(std::uint32_t id) const noexcept;
  [[nodiscard]] bool seen(std::uint32_t id) const noexcept;

  std::size_t dimension_ = 0;
  std::uint32_t max_nodes_ = 0;
  std::uint32_t m_ = 16;
  std::uint32_t m0_ = 32;
  std::uint32_t ef_construction_ = 64;
  std::uint32_t ef_search_ = 64;
  std::uint32_t count_ = 0;
  std::uint32_t entry_ = kNilNode;
  std::uint32_t entry_level_ = 0;
  std::uint32_t rng_ = 1;
  float* vecs_ = nullptr;
  Node* nodes_ = nullptr;
  std::uint32_t* links_ = nullptr;
  std::uint64_t* payloads_ = nullptr;
  mutable std::uint32_t* stamp_ = nullptr;
  mutable std::uint32_t epoch_ = 1;
};

}  // namespace kestrel

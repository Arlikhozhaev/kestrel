#include "kestrel/hnsw.hpp"

#include "kestrel/simd.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace kestrel {
namespace {

struct DistId {
  float dist = 0.f;
  std::uint32_t id = kNilNode;
};

void sift_up(DistId* heap, std::uint32_t index, bool min_heap) noexcept {
  while (index > 0) {
    const std::uint32_t parent = (index - 1u) / 2u;
    const bool out_of_order =
        min_heap ? heap[index].dist < heap[parent].dist : heap[index].dist > heap[parent].dist;
    if (!out_of_order) {
      break;
    }
    const DistId tmp = heap[index];
    heap[index] = heap[parent];
    heap[parent] = tmp;
    index = parent;
  }
}

void sift_down(DistId* heap, std::uint32_t count, std::uint32_t index, bool min_heap) noexcept {
  while (true) {
    std::uint32_t best = index;
    const std::uint32_t left = index * 2u + 1u;
    const std::uint32_t right = left + 1u;
    if (left < count) {
      const bool better = min_heap ? heap[left].dist < heap[best].dist : heap[left].dist > heap[best].dist;
      if (better) {
        best = left;
      }
    }
    if (right < count) {
      const bool better = min_heap ? heap[right].dist < heap[best].dist : heap[right].dist > heap[best].dist;
      if (better) {
        best = right;
      }
    }
    if (best == index) {
      break;
    }
    const DistId tmp = heap[index];
    heap[index] = heap[best];
    heap[best] = tmp;
    index = best;
  }
}

void heap_push(DistId* heap, std::uint32_t& count, DistId item, bool min_heap) noexcept {
  heap[count] = item;
  sift_up(heap, count, min_heap);
  ++count;
}

DistId heap_pop(DistId* heap, std::uint32_t& count, bool min_heap) noexcept {
  const DistId top = heap[0];
  --count;
  if (count > 0) {
    heap[0] = heap[count];
    sift_down(heap, count, 0, min_heap);
  }
  return top;
}

}  // namespace

HnswIndex::HnswIndex(const HnswConfig& config) noexcept {
  dimension_ = padded_dimension(config.dimension);
  if (dimension_ == 0 || dimension_ > kHnswMaxDimension || config.max_nodes < 2) {
    std::fputs("kestrel: HNSW dimension or capacity is unusable\n", stderr);
    return;
  }
  max_nodes_ = config.max_nodes;
  m_ = config.m == 0 ? 16u : config.m;
  m0_ = config.m0 < m_ ? m_ : config.m0;
  if (m0_ > 64u) {
    m0_ = 64u;
  }
  if (m_ > m0_) {
    m_ = m0_;
  }
  ef_construction_ = config.ef_construction < m_ ? m_ : config.ef_construction;
  ef_search_ = config.ef_search == 0 ? 16u : config.ef_search;
  if (ef_construction_ > 128u) {
    ef_construction_ = 128u;
  }
  if (ef_search_ > 128u) {
    ef_search_ = 128u;
  }
  rng_ = config.seed == 0 ? 0x4b455354u : config.seed;

  const std::size_t vector_bytes = static_cast<std::size_t>(max_nodes_) * dimension_ * sizeof(float);
  const std::size_t aligned_bytes = (vector_bytes + 31u) & ~std::size_t{31};
  vecs_ = static_cast<float*>(std::aligned_alloc(32, aligned_bytes == 0 ? 32 : aligned_bytes));
  nodes_ = static_cast<Node*>(std::calloc(max_nodes_, sizeof(Node)));
  links_ = static_cast<std::uint32_t*>(
      std::calloc(static_cast<std::size_t>(max_nodes_) * kHnswMaxLevel * m0_, sizeof(std::uint32_t)));
  payloads_ = static_cast<std::uint64_t*>(std::calloc(max_nodes_, sizeof(std::uint64_t)));
  stamp_ = static_cast<std::uint32_t*>(std::calloc(max_nodes_, sizeof(std::uint32_t)));
  if (vecs_ == nullptr || nodes_ == nullptr || links_ == nullptr || payloads_ == nullptr || stamp_ == nullptr) {
    std::fputs("kestrel: HNSW allocation failed\n", stderr);
    std::free(vecs_);
    std::free(nodes_);
    std::free(links_);
    std::free(payloads_);
    std::free(stamp_);
    vecs_ = nullptr;
    nodes_ = nullptr;
    links_ = nullptr;
    payloads_ = nullptr;
    stamp_ = nullptr;
    return;
  }
  const std::size_t link_count = static_cast<std::size_t>(max_nodes_) * kHnswMaxLevel * m0_;
  for (std::size_t i = 0; i < link_count; ++i) {
    links_[i] = kNilNode;
  }
}

HnswIndex::~HnswIndex() {
  std::free(vecs_);
  std::free(nodes_);
  std::free(links_);
  std::free(payloads_);
  std::free(stamp_);
}

std::uint64_t HnswIndex::payload(std::uint32_t id) const noexcept {
  if (id >= count_ || payloads_ == nullptr) {
    return 0;
  }
  return payloads_[id];
}

float* HnswIndex::vector_at(std::uint32_t id) noexcept {
  return vecs_ + static_cast<std::size_t>(id) * dimension_;
}

const float* HnswIndex::vector_at(std::uint32_t id) const noexcept {
  return vecs_ + static_cast<std::size_t>(id) * dimension_;
}

float HnswIndex::distance(std::uint32_t a, std::uint32_t b) const noexcept {
  return 1.f - dot_product(vector_at(a), vector_at(b), dimension_);
}

float HnswIndex::distance_to(const float* query, std::uint32_t id) const noexcept {
  return 1.f - dot_product(query, vector_at(id), dimension_);
}

std::uint32_t HnswIndex::random_level() noexcept {
  std::uint32_t level = 0;
  while (level + 1u < kHnswMaxLevel) {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    if ((rng_ % m_) != 0u) {
      break;
    }
    ++level;
  }
  return level;
}

std::uint32_t HnswIndex::neighbor_cap(std::uint32_t level) const noexcept {
  return level == 0 ? m0_ : m_;
}

std::uint32_t HnswIndex::neighbor_at(std::uint32_t id, std::uint32_t level, std::uint32_t slot) const noexcept {
  const std::size_t index =
      (static_cast<std::size_t>(id) * kHnswMaxLevel + level) * m0_ + slot;
  return links_[index];
}

void HnswIndex::set_neighbor(std::uint32_t id, std::uint32_t level, std::uint32_t slot,
                             std::uint32_t other) noexcept {
  const std::size_t index =
      (static_cast<std::size_t>(id) * kHnswMaxLevel + level) * m0_ + slot;
  links_[index] = other;
}

void HnswIndex::link(std::uint32_t from, std::uint32_t to, std::uint32_t level) noexcept {
  const std::uint32_t cap = neighbor_cap(level);
  std::uint32_t count = nodes_[from].link_count[level];
  for (std::uint32_t i = 0; i < count; ++i) {
    if (neighbor_at(from, level, i) == to) {
      return;
    }
  }
  if (count < cap) {
    set_neighbor(from, level, count, to);
    nodes_[from].link_count[level] = count + 1u;
    return;
  }
  DistId best[65];
  std::uint32_t best_count = 0;
  best[best_count++] = DistId{distance(from, to), to};
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::uint32_t other = neighbor_at(from, level, i);
    best[best_count++] = DistId{distance(from, other), other};
  }
  for (std::uint32_t i = 1; i < best_count; ++i) {
    const DistId item = best[i];
    std::uint32_t j = i;
    while (j > 0 && best[j - 1].dist > item.dist) {
      best[j] = best[j - 1];
      --j;
    }
    best[j] = item;
  }
  const std::uint32_t keep = best_count < cap ? best_count : cap;
  nodes_[from].link_count[level] = keep;
  for (std::uint32_t i = 0; i < cap; ++i) {
    set_neighbor(from, level, i, i < keep ? best[i].id : kNilNode);
  }
}

void HnswIndex::mark(std::uint32_t id) const noexcept {
  stamp_[id] = epoch_;
}

bool HnswIndex::seen(std::uint32_t id) const noexcept {
  return stamp_[id] == epoch_;
}

std::uint32_t HnswIndex::search_layer(const float* query, std::uint32_t entry, std::uint32_t ef,
                                      std::uint32_t level, std::uint32_t* found, float* found_dist,
                                      std::uint32_t found_cap) const noexcept {
  if (entry == kNilNode || ef == 0 || found_cap == 0) {
    return 0;
  }
  ++epoch_;
  if (epoch_ == 0) {
    std::memset(stamp_, 0, static_cast<std::size_t>(max_nodes_) * sizeof(std::uint32_t));
    epoch_ = 1;
  }
  DistId candidates[128];
  DistId best[128];
  std::uint32_t candidate_count = 0;
  std::uint32_t best_count = 0;
  const std::uint32_t limit = ef < 128u ? ef : 128u;
  const DistId first{distance_to(query, entry), entry};
  heap_push(candidates, candidate_count, first, true);
  heap_push(best, best_count, first, false);
  mark(entry);

  while (candidate_count > 0) {
    const DistId current = heap_pop(candidates, candidate_count, true);
    if (best_count >= limit && current.dist > best[0].dist) {
      break;
    }
    const std::uint32_t degree = nodes_[current.id].link_count[level];
    for (std::uint32_t i = 0; i < degree; ++i) {
      const std::uint32_t other = neighbor_at(current.id, level, i);
      if (other == kNilNode || seen(other)) {
        continue;
      }
      mark(other);
      const float dist = distance_to(query, other);
      if ((best_count < limit || dist < best[0].dist) && candidate_count < 128u && best_count < 128u) {
        heap_push(candidates, candidate_count, DistId{dist, other}, true);
        heap_push(best, best_count, DistId{dist, other}, false);
        if (best_count > limit) {
          heap_pop(best, best_count, false);
        }
      }
    }
  }

  DistId ordered[128];
  const std::uint32_t have = best_count;
  for (std::uint32_t i = 0; i < have; ++i) {
    ordered[i] = heap_pop(best, best_count, false);
  }
  // heap_pop on a max-heap yields furthest first. Reverse into nearest first.
  std::uint32_t written = 0;
  for (std::uint32_t i = 0; i < have && written < found_cap; ++i) {
    const DistId item = ordered[have - 1u - i];
    found[written] = item.id;
    found_dist[written] = item.dist;
    ++written;
  }
  return written;
}

std::uint32_t HnswIndex::insert(std::span<const float> vector, std::uint64_t payload) noexcept {
  if (!ok() || vector.size() != dimension_ || count_ >= max_nodes_) {
    return kNilNode;
  }
  const std::uint32_t id = count_;
  float* dest = vector_at(id);
  std::memcpy(dest, vector.data(), dimension_ * sizeof(float));
  const float squared = dot_product(dest, dest, dimension_);
  if (squared > 0.f) {
    const float inv = 1.f / std::sqrt(squared);
    scale_inplace(std::span<float>(dest, dimension_), inv);
  }
  const std::uint32_t level = count_ == 0 ? 0 : random_level();
  nodes_[id].level = level;
  payloads_[id] = payload;
  ++count_;
  if (entry_ == kNilNode) {
    entry_ = id;
    entry_level_ = level;
    return id;
  }

  const float* query = dest;
  std::uint32_t ep = entry_;
  const std::uint32_t top = entry_level_ > level ? entry_level_ : level;
  for (std::uint32_t lc = top; lc > level; --lc) {
    bool moved = true;
    while (moved) {
      moved = false;
      const std::uint32_t degree = nodes_[ep].link_count[lc];
      float best = distance_to(query, ep);
      for (std::uint32_t i = 0; i < degree; ++i) {
        const std::uint32_t other = neighbor_at(ep, lc, i);
        if (other == kNilNode) {
          continue;
        }
        const float dist = distance_to(query, other);
        if (dist < best) {
          best = dist;
          ep = other;
          moved = true;
        }
      }
    }
  }

  std::uint32_t found[128];
  float found_dist[128];
  for (std::uint32_t lc = level + 1u; lc-- > 0;) {
    const std::uint32_t found_count =
        search_layer(query, ep, ef_construction_, lc, found, found_dist, ef_construction_);
    const std::uint32_t cap = neighbor_cap(lc);
    const std::uint32_t take = found_count < cap ? found_count : cap;
    for (std::uint32_t i = 0; i < take; ++i) {
      if (found[i] == id) {
        continue;
      }
      link(id, found[i], lc);
      link(found[i], id, lc);
    }
    if (found_count > 0) {
      ep = found[0];
    }
  }
  if (level > entry_level_) {
    entry_ = id;
    entry_level_ = level;
  }
  return id;
}

std::size_t HnswIndex::search(std::span<const float> query, std::span<HnswHit> out) const noexcept {
  if (!ok() || count_ == 0 || out.empty() || query.size() != dimension_ || entry_ == kNilNode) {
    return 0;
  }
  alignas(32) float normalized[kHnswMaxDimension];
  std::memcpy(normalized, query.data(), dimension_ * sizeof(float));
  const float squared = dot_product(normalized, normalized, dimension_);
  if (squared > 0.f) {
    const float inv = 1.f / std::sqrt(squared);
    scale_inplace(std::span<float>(normalized, dimension_), inv);
  }
  std::uint32_t ep = entry_;
  for (std::uint32_t lc = entry_level_; lc > 0; --lc) {
    bool moved = true;
    while (moved) {
      moved = false;
      float best = distance_to(normalized, ep);
      const std::uint32_t degree = nodes_[ep].link_count[lc];
      for (std::uint32_t i = 0; i < degree; ++i) {
        const std::uint32_t other = neighbor_at(ep, lc, i);
        if (other == kNilNode) {
          continue;
        }
        const float dist = distance_to(normalized, other);
        if (dist < best) {
          best = dist;
          ep = other;
          moved = true;
        }
      }
    }
  }
  std::uint32_t found[128];
  float found_dist[128];
  const std::uint32_t found_count =
      search_layer(normalized, ep, ef_search_, 0, found, found_dist, static_cast<std::uint32_t>(out.size()));
  const std::uint32_t limit = found_count < out.size() ? found_count : static_cast<std::uint32_t>(out.size());
  for (std::uint32_t i = 0; i < limit; ++i) {
    out[i].id = found[i];
    out[i].score = 1.f - found_dist[i];
    out[i].payload = payloads_[found[i]];
  }
  return limit;
}

}  // namespace kestrel

#include "kestrel/core_mail.hpp"

#include <cstring>
#include <new>

namespace kestrel {
namespace {

struct MailHeader {
  std::uint64_t key;
  std::uint32_t kind;
  std::uint32_t conn;
  std::uint32_t epoch;
  std::uint32_t dim;
  std::uint32_t text_len;
  std::uint32_t response_len;
  std::uint32_t model_len;
};

constexpr std::size_t kHeaderBytes = sizeof(MailHeader);

}  // namespace

struct CoreMail::Edge {
  SpscRing<std::uint32_t, 2> data;
  SpscRing<std::uint32_t, 2> ack;
  std::byte* block[2] = {};
  std::uint8_t free_n = 2;
  std::uint8_t free_id[2] = {0, 1};
};

CoreMail::CoreMail(std::uint32_t cores, std::uint32_t dimension) noexcept : cores_(cores), dimension_(dimension) {
  if (cores_ < 2) {
    ready_ = true;
    return;
  }
  if (dimension_ == 0 || dimension_ > 1024u) {
    cores_ = 0;
    return;
  }
  const std::size_t bytes = static_cast<std::size_t>(cores_) * cores_;
  edges_ = new (std::nothrow) Edge*[bytes];
  if (edges_ == nullptr) {
    return;
  }
  for (std::size_t i = 0; i < bytes; ++i) {
    edges_[i] = nullptr;
  }
  const std::size_t block = kHeaderBytes + static_cast<std::size_t>(dimension_) * sizeof(float) +
                            static_cast<std::size_t>(kMailTextCap) * 2u + kMailModelCap;
  for (std::uint32_t from = 0; from < cores_; ++from) {
    for (std::uint32_t to = 0; to < cores_; ++to) {
      if (from == to) {
        continue;
      }
      Edge* edge = new (std::nothrow) Edge;
      const std::size_t index = static_cast<std::size_t>(from) * cores_ + to;
      edges_[index] = edge;
      if (edge == nullptr) {
        return;
      }
      edge->block[0] = new (std::nothrow) std::byte[block];
      edge->block[1] = new (std::nothrow) std::byte[block];
      if (edge->block[0] == nullptr || edge->block[1] == nullptr) {
        return;
      }
    }
  }
  ready_ = true;
}

CoreMail::~CoreMail() {
  if (edges_ == nullptr) {
    return;
  }
  const std::size_t bytes = static_cast<std::size_t>(cores_) * cores_;
  for (std::size_t i = 0; i < bytes; ++i) {
    Edge* edge = edges_[i];
    if (edge == nullptr) {
      continue;
    }
    delete[] edge->block[0];
    delete[] edge->block[1];
    delete edge;
  }
  delete[] edges_;
  edges_ = nullptr;
}

CoreMail::Edge* CoreMail::edge(std::uint32_t from, std::uint32_t to) noexcept {
  if (!ready_ || edges_ == nullptr || from >= cores_ || to >= cores_ || from == to) {
    return nullptr;
  }
  return edges_[static_cast<std::size_t>(from) * cores_ + to];
}

bool CoreMail::publish(std::uint32_t from, std::uint32_t to, const MailView& view) noexcept {
  Edge* slot = edge(from, to);
  if (slot == nullptr) {
    return false;
  }
  if (view.text.size() > kMailTextCap || view.response.size() > kMailTextCap || view.model.size() >= kMailModelCap) {
    return false;
  }
  if ((view.kind == MailKind::Replica || view.kind == MailKind::Hit) && view.vector.size() != dimension_) {
    return false;
  }
  std::uint32_t ack = 0;
  while (slot->ack.try_pop(ack)) {
    if (slot->free_n < 2 && ack < 2u) {
      slot->free_id[slot->free_n] = static_cast<std::uint8_t>(ack);
      ++slot->free_n;
    }
  }
  if (slot->free_n == 0) {
    return false;
  }
  --slot->free_n;
  const std::uint8_t id = slot->free_id[slot->free_n];
  std::byte* block = slot->block[id];
  MailHeader header;
  header.key = view.key;
  header.kind = static_cast<std::uint32_t>(view.kind);
  header.conn = view.conn;
  header.epoch = view.epoch;
  header.dim = static_cast<std::uint32_t>(view.vector.size());
  header.text_len = static_cast<std::uint32_t>(view.text.size());
  header.response_len = static_cast<std::uint32_t>(view.response.size());
  header.model_len = static_cast<std::uint32_t>(view.model.size());
  std::memcpy(block, &header, sizeof(header));
  std::byte* cursor = block + kHeaderBytes;
  if (!view.vector.empty()) {
    std::memcpy(cursor, view.vector.data(), view.vector.size() * sizeof(float));
  }
  cursor += static_cast<std::size_t>(dimension_) * sizeof(float);
  if (!view.text.empty()) {
    std::memcpy(cursor, view.text.data(), view.text.size());
  }
  cursor += kMailTextCap;
  if (!view.response.empty()) {
    std::memcpy(cursor, view.response.data(), view.response.size());
  }
  cursor += kMailTextCap;
  if (!view.model.empty()) {
    std::memcpy(cursor, view.model.data(), view.model.size());
  }
  if (!slot->data.try_push(static_cast<std::uint32_t>(id))) {
    slot->free_id[slot->free_n] = id;
    ++slot->free_n;
    return false;
  }
  return true;
}

bool CoreMail::take(std::uint32_t to, std::uint32_t from, MailView& out, float* vector, char* text, char* response,
                    char* model) noexcept {
  Edge* slot = edge(from, to);
  if (slot == nullptr || vector == nullptr || text == nullptr || response == nullptr || model == nullptr) {
    return false;
  }
  std::uint32_t id = 0;
  if (!slot->data.try_pop(id) || id > 1u) {
    return false;
  }
  const std::byte* block = slot->block[id];
  MailHeader header;
  std::memcpy(&header, block, sizeof(header));
  if (header.dim > dimension_ || header.text_len > kMailTextCap || header.response_len > kMailTextCap ||
      header.model_len >= kMailModelCap) {
    (void)slot->ack.try_push(id);
    return false;
  }
  const std::byte* cursor = block + kHeaderBytes;
  if (header.dim > 0) {
    std::memcpy(vector, cursor, static_cast<std::size_t>(header.dim) * sizeof(float));
  }
  cursor += static_cast<std::size_t>(dimension_) * sizeof(float);
  if (header.text_len > 0) {
    std::memcpy(text, cursor, header.text_len);
  }
  cursor += kMailTextCap;
  if (header.response_len > 0) {
    std::memcpy(response, cursor, header.response_len);
  }
  cursor += kMailTextCap;
  if (header.model_len > 0) {
    std::memcpy(model, cursor, header.model_len);
  }
  model[header.model_len] = '\0';
  out.kind = static_cast<MailKind>(header.kind);
  out.key = header.key;
  out.conn = header.conn;
  out.epoch = header.epoch;
  out.vector = std::span<const float>(vector, header.dim);
  out.text = std::string_view(text, header.text_len);
  out.response = std::string_view(response, header.response_len);
  out.model = std::string_view(model, header.model_len);
  if (!slot->ack.try_push(id)) {
    return false;
  }
  return true;
}

}  // namespace kestrel

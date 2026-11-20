#include "kestrel/core_mail.hpp"
#include "kestrel/cluster_wire.hpp"
#include "kestrel/frame.hpp"
#include "check.hpp"

#include <atomic>
#include <cstring>
#include <thread>

int main() {
  kestrel::WireIdentity id;
  id.endpoint = {0x7f000001u, 9000};
  id.incarnation = 0x0102030405060708ull;
  std::byte raw[64]{};
  std::size_t n = 0;
  CHECK(kestrel::encode_identity(id, raw, n));
  CHECK(n == 16);
  kestrel::WireIdentity back;
  CHECK(kestrel::decode_identity(std::span<const std::byte>(raw, n), back));
  CHECK(back.endpoint == id.endpoint);
  CHECK(back.incarnation == id.incarnation);

  const float vector[8] = {1.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
  CHECK(kestrel::encode_cache_payload(vector, "ping", "pong", raw, n));
  kestrel::CacheView cache;
  CHECK(kestrel::decode_cache_payload(std::span<const std::byte>(raw, n), cache));
  CHECK(cache.text == "ping");
  CHECK(cache.response == "pong");
  CHECK(cache.vector.size() == 8);
  CHECK(cache.vector[0] == 1.f);

  kestrel::FrameFields fields;
  fields.type = kestrel::FrameType::Fetch;
  fields.stream_id = 9;
  std::byte frame[128]{};
  std::byte payload[32]{};
  std::size_t payload_n = 0;
  kestrel::FetchView fetch;
  fetch.model = "m";
  fetch.text = "ping";
  CHECK(kestrel::encode_fetch(fetch, payload, payload_n));
  CHECK(kestrel::encode_frame(frame, fields, std::span<const std::byte>(payload, payload_n)));
  kestrel::DecodedFrame decoded;
  CHECK(kestrel::decode_frame(frame, decoded) == kestrel::DecodeStatus::Ok);
  CHECK(decoded.type == kestrel::FrameType::Fetch);
  CHECK(decoded.stream_id == 9);

  kestrel::CoreMail mail(2, 8);
  CHECK(mail.ok());
  alignas(32) float sent[8] = {0, 1, 0, 0, 0, 0, 0, 0};
  kestrel::MailView view;
  view.kind = kestrel::MailKind::Replica;
  view.key = 42;
  view.conn = 3;
  view.epoch = 9;
  view.vector = sent;
  view.text = "ping";
  view.response = "pong";
  view.model = "m";
  CHECK(mail.publish(0, 1, view));

  alignas(32) float got_vec[8]{};
  char text[64]{};
  char response[64]{};
  char model[64]{};
  kestrel::MailView got;
  CHECK(mail.take(1, 0, got, got_vec, text, response, model));
  CHECK(got.kind == kestrel::MailKind::Replica);
  CHECK(got.key == 42);
  CHECK(got.conn == 3);
  CHECK(got.epoch == 9);
  CHECK(got.text == "ping");
  CHECK(got.response == "pong");
  CHECK(got.model == "m");
  CHECK(got.vector[1] == 1.f);
  CHECK(!mail.take(1, 0, got, got_vec, text, response, model));

  std::atomic<bool> seen{false};
  std::thread producer([&] {
    kestrel::MailView again = view;
    again.key = 7;
    again.text = "again";
    while (!mail.publish(1, 0, again)) {
    }
  });
  std::thread consumer([&] {
    kestrel::MailView in;
    while (!mail.take(0, 1, in, got_vec, text, response, model)) {
    }
    seen.store(in.key == 7 && in.text == "again", std::memory_order_release);
  });
  producer.join();
  consumer.join();
  CHECK(seen.load(std::memory_order_acquire));
  return kestrel::test::failures.load() == 0 ? 0 : 1;
}

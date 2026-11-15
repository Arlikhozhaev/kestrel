// Reactor half of Phase 2. Included inside server.cpp's anonymous namespace.

constexpr int kMaxPeers = 32;
constexpr std::size_t kPeerBuf = 64u * 1024u;
constexpr std::uint64_t kFetchWaitMs = 1000;
constexpr int kMaxWaiters = 16;

struct PeerConn {
  int fd = -1;
  MemberEndpoint endpoint{};
  bool hello = false;
  bool connecting = false;
  bool connect_on = false;
  bool recv_on = false;
  bool send_on = false;
  sockaddr_in connect_addr{};
  Circuit circuit{};
  std::byte* in = nullptr;
  std::byte* outb = nullptr;
  std::size_t in_len = 0;
  std::size_t out_len = 0;
  std::size_t out_sent = 0;
  Watch watch{};
  Op recv_op{};
  Op send_op{};
  Op connect_op{};
};

struct Waiter {
  bool used = false;
  bool remote = false;
  bool filling = false;
  int peer = -1;
  int core = 0;
  std::uint32_t conn = 0;
  std::uint32_t epoch = 0;
  std::uint64_t rpc = 0;
  std::uint64_t key = 0;
  std::uint64_t deadline = 0;
  std::uint8_t asked = 0;
  MemberEndpoint owners[kReplicaCount]{};
  std::uint32_t owner_n = 0;
};

struct Cluster {
  HashRing ring;
  Swim swim;
  PeerConn peers[kMaxPeers]{};
  MemberEndpoint dial[kMaxPeers]{};
  int dial_n = 0;
  int listen_fd = -1;
  Watch listen_watch{};
  Op accept_op{};
  Op wake_op{};
  bool accept_on = false;
  bool wake_on = false;
  sockaddr_storage accept_addr{};
  socklen_t accept_len = 0;
  std::uint64_t wake_buf = 0;
  std::uint64_t next_rpc = 1;
  MemberEndpoint self{};
  Waiter waiters[kMaxWaiters]{};
};

[[nodiscard]] Cluster* cluster_of(Server::Impl* impl) noexcept {
  return static_cast<Cluster*>(impl->cluster_node);
}

std::uint64_t mono_ms() noexcept {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

void wake_core(Server::Impl* impl, std::uint32_t id) noexcept {
  if (impl == nullptr || id >= 64u || impl->wake_fd[id] < 0) {
    return;
  }
  const std::uint64_t one = 1;
  if (::write(impl->wake_fd[id], &one, sizeof(one)) < 0) {
    // EAGAIN: a previous wake is still unread. The record is already in the ring.
  }
}

void publish_replica(Core& core, std::span<const float> vector, std::string_view text,
                     std::string_view response) noexcept {
  CoreMail* mail = core.impl->mail;
  if (mail == nullptr || !mail->ok()) {
    return;
  }
  MailView view;
  view.kind = MailKind::Replica;
  view.key = HashRing::key_of(text);
  view.vector = vector;
  view.text = text;
  view.response = response;
  for (std::uint32_t to = 0; to < mail->cores(); ++to) {
    if (to == static_cast<std::uint32_t>(core.id)) {
      continue;
    }
    if (mail->publish(static_cast<std::uint32_t>(core.id), to, view)) {
      wake_core(core.impl, to);
    }
  }
}

bool store_record(Core& core, std::span<const float> vector, std::string_view text, std::string_view response,
                  bool broadcast) noexcept {
  if (!core.wal.ok() || !core.index.ok() || vector.size() != core.index.dimension() || text.empty() ||
      response.empty() || text.size() > kMaxText || response.size() > kMaxText) {
    return false;
  }
  HnswHit hit;
  if (core.index.search(vector, std::span<HnswHit>(&hit, 1)) == 1 && hit.score >= core.impl->config.hit_threshold) {
    WalRecord record;
    if (core.wal.read(hit.payload, record) && record.text == text) {
      return true;
    }
  }
  const std::uint64_t offset = core.wal.append(vector, text, response);
  if (offset == UINT64_MAX) {
    return false;
  }
  if (core.index.insert(vector, offset) == kNilNode) {
    return false;
  }
  if (broadcast) {
    publish_replica(core, vector, text, response);
  }
  return true;
}

void peer_reclaim(PeerConn& peer) noexcept {
  if (peer.send_on || peer.out_sent == 0) {
    return;
  }
  const std::size_t rest = peer.out_len - peer.out_sent;
  if (rest > 0 && peer.outb != nullptr) {
    std::memmove(peer.outb, peer.outb + peer.out_sent, rest);
  }
  peer.out_len = rest;
  peer.out_sent = 0;
}

void peer_arm(Core& core, PeerConn& peer) noexcept {
  if (core.ring_ready || core.ep < 0 || peer.fd < 0) {
    return;
  }
  std::uint32_t events = EPOLLIN;
  if (peer.connecting || peer.out_len > peer.out_sent) {
    events |= EPOLLOUT;
  }
  arm_fd(core.ep, peer.fd, peer.watch, events);
}

[[nodiscard]] bool peer_queue(Core& core, PeerConn& peer, FrameType type, std::uint64_t stream,
                             std::span<const std::byte> payload) noexcept {
  if (peer.fd < 0 || peer.outb == nullptr) {
    return false;
  }
  peer_reclaim(peer);
  const std::size_t need = kFrameHeaderBytes + payload.size();
  if (peer.out_len + need > kPeerBuf) {
    return false;
  }
  FrameFields fields;
  fields.type = type;
  fields.stream_id = stream;
  std::byte* dest = peer.outb + peer.out_len;
  if (!encode_frame(std::span<std::byte>(dest, need), fields, payload)) {
    return false;
  }
  peer.out_len += need;
  peer_arm(core, peer);
  return true;
}

[[nodiscard]] bool peer_send_hello(Core& core, Cluster& cluster, PeerConn& peer) noexcept {
  std::byte payload[16];
  std::size_t n = 0;
  WireIdentity id;
  id.endpoint = cluster.self;
  id.incarnation = cluster.swim.incarnation();
  if (!encode_identity(id, payload, n)) {
    return false;
  }
  return peer_queue(core, peer, FrameType::Hello, 0, std::span<const std::byte>(payload, n));
}

void cluster_rebuild(Core& core, Cluster& cluster) noexcept {
  MemberEndpoint alive[kMaxPeers];
  const std::uint32_t n = cluster.swim.alive(alive, kMaxPeers);
  if (n == 0 || !cluster.ring.set_members(std::span<const MemberEndpoint>(alive, n))) {
    return;
  }
  int ready = 0;
  for (int i = 0; i < kMaxPeers; ++i) {
    if (cluster.peers[i].fd >= 0 && cluster.peers[i].hello) {
      ++ready;
    }
  }
  core.impl->members.store(ready + 1, std::memory_order_release);
}

void peer_close(Core& core, Cluster& cluster, PeerConn& peer) noexcept {
  if (peer.fd >= 0) {
    arm_fd(core.ep, peer.fd, peer.watch, 0);
    ::close(peer.fd);
    peer.fd = -1;
  }
  peer.hello = false;
  peer.connecting = false;
  peer.connect_on = false;
  peer.recv_on = false;
  peer.send_on = false;
  peer.in_len = 0;
  peer.out_len = 0;
  peer.out_sent = 0;
  peer.circuit.fail(mono_ms());
  cluster_rebuild(core, cluster);
}

[[nodiscard]] PeerConn* peer_find(Cluster& cluster, MemberEndpoint endpoint) noexcept {
  for (int i = 0; i < kMaxPeers; ++i) {
    if (cluster.peers[i].fd >= 0 && cluster.peers[i].hello && cluster.peers[i].endpoint == endpoint) {
      return &cluster.peers[i];
    }
  }
  return nullptr;
}

[[nodiscard]] PeerConn* peer_slot(Cluster& cluster) noexcept {
  for (int i = 0; i < kMaxPeers; ++i) {
    if (cluster.peers[i].fd < 0 && !cluster.peers[i].connecting) {
      return &cluster.peers[i];
    }
  }
  return nullptr;
}

[[nodiscard]] int peer_index(Cluster& cluster, const PeerConn& peer) noexcept {
  return static_cast<int>(&peer - cluster.peers);
}

void peer_attach(Cluster& cluster, PeerConn& peer, int fd) noexcept {
  const auto index = static_cast<std::uint16_t>(peer_index(cluster, peer));
  peer.fd = fd;
  peer.hello = false;
  peer.connecting = false;
  peer.watch.kind = kWatchPeer;
  peer.watch.slot = index;
  peer.watch.conn = nullptr;
  peer.recv_op.kind = Op::PeerRecv;
  peer.recv_op.slot = index;
  peer.recv_op.conn = nullptr;
  peer.send_op.kind = Op::PeerSend;
  peer.send_op.slot = index;
  peer.send_op.conn = nullptr;
  peer.connect_op.kind = Op::PeerConnect;
  peer.connect_op.slot = index;
  peer.connect_op.conn = nullptr;
}

void cluster_emit(Core& core, Cluster& cluster, const SwimAction& action) noexcept {
  std::byte payload[32];
  std::size_t n = 0;
  if (action.kind == SwimActionKind::Member) {
    WireIdentity id;
    id.endpoint = action.about;
    id.incarnation = action.incarnation;
    if (!encode_member(id, static_cast<std::uint16_t>(action.status), payload, n)) {
      return;
    }
    for (int i = 0; i < kMaxPeers; ++i) {
      if (cluster.peers[i].hello) {
        (void)peer_queue(core, cluster.peers[i], FrameType::Member, 0, std::span<const std::byte>(payload, n));
      }
    }
    cluster_rebuild(core, cluster);
    return;
  }
  PeerConn* peer = peer_find(cluster, action.to);
  if (peer == nullptr) {
    return;
  }
  if (action.kind == SwimActionKind::Ping || action.kind == SwimActionKind::PingAck) {
    WireIdentity id;
    id.endpoint = action.about;
    id.incarnation = action.incarnation;
    if (!encode_identity(id, payload, n)) {
      return;
    }
    const FrameType type = action.kind == SwimActionKind::Ping ? FrameType::Ping : FrameType::PingAck;
    (void)peer_queue(core, *peer, type, 0, std::span<const std::byte>(payload, n));
    return;
  }
  if (action.kind == SwimActionKind::PingReq) {
    WireIdentity from;
    from.endpoint = cluster.self;
    from.incarnation = cluster.swim.incarnation();
    WireIdentity target;
    target.endpoint = action.about;
    target.incarnation = action.incarnation;
    if (!encode_pingreq(from, target, payload, n)) {
      return;
    }
    (void)peer_queue(core, *peer, FrameType::PingReq, 0, std::span<const std::byte>(payload, n));
  }
}

[[nodiscard]] Waiter* waiter_alloc(Cluster& cluster) noexcept {
  for (int i = 0; i < kMaxWaiters; ++i) {
    if (!cluster.waiters[i].used) {
      cluster.waiters[i] = Waiter{};
      cluster.waiters[i].used = true;
      return &cluster.waiters[i];
    }
  }
  return nullptr;
}

void waiter_clear(Waiter& waiter) noexcept { waiter = Waiter{}; }

[[nodiscard]] bool serve_text(Core& core, Conn& conn, std::string_view response) {
  const std::string_view model =
      conn.model_len == 0 ? std::string_view("kestrel") : std::string_view(conn.model, conn.model_len);
  const std::size_t n = conn.client_stream
                            ? write_chat_sse(model, response, std::span<std::byte>(conn.outb, kBufBytes))
                            : write_chat_json(model, response, std::span<std::byte>(conn.outb, kBufBytes));
  if (n == 0) {
    return false;
  }
  queue_out(conn, n, conn.keep_alive);
  rearm(core, conn);
  return true;
}

[[nodiscard]] bool lookup_copy(Core& core, std::string_view prompt, char* dest, std::size_t cap, std::size_t& out,
                               float* vector_out) noexcept {
  alignas(32) float vec[kHnswMaxDimension];
  const std::size_t dim = core.index.dimension();
  if (vector_out == nullptr || !core.embed.embed(prompt, std::span<float>(vec, dim))) {
    return false;
  }
  HnswHit hit;
  if (core.index.search(std::span<const float>(vec, dim), std::span<HnswHit>(&hit, 1)) != 1 ||
      hit.score < core.impl->config.hit_threshold) {
    return false;
  }
  WalRecord record;
  if (!core.wal.read(hit.payload, record) || record.text != prompt || record.response.size() > cap ||
      record.vector.size() != dim) {
    return false;
  }
  std::memcpy(vector_out, record.vector.data(), dim * sizeof(float));
  std::memcpy(dest, record.response.data(), record.response.size());
  out = record.response.size();
  return true;
}

void mail_result(Core& core, int to, MailKind kind, std::uint32_t conn, std::uint64_t key, std::string_view text,
                 std::string_view response, std::span<const float> vector) noexcept {
  if (core.impl->mail == nullptr || to == core.id) {
    return;
  }
  MailView view;
  view.kind = kind;
  view.conn = conn;
  view.key = key;
  view.text = text;
  view.response = response;
  view.vector = vector;
  if (core.impl->mail->publish(static_cast<std::uint32_t>(core.id), static_cast<std::uint32_t>(to), view)) {
    wake_core(core.impl, static_cast<std::uint32_t>(to));
  }
}

bool start_internal_fill(Core& core, Waiter& waiter, std::string_view prompt, std::string_view model);

void complete_fill_waiters(Core& core, Cluster& cluster, std::uint64_t key, std::span<const float> vector,
                           std::string_view text, std::string_view response) noexcept {
  for (int i = 0; i < kMaxWaiters; ++i) {
    Waiter& waiter = cluster.waiters[i];
    if (!waiter.used || !waiter.filling || waiter.key != key) {
      continue;
    }
    if (waiter.remote) {
      if (waiter.peer >= 0 && waiter.peer < kMaxPeers) {
        std::byte payload[kPeerBuf];
        std::size_t n = 0;
        if (encode_cache_payload(vector, text, response, payload, n)) {
          (void)peer_queue(core, cluster.peers[waiter.peer], FrameType::CacheEntry, waiter.rpc,
                           std::span<const std::byte>(payload, n));
        }
      }
    } else if (waiter.core == core.id) {
      if (waiter.conn < core.conn_cap) {
        Conn& conn = core.conns[waiter.conn];
        if (conn.phase == Phase::AwaitCluster && conn.epoch == waiter.epoch) {
          if (!serve_text(core, conn, response)) {
            send_status(core, conn, 502, "cache response too large");
          }
        }
      }
    } else {
      mail_result(core, waiter.core, MailKind::Hit, waiter.conn, key, text, response, vector);
    }
    waiter_clear(waiter);
  }
}

void cluster_note_stored(Core& core, std::span<const float> vector, std::string_view text,
                         std::string_view response) noexcept {
  Cluster* cluster = cluster_of(core.impl);
  if (cluster == nullptr || core.id != 0) {
    return;
  }
  const std::uint64_t key = HashRing::key_of(text);
  MemberEndpoint owners[kReplicaCount];
  const std::uint32_t n = cluster->ring.owners(key, owners);
  for (std::uint32_t i = 0; i < n; ++i) {
    if (owners[i] == cluster->self) {
      continue;
    }
    PeerConn* peer = peer_find(*cluster, owners[i]);
    if (peer == nullptr) {
      continue;
    }
    std::byte payload[kPeerBuf];
    std::size_t bytes = 0;
    if (encode_cache_payload(vector, text, response, payload, bytes)) {
      (void)peer_queue(core, *peer, FrameType::CacheEntry, 0, std::span<const std::byte>(payload, bytes));
    }
  }
  complete_fill_waiters(core, *cluster, key, vector, text, response);
}

void cluster_fill_failed(Core& core, Conn& conn) noexcept {
  Cluster* cluster = cluster_of(core.impl);
  if (cluster == nullptr) {
    return;
  }
  for (int i = 0; i < kMaxWaiters; ++i) {
    Waiter& waiter = cluster->waiters[i];
    if (!waiter.used || !waiter.filling || waiter.key != conn.key) {
      continue;
    }
    if (waiter.remote && waiter.peer >= 0 && waiter.peer < kMaxPeers) {
      std::byte payload[8];
      std::size_t n = 0;
      if (encode_key(waiter.key, payload, n)) {
        (void)peer_queue(core, cluster->peers[waiter.peer], FrameType::FetchFail, waiter.rpc,
                         std::span<const std::byte>(payload, n));
      }
    } else if (waiter.core == core.id && waiter.conn < core.conn_cap) {
      Conn& client = core.conns[waiter.conn];
      if (client.phase == Phase::AwaitCluster && client.epoch == waiter.epoch) {
        send_status(core, client, 502, "upstream failed");
        rearm(core, client);
      }
    } else if (!waiter.remote) {
      mail_result(core, waiter.core, MailKind::Fail, waiter.conn, waiter.key, {}, {}, {});
    }
    waiter_clear(waiter);
  }
}

[[nodiscard]] bool peer_usable(Cluster& cluster, MemberEndpoint endpoint, std::uint64_t now) noexcept {
  PeerConn* peer = peer_find(cluster, endpoint);
  if (peer == nullptr) {
    return false;
  }
  return !(peer->circuit.state == Circuit::State::Open && now < peer->circuit.open_until_ms);
}

[[nodiscard]] bool send_fetch(Core& core, Cluster& cluster, PeerConn& peer, Waiter& waiter, std::string_view model,
                             std::string_view text) noexcept {
  std::byte payload[kMaxText + 80];
  std::size_t n = 0;
  FetchView view;
  view.model = model;
  view.text = text;
  if (!encode_fetch(view, payload, n)) {
    return false;
  }
  waiter.rpc = cluster.next_rpc++;
  if (cluster.next_rpc == 0) {
    cluster.next_rpc = 1;
  }
  waiter.deadline = mono_ms() + kFetchWaitMs;
  return peer_queue(core, peer, FrameType::Fetch, waiter.rpc, std::span<const std::byte>(payload, n));
}

bool start_internal_fill(Core& core, Waiter& waiter, std::string_view prompt, std::string_view model) {
  Conn* conn = acquire(core);
  if (conn == nullptr) {
    return false;
  }
  conn->internal = true;
  conn->fd = -1;
  conn->have_prompt = true;
  conn->cacheable = true;
  conn->key = waiter.key;
  conn->prompt_len = prompt.size() < sizeof(conn->prompt) ? prompt.size() : sizeof(conn->prompt) - 1;
  std::memcpy(conn->prompt, prompt.data(), conn->prompt_len);
  conn->model_len = model.size() < sizeof(conn->model) ? model.size() : sizeof(conn->model) - 1;
  std::memcpy(conn->model, model.data(), conn->model_len);
  conn->model[conn->model_len] = '\0';
  const std::size_t n = write_upstream_request(std::string_view(conn->model, conn->model_len),
                                              std::string_view(conn->prompt, conn->prompt_len),
                                              std::span<std::byte>(conn->in, kBufBytes));
  if (n == 0) {
    recycle(core, *conn);
    return false;
  }
  conn->in_len = n;
  conn->request_total = n;
  waiter.filling = true;
  begin_proxy(core, *conn);
  return conn->phase != Phase::Free;
}

// Returns true when the local connection should call the model itself.
bool decide_miss(Core& core, Cluster& cluster, int reply_core, std::uint32_t conn_index, std::uint32_t epoch,
                 std::string_view prompt, std::string_view model, Conn* local) {
  const std::uint64_t key = HashRing::key_of(prompt);
  alignas(32) float vec[kHnswMaxDimension];
  char cached[kMaxText];
  std::size_t cached_len = 0;
  if (lookup_copy(core, prompt, cached, sizeof(cached), cached_len, vec)) {
    if (local != nullptr && reply_core == core.id) {
      if (!serve_text(core, *local, std::string_view(cached, cached_len))) {
        send_status(core, *local, 502, "cache response too large");
      }
      return false;
    }
    mail_result(core, reply_core, MailKind::Hit, conn_index, key, prompt, std::string_view(cached, cached_len),
                std::span<const float>(vec, core.index.dimension()));
    return false;
  }
  MemberEndpoint owners[kReplicaCount];
  const std::uint32_t owner_n = cluster.ring.owners(key, owners);
  const bool primary = owner_n > 0 && owners[0] == cluster.self;
  const bool secondary = owner_n > 1 && owners[1] == cluster.self;
  const std::uint64_t now = mono_ms();
  auto fill_self = [&]() -> bool {
    if (local != nullptr && reply_core == core.id) {
      local->key = key;
      return true;
    }
    if (reply_core == core.id) {
      return false;
    }
    mail_result(core, reply_core, MailKind::Fill, conn_index, key, prompt, {}, {});
    return false;
  };
  if (primary || owner_n == 0) {
    return fill_self();
  }
  MemberEndpoint target = owners[0];
  std::uint8_t asked = 1u;
  if (target == cluster.self) {
    target = owner_n > 1 ? owners[1] : cluster.self;
    asked = 2u;
  }
  if (target == cluster.self || !peer_usable(cluster, target, now)) {
    if (secondary || primary) {
      return fill_self();
    }
    if (local != nullptr && reply_core == core.id) {
      send_status(core, *local, 502, "cache owners unreachable");
      return false;
    }
    mail_result(core, reply_core, MailKind::Fail, conn_index, key, {}, {}, {});
    return false;
  }
  Waiter* waiter = waiter_alloc(cluster);
  if (waiter == nullptr) {
    if (local != nullptr && reply_core == core.id) {
      send_status(core, *local, 502, "cluster busy");
    } else {
      mail_result(core, reply_core, MailKind::Fail, conn_index, key, {}, {}, {});
    }
    return false;
  }
  waiter->core = reply_core;
  waiter->conn = conn_index;
  waiter->epoch = epoch;
  waiter->key = key;
  waiter->asked = asked;
  waiter->owner_n = owner_n;
  for (std::uint32_t i = 0; i < owner_n; ++i) {
    waiter->owners[i] = owners[i];
  }
  PeerConn* peer = peer_find(cluster, target);
  if (peer == nullptr || !send_fetch(core, cluster, *peer, *waiter, model, prompt)) {
    waiter_clear(*waiter);
    return fill_self();
  }
  if (local != nullptr && reply_core == core.id) {
    local->phase = Phase::AwaitCluster;
    local->key = key;
  }
  return false;
}

bool cluster_continue_miss(Core& core, Conn& conn) {
  if (!core.impl->config.cluster) {
    return true;
  }
  const std::string_view prompt(conn.prompt, conn.prompt_len);
  const std::string_view model = conn.model_len == 0 ? std::string_view("kestrel")
                                                     : std::string_view(conn.model, conn.model_len);
  conn.key = HashRing::key_of(prompt);
  Cluster* cluster = cluster_of(core.impl);
  if (cluster == nullptr || core.id != 0) {
    MailView view;
    view.kind = MailKind::Resolve;
    view.key = conn.key;
    view.conn = static_cast<std::uint32_t>(&conn - core.conns);
    view.text = prompt;
    view.model = model;
    if (core.impl->mail == nullptr ||
        !core.impl->mail->publish(static_cast<std::uint32_t>(core.id), 0, view)) {
      send_status(core, conn, 502, "cluster busy");
      return false;
    }
    wake_core(core.impl, 0);
    conn.phase = Phase::AwaitCluster;
    return false;
  }
  return decide_miss(core, *cluster, core.id, static_cast<std::uint32_t>(&conn - core.conns), conn.epoch, prompt,
                     model, &conn);
}

void on_fetch(Core& core, Cluster& cluster, PeerConn& peer, const DecodedFrame& frame) noexcept {
  FetchView view;
  if (!decode_fetch(frame.payload, view)) {
    return;
  }
  alignas(32) float vec[kHnswMaxDimension];
  char cached[kMaxText];
  std::size_t cached_len = 0;
  if (lookup_copy(core, view.text, cached, sizeof(cached), cached_len, vec)) {
    std::byte payload[kPeerBuf];
    std::size_t n = 0;
    if (encode_cache_payload(std::span<const float>(vec, core.index.dimension()), view.text,
                            std::string_view(cached, cached_len), payload, n)) {
      (void)peer_queue(core, peer, FrameType::CacheEntry, frame.stream_id, std::span<const std::byte>(payload, n));
      return;
    }
  }
  const std::uint64_t key = HashRing::key_of(view.text);
  MemberEndpoint owners[kReplicaCount];
  const std::uint32_t owner_n = cluster.ring.owners(key, owners);
  const bool primary = owner_n == 0 || (owner_n > 0 && owners[0] == cluster.self);
  const bool secondary = owner_n > 1 && owners[1] == cluster.self;
  const bool primary_up = owner_n > 0 && (owners[0] == cluster.self || peer_usable(cluster, owners[0], mono_ms()));
  if (!(primary || (secondary && !primary_up))) {
    std::byte payload[8];
    std::size_t n = 0;
    if (encode_key(key, payload, n)) {
      (void)peer_queue(core, peer, FrameType::FetchMiss, frame.stream_id, std::span<const std::byte>(payload, n));
    }
    return;
  }
  for (int i = 0; i < kMaxWaiters; ++i) {
    if (cluster.waiters[i].used && cluster.waiters[i].filling && cluster.waiters[i].key == key) {
      Waiter* extra = waiter_alloc(cluster);
      if (extra == nullptr) {
        break;
      }
      extra->remote = true;
      extra->filling = true;
      extra->peer = peer_index(cluster, peer);
      extra->rpc = frame.stream_id;
      extra->key = key;
      extra->deadline = mono_ms() + kFetchWaitMs;
      return;
    }
  }
  Waiter* waiter = waiter_alloc(cluster);
  if (waiter == nullptr) {
    std::byte payload[8];
    std::size_t n = 0;
    if (encode_key(key, payload, n)) {
      (void)peer_queue(core, peer, FrameType::FetchFail, frame.stream_id, std::span<const std::byte>(payload, n));
    }
    return;
  }
  waiter->remote = true;
  waiter->peer = peer_index(cluster, peer);
  waiter->rpc = frame.stream_id;
  waiter->key = key;
  waiter->deadline = mono_ms() + kFetchWaitMs;
  if (!start_internal_fill(core, *waiter, view.text, view.model)) {
    std::byte payload[8];
    std::size_t n = 0;
    if (encode_key(key, payload, n)) {
      (void)peer_queue(core, peer, FrameType::FetchFail, frame.stream_id, std::span<const std::byte>(payload, n));
    }
    waiter_clear(*waiter);
  }
}

void on_cache_entry(Core& core, Cluster& cluster, const DecodedFrame& frame) noexcept {
  CacheView view;
  if (!decode_cache_payload(frame.payload, view)) {
    return;
  }
  alignas(32) float vec[kHnswMaxDimension];
  if (view.vector.size() != core.index.dimension() || view.vector.size() > kHnswMaxDimension) {
    return;
  }
  std::memcpy(vec, view.vector.data(), view.vector.size() * sizeof(float));
  const auto vector = std::span<const float>(vec, view.vector.size());
  if (!store_record(core, vector, view.text, view.response, true)) {
    return;
  }
  if (frame.stream_id == 0) {
    return;
  }
  for (int i = 0; i < kMaxWaiters; ++i) {
    Waiter& waiter = cluster.waiters[i];
    if (!waiter.used || waiter.filling || waiter.rpc != frame.stream_id) {
      continue;
    }
    if (waiter.core == core.id && waiter.conn < core.conn_cap) {
      Conn& conn = core.conns[waiter.conn];
      if (conn.phase == Phase::AwaitCluster && conn.epoch == waiter.epoch) {
        if (!serve_text(core, conn, view.response)) {
          send_status(core, conn, 502, "cache response too large");
        }
      }
    } else if (waiter.core != core.id) {
      mail_result(core, waiter.core, MailKind::Hit, waiter.conn, waiter.key, view.text, view.response, vector);
    }
    waiter_clear(waiter);
  }
}

void on_fetch_miss(Core& core, Cluster& cluster, std::uint64_t rpc) noexcept {
  for (int i = 0; i < kMaxWaiters; ++i) {
    Waiter& waiter = cluster.waiters[i];
    if (!waiter.used || waiter.filling || waiter.rpc != rpc) {
      continue;
    }
    const std::uint64_t now = mono_ms();
    MemberEndpoint next{};
    bool have = false;
    for (std::uint32_t o = 0; o < waiter.owner_n; ++o) {
      const std::uint8_t bit = static_cast<std::uint8_t>(1u << o);
      if ((waiter.asked & bit) != 0 || waiter.owners[o] == cluster.self) {
        continue;
      }
      if (peer_usable(cluster, waiter.owners[o], now)) {
        next = waiter.owners[o];
        waiter.asked = static_cast<std::uint8_t>(waiter.asked | bit);
        have = true;
        break;
      }
    }
    if (have) {
      PeerConn* peer = peer_find(cluster, next);
      std::string_view model;
      std::string_view prompt;
      if (waiter.core == core.id && waiter.conn < core.conn_cap) {
        Conn& conn = core.conns[waiter.conn];
        model = std::string_view(conn.model, conn.model_len);
        prompt = std::string_view(conn.prompt, conn.prompt_len);
      }
      if (peer != nullptr && !prompt.empty() && send_fetch(core, cluster, *peer, waiter, model, prompt)) {
        return;
      }
    }
    bool self_owner = false;
    for (std::uint32_t o = 0; o < waiter.owner_n; ++o) {
      if (waiter.owners[o] == cluster.self) {
        self_owner = true;
      }
    }
    if (self_owner && waiter.core == core.id && waiter.conn < core.conn_cap) {
      Conn& conn = core.conns[waiter.conn];
      if (conn.phase == Phase::AwaitCluster && conn.epoch == waiter.epoch) {
        waiter_clear(waiter);
        begin_proxy(core, conn);
        return;
      }
    }
    if (self_owner && waiter.core != core.id) {
      mail_result(core, waiter.core, MailKind::Fill, waiter.conn, waiter.key, {}, {}, {});
      waiter_clear(waiter);
      return;
    }
    if (waiter.core == core.id && waiter.conn < core.conn_cap) {
      Conn& conn = core.conns[waiter.conn];
      if (conn.phase == Phase::AwaitCluster && conn.epoch == waiter.epoch) {
        send_status(core, conn, 502, "cache miss");
        rearm(core, conn);
      }
    } else if (waiter.core != core.id) {
      mail_result(core, waiter.core, MailKind::Fail, waiter.conn, waiter.key, {}, {}, {});
    }
    waiter_clear(waiter);
  }
}

void on_hello(Core& core, Cluster& cluster, PeerConn& peer, const DecodedFrame& frame) noexcept {
  WireIdentity id;
  if (!decode_identity(frame.payload, id)) {
    peer_close(core, cluster, peer);
    return;
  }
  if (id.endpoint == cluster.self) {
    peer_close(core, cluster, peer);
    return;
  }
  std::uint64_t incarnation = id.incarnation == 0 ? 1 : id.incarnation;
  for (std::uint32_t i = 0; i < cluster.swim.count(); ++i) {
    const SwimMember& member = cluster.swim.member(i);
    if (member.endpoint == id.endpoint && member.status == SwimStatus::Dead && incarnation <= member.incarnation) {
      incarnation = member.incarnation + 1u;
    }
  }
  SwimAction actions[4];
  const std::uint32_t n =
      cluster.swim.observe(id.endpoint, SwimStatus::Alive, incarnation, mono_ms(), actions, 4);
  peer.endpoint = id.endpoint;
  peer.hello = true;
  peer.connecting = false;
  for (std::uint32_t i = 0; i < n; ++i) {
    cluster_emit(core, cluster, actions[i]);
  }
  cluster_rebuild(core, cluster);
}

void on_peer_frame(Core& core, Cluster& cluster, PeerConn& peer, const DecodedFrame& frame) noexcept {
  if (!peer.hello && frame.type != FrameType::Hello) {
    return;
  }
  if (frame.type == FrameType::Hello) {
    on_hello(core, cluster, peer, frame);
    return;
  }
  if (frame.type == FrameType::Ping) {
    WireIdentity id;
    if (!decode_identity(frame.payload, id)) {
      return;
    }
    SwimAction actions[4];
    const std::uint32_t n = cluster.swim.on_ping(id.endpoint, id.incarnation, mono_ms(), actions, 4);
    for (std::uint32_t i = 0; i < n; ++i) {
      cluster_emit(core, cluster, actions[i]);
    }
    return;
  }
  if (frame.type == FrameType::PingAck) {
    WireIdentity id;
    if (!decode_identity(frame.payload, id)) {
      return;
    }
    SwimAction actions[4];
    const std::uint32_t n = cluster.swim.on_ack(id.endpoint, id.incarnation, mono_ms(), actions, 4);
    for (std::uint32_t i = 0; i < n; ++i) {
      cluster_emit(core, cluster, actions[i]);
    }
    cluster_rebuild(core, cluster);
    return;
  }
  if (frame.type == FrameType::PingReq) {
    WireIdentity from;
    WireIdentity target;
    if (!decode_pingreq(frame.payload, from, target)) {
      return;
    }
    SwimAction actions[4];
    const std::uint32_t n =
        cluster.swim.on_pingreq(from.endpoint, target.endpoint, target.incarnation, mono_ms(), actions, 4);
    for (std::uint32_t i = 0; i < n; ++i) {
      cluster_emit(core, cluster, actions[i]);
    }
    return;
  }
  if (frame.type == FrameType::Member) {
    WireIdentity id;
    std::uint16_t status = 0;
    if (!decode_member(frame.payload, id, status) || status < 1 || status > 3) {
      return;
    }
    SwimAction actions[4];
    const std::uint32_t n = cluster.swim.observe(id.endpoint, static_cast<SwimStatus>(status), id.incarnation,
                                                mono_ms(), actions, 4);
    for (std::uint32_t i = 0; i < n; ++i) {
      cluster_emit(core, cluster, actions[i]);
    }
    cluster_rebuild(core, cluster);
    return;
  }
  if (frame.type == FrameType::Fetch) {
    on_fetch(core, cluster, peer, frame);
    return;
  }
  if (frame.type == FrameType::CacheEntry) {
    on_cache_entry(core, cluster, frame);
    return;
  }
  if (frame.type == FrameType::FetchMiss || frame.type == FrameType::FetchFail) {
    on_fetch_miss(core, cluster, frame.stream_id);
  }
}

void peer_parse(Core& core, Cluster& cluster, PeerConn& peer) noexcept {
  std::size_t off = 0;
  while (peer.fd >= 0 && off < peer.in_len) {
    DecodedFrame frame;
    const DecodeStatus status =
        decode_frame(std::span<const std::byte>(peer.in + off, peer.in_len - off), frame);
    if (status == DecodeStatus::NeedMore) {
      if (peer.in_len == kPeerBuf && off == 0) {
        peer_close(core, cluster, peer);
      }
      break;
    }
    if (status != DecodeStatus::Ok) {
      peer_close(core, cluster, peer);
      return;
    }
    const std::size_t total = kFrameHeaderBytes + frame.payload.size();
    on_peer_frame(core, cluster, peer, frame);
    off += total;
  }
  if (peer.fd >= 0 && off > 0 && off <= peer.in_len) {
    const std::size_t rest = peer.in_len - off;
    if (rest > 0) {
      std::memmove(peer.in, peer.in + off, rest);
    }
    peer.in_len = rest;
  }
}

void peer_readable(Core& core, Cluster& cluster, PeerConn& peer) noexcept {
  if (peer.fd < 0 || peer.in == nullptr) {
    return;
  }
  while (peer.in_len < kPeerBuf) {
    const ssize_t n = ::recv(peer.fd, peer.in + peer.in_len, kPeerBuf - peer.in_len, 0);
    if (n > 0) {
      peer.in_len += static_cast<std::size_t>(n);
      continue;
    }
    if (n == 0) {
      peer_close(core, cluster, peer);
      return;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      break;
    }
    peer_close(core, cluster, peer);
    return;
  }
  peer_parse(core, cluster, peer);
}

void peer_writable(Core& core, Cluster& cluster, PeerConn& peer) noexcept {
  if (peer.fd < 0) {
    return;
  }
  if (peer.connecting) {
    int error = 0;
    socklen_t len = sizeof(error);
    if (::getsockopt(peer.fd, SOL_SOCKET, SO_ERROR, &error, &len) != 0 || error != 0) {
      peer_close(core, cluster, peer);
      return;
    }
    peer.connecting = false;
    if (!peer_send_hello(core, cluster, peer)) {
      peer_close(core, cluster, peer);
      return;
    }
  }
  while (peer.out_sent < peer.out_len) {
    const ssize_t n = ::send(peer.fd, peer.outb + peer.out_sent, peer.out_len - peer.out_sent, MSG_NOSIGNAL);
    if (n > 0) {
      peer.out_sent += static_cast<std::size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      break;
    }
    peer_close(core, cluster, peer);
    return;
  }
  peer_reclaim(peer);
  peer_arm(core, peer);
}

void peer_dial(Core& core, Cluster& cluster, MemberEndpoint endpoint) noexcept {
  if (endpoint == cluster.self || peer_find(cluster, endpoint) != nullptr) {
    return;
  }
  for (int i = 0; i < kMaxPeers; ++i) {
    if (cluster.peers[i].fd >= 0 && cluster.peers[i].endpoint == endpoint) {
      return;
    }
  }
  PeerConn* peer = peer_slot(cluster);
  if (peer == nullptr) {
    return;
  }
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return;
  }
  const bool blocking = core.ring_ready;
  if (!(set_nodelay(fd) && (blocking ? set_cloexec(fd) : set_nonblock_cloexec(fd)))) {
    ::close(fd);
    return;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(endpoint.port);
  addr.sin_addr.s_addr = htonl(endpoint.addr);
  peer->endpoint = endpoint;
  peer_attach(cluster, *peer, fd);
  if (blocking) {
    peer->connecting = true;
    return;
  }
  const int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  if (rc == 0) {
    peer->connecting = false;
    if (!peer_send_hello(core, cluster, *peer)) {
      peer_close(core, cluster, *peer);
    }
    return;
  }
  if (errno == EINPROGRESS) {
    peer->connecting = true;
    peer_arm(core, *peer);
    return;
  }
  peer_close(core, cluster, *peer);
}

void peer_accept(Core& core, Cluster& cluster) noexcept {
  if (cluster.listen_fd < 0) {
    return;
  }
  for (int burst = 0; burst < kAcceptBurst; ++burst) {
    cluster.accept_len = sizeof(cluster.accept_addr);
    const int fd = ::accept4(cluster.listen_fd, reinterpret_cast<sockaddr*>(&cluster.accept_addr),
                            &cluster.accept_len, SOCK_CLOEXEC);
    if (fd < 0) {
      return;
    }
    PeerConn* peer = peer_slot(cluster);
    if (peer == nullptr || !set_nodelay(fd)) {
      ::close(fd);
      return;
    }
    if (!core.ring_ready && !set_nonblock_cloexec(fd)) {
      ::close(fd);
      return;
    }
    peer_attach(cluster, *peer, fd);
    if (!peer_send_hello(core, cluster, *peer)) {
      peer_close(core, cluster, *peer);
      continue;
    }
    peer_arm(core, *peer);
  }
}

void cluster_tick(Core& core, Cluster& cluster) noexcept {
  const std::uint64_t now = mono_ms();
  SwimAction actions[8];
  const std::uint32_t n = cluster.swim.tick(now, actions, 8);
  for (std::uint32_t i = 0; i < n; ++i) {
    cluster_emit(core, cluster, actions[i]);
  }
  for (int i = 0; i < kMaxWaiters; ++i) {
    Waiter& waiter = cluster.waiters[i];
    if (!waiter.used || waiter.filling || waiter.deadline == 0 || now < waiter.deadline) {
      continue;
    }
    on_fetch_miss(core, cluster, waiter.rpc);
  }
  for (int i = 0; i < cluster.dial_n; ++i) {
    bool live = false;
    for (int p = 0; p < kMaxPeers; ++p) {
      if (cluster.peers[p].fd >= 0 && cluster.peers[p].endpoint == cluster.dial[i]) {
        live = true;
      }
    }
    if (!live) {
      peer_dial(core, cluster, cluster.dial[i]);
    }
  }
}

void drain_mail(Core& core) noexcept {
  CoreMail* mail = core.impl->mail;
  if (mail == nullptr || !mail->ok()) {
    return;
  }
  alignas(32) float vec[kHnswMaxDimension];
  char text[kMailTextCap];
  char response[kMailTextCap];
  char model[kMailModelCap];
  for (std::uint32_t from = 0; from < mail->cores(); ++from) {
    if (from == static_cast<std::uint32_t>(core.id)) {
      continue;
    }
    MailView view;
    while (mail->take(static_cast<std::uint32_t>(core.id), from, view, vec, text, response, model)) {
      const auto vector = std::span<const float>(vec, view.vector.size());
      if (view.kind == MailKind::Replica) {
        if (store_record(core, vector, view.text, view.response, false) && core.id == 0) {
          cluster_note_stored(core, vector, view.text, view.response);
        }
      } else if (view.kind == MailKind::Hit) {
        (void)store_record(core, vector, view.text, view.response, false);
        if (view.conn < core.conn_cap) {
          Conn& conn = core.conns[view.conn];
          if (conn.phase == Phase::AwaitCluster) {
            if (!serve_text(core, conn, view.response)) {
              send_status(core, conn, 502, "cache response too large");
              rearm(core, conn);
            }
          }
        }
      } else if (view.kind == MailKind::Fill) {
        if (view.conn < core.conn_cap) {
          Conn& conn = core.conns[view.conn];
          if (conn.phase == Phase::AwaitCluster) {
            conn.key = view.key;
            begin_proxy(core, conn);
          }
        }
      } else if (view.kind == MailKind::Fail) {
        if (view.conn < core.conn_cap) {
          Conn& conn = core.conns[view.conn];
          if (conn.phase == Phase::AwaitCluster) {
            send_status(core, conn, 502, "cache owners unreachable");
            rearm(core, conn);
          }
        }
      } else if (view.kind == MailKind::Resolve && core.id == 0) {
        Cluster* cluster = cluster_of(core.impl);
        if (cluster != nullptr) {
          (void)decide_miss(core, *cluster, static_cast<int>(from), view.conn, 0, view.text, view.model, nullptr);
        }
      }
    }
  }
}

void drain_wake_fd(Core& core) noexcept {
  const int fd = core.impl->wake_fd[core.id];
  if (fd < 0) {
    return;
  }
  std::uint64_t value = 0;
  while (::read(fd, &value, sizeof(value)) > 0) {
  }
}

void cluster_on_loop(Core& core) noexcept {
  drain_wake_fd(core);
  drain_mail(core);
  Cluster* cluster = cluster_of(core.impl);
  if (cluster == nullptr || core.id != 0) {
    return;
  }
  cluster_tick(core, *cluster);
}

void peer_epoll(Core& core, Watch* watch, std::uint32_t events) noexcept {
  Cluster* cluster = cluster_of(core.impl);
  if (cluster == nullptr || watch == nullptr) {
    return;
  }
  if (watch->kind == kWatchPeerListen) {
    if ((events & (EPOLLERR | EPOLLHUP)) == 0) {
      peer_accept(core, *cluster);
    }
    return;
  }
  if (watch->slot >= kMaxPeers) {
    return;
  }
  PeerConn& peer = cluster->peers[watch->slot];
  if (peer.fd < 0) {
    return;
  }
  if ((events & EPOLLERR) != 0 && peer.connecting) {
    peer_writable(core, *cluster, peer);
    return;
  }
  if ((events & EPOLLOUT) != 0) {
    peer_writable(core, *cluster, peer);
  }
  if (peer.fd >= 0 && (events & (EPOLLIN | EPOLLHUP)) != 0) {
    peer_readable(core, *cluster, peer);
  }
}

[[nodiscard]] bool parse_peers(const char* text, MemberEndpoint* out, int cap, int& count) noexcept {
  count = 0;
  if (text == nullptr || text[0] == '\0') {
    return true;
  }
  const char* cursor = text;
  while (*cursor != '\0') {
    if (count >= cap) {
      return false;
    }
    const char* comma = std::strchr(cursor, ',');
    const std::size_t len = comma == nullptr ? std::strlen(cursor) : static_cast<std::size_t>(comma - cursor);
    char host[64];
    if (len == 0 || len >= sizeof(host)) {
      return false;
    }
    std::memcpy(host, cursor, len);
    host[len] = '\0';
    char* colon = std::strrchr(host, ':');
    if (colon == nullptr || colon == host || colon[1] == '\0') {
      return false;
    }
    *colon = '\0';
    in_addr addr{};
    if (::inet_pton(AF_INET, host, &addr) != 1) {
      return false;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long port = std::strtoul(colon + 1, &end, 10);
    if (errno != 0 || end == colon + 1 || *end != '\0' || port == 0 || port > 65535ul) {
      return false;
    }
    out[count].addr = ntohl(addr.s_addr);
    out[count].port = static_cast<std::uint16_t>(port);
    ++count;
    if (comma == nullptr) {
      break;
    }
    cursor = comma + 1;
  }
  return true;
}

void cluster_setup(Core& core) noexcept {
  const int wake = core.id >= 0 && core.id < 64 ? core.impl->wake_fd[core.id] : -1;
  if (wake >= 0 && core.ep >= 0) {
    core.wake_watch.kind = kWatchWake;
    core.wake_watch.conn = nullptr;
    arm_fd(core.ep, wake, core.wake_watch, static_cast<std::uint32_t>(EPOLLIN));
  }
  if (core.id != 0 || !core.impl->config.cluster || core.impl->peer_listen < 0) {
    return;
  }
  if (core.impl->cluster_node != nullptr) {
    return;
  }
  Cluster* cluster = new (std::nothrow) Cluster;
  if (cluster == nullptr) {
    core.impl->failed.store(true, std::memory_order_release);
    return;
  }
  for (int i = 0; i < kMaxPeers; ++i) {
    cluster->peers[i].fd = -1;
    cluster->peers[i].in = new (std::nothrow) std::byte[kPeerBuf];
    cluster->peers[i].outb = new (std::nothrow) std::byte[kPeerBuf];
    if (cluster->peers[i].in == nullptr || cluster->peers[i].outb == nullptr) {
      core.impl->failed.store(true, std::memory_order_release);
      for (int j = 0; j <= i; ++j) {
        delete[] cluster->peers[j].in;
        delete[] cluster->peers[j].outb;
        cluster->peers[j].in = nullptr;
        cluster->peers[j].outb = nullptr;
      }
      delete cluster;
      return;
    }
  }
  cluster->self.addr = core.impl->self_addr;
  cluster->self.port = core.impl->peer_port;
  cluster->listen_fd = core.impl->peer_listen;
  cluster->swim.configure(200, 200, 1000);
  cluster->swim.start(cluster->self, 1);
  if (!parse_peers(core.impl->peers_text, cluster->dial, kMaxPeers, cluster->dial_n)) {
    std::fprintf(stderr, "kestrel: --peers must be IPv4:PORT,IPv4:PORT\n");
    core.impl->failed.store(true, std::memory_order_release);
  }
  cluster->listen_watch.kind = kWatchPeerListen;
  cluster->listen_watch.conn = nullptr;
  cluster->accept_op.kind = Op::PeerAccept;
  cluster->wake_op.kind = Op::Wake;
  core.impl->cluster_node = cluster;
  cluster_rebuild(core, *cluster);
  if (core.ep >= 0) {
    arm_fd(core.ep, cluster->listen_fd, cluster->listen_watch, EPOLLIN);
  }
  for (int i = 0; i < cluster->dial_n; ++i) {
    peer_dial(core, *cluster, cluster->dial[i]);
  }
}

void cluster_shutdown(Core& core) noexcept {
  if (core.id != 0) {
    return;
  }
  Cluster* cluster = cluster_of(core.impl);
  if (cluster == nullptr) {
    return;
  }
  for (int i = 0; i < kMaxPeers; ++i) {
    if (cluster->peers[i].fd >= 0) {
      ::close(cluster->peers[i].fd);
      cluster->peers[i].fd = -1;
    }
    delete[] cluster->peers[i].in;
    delete[] cluster->peers[i].outb;
  }
  if (cluster->listen_fd >= 0) {
    ::close(cluster->listen_fd);
    core.impl->peer_listen = -1;
  }
  delete cluster;
  core.impl->cluster_node = nullptr;
}

void cluster_drive_uring(Core& core) noexcept {
#ifdef KESTREL_HAS_LIBURING
  Cluster* cluster = cluster_of(core.impl);
  if (!core.ring_ready || cluster == nullptr || core.id != 0) {
    return;
  }
  if (!cluster->accept_on && cluster->listen_fd >= 0) {
    io_uring_sqe* sqe = next_sqe(core);
    if (sqe != nullptr) {
      cluster->accept_len = sizeof(cluster->accept_addr);
      io_uring_prep_accept(sqe, cluster->listen_fd, reinterpret_cast<sockaddr*>(&cluster->accept_addr),
                           &cluster->accept_len, SOCK_CLOEXEC);
      io_uring_sqe_set_data(sqe, &cluster->accept_op);
      cluster->accept_on = true;
    }
  }
  const int wake = core.impl->wake_fd[core.id];
  if (wake >= 0 && !cluster->wake_on) {
    io_uring_sqe* sqe = next_sqe(core);
    if (sqe != nullptr) {
      io_uring_prep_read(sqe, wake, &cluster->wake_buf, sizeof(cluster->wake_buf), 0);
      io_uring_sqe_set_data(sqe, &cluster->wake_op);
      cluster->wake_on = true;
    }
  }
  for (int i = 0; i < kMaxPeers; ++i) {
    PeerConn& peer = cluster->peers[i];
    if (peer.fd < 0) {
      continue;
    }
    if (peer.connecting && !peer.connect_on) {
      io_uring_sqe* sqe = next_sqe(core);
      if (sqe != nullptr) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(peer.endpoint.port);
        addr.sin_addr.s_addr = htonl(peer.endpoint.addr);
        peer.connect_addr = addr;
        io_uring_prep_connect(sqe, peer.fd, reinterpret_cast<sockaddr*>(&peer.connect_addr), sizeof(peer.connect_addr));
        io_uring_sqe_set_data(sqe, &peer.connect_op);
        peer.connect_on = true;
      }
    }
    if (!peer.recv_on && !peer.connecting && peer.in_len < kPeerBuf) {
      io_uring_sqe* sqe = next_sqe(core);
      if (sqe != nullptr) {
        io_uring_prep_recv(sqe, peer.fd, peer.in + peer.in_len, kPeerBuf - peer.in_len, 0);
        io_uring_sqe_set_data(sqe, &peer.recv_op);
        peer.recv_on = true;
      }
    }
    if (!peer.send_on && peer.out_len > peer.out_sent) {
      io_uring_sqe* sqe = next_sqe(core);
      if (sqe != nullptr) {
        io_uring_prep_send(sqe, peer.fd, peer.outb + peer.out_sent, peer.out_len - peer.out_sent, MSG_NOSIGNAL);
        io_uring_sqe_set_data(sqe, &peer.send_op);
        peer.send_on = true;
      }
    }
  }
#else
  (void)core;
#endif
}

void cluster_on_cqe(Core& core, Op* op, int res) noexcept {
  Cluster* cluster = cluster_of(core.impl);
  if (cluster == nullptr || op == nullptr) {
    return;
  }
  if (op->kind == Op::Wake) {
    cluster->wake_on = false;
    drain_mail(core);
    return;
  }
  if (op->kind == Op::PeerAccept) {
    cluster->accept_on = false;
    if (res >= 0) {
      PeerConn* peer = peer_slot(*cluster);
      if (peer == nullptr || !set_nodelay(res)) {
        ::close(res);
        return;
      }
      peer_attach(*cluster, *peer, res);
      if (!peer_send_hello(core, *cluster, *peer)) {
        peer_close(core, *cluster, *peer);
      }
    }
    return;
  }
  if (op->slot >= kMaxPeers) {
    return;
  }
  PeerConn& peer = cluster->peers[op->slot];
  if (op->kind == Op::PeerConnect) {
    peer.connect_on = false;
    peer.connecting = false;
    if (res != 0 || !peer_send_hello(core, *cluster, peer)) {
      peer_close(core, *cluster, peer);
    }
    return;
  }
  if (op->kind == Op::PeerRecv) {
    peer.recv_on = false;
    if (res > 0) {
      peer.in_len += static_cast<std::size_t>(res);
      peer_parse(core, *cluster, peer);
    } else if (res != -EAGAIN && res != -EINTR) {
      peer_close(core, *cluster, peer);
    }
    return;
  }
  if (op->kind == Op::PeerSend) {
    peer.send_on = false;
    if (res > 0) {
      peer.out_sent += static_cast<std::size_t>(res);
      peer_reclaim(peer);
    } else if (res != -EAGAIN && res != -EINTR) {
      peer_close(core, *cluster, peer);
    }
  }
}

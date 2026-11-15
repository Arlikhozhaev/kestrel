#include "kestrel/swim.hpp"

namespace kestrel {

void Swim::configure(std::uint32_t period_ms, std::uint32_t timeout_ms, std::uint32_t suspect_ms) noexcept {
  if (period_ms == 0 || timeout_ms == 0 || suspect_ms == 0) {
    return;
  }
  period_ms_ = period_ms;
  timeout_ms_ = timeout_ms;
  suspect_ms_ = suspect_ms;
}

void Swim::start(MemberEndpoint self, std::uint64_t incarnation) noexcept {
  self_ = self;
  self_incarnation_ = incarnation == 0 ? 1 : incarnation;
  count_ = 0;
  self_index_ = 0;
  cursor_ = 0;
  probe_index_ = kSwimMaxMembers;
  probe_indirect_ = false;
  req_from_ = kSwimMaxMembers;
  next_period_ms_ = 0;
  members_[0].endpoint = self_;
  members_[0].status = SwimStatus::Alive;
  members_[0].incarnation = self_incarnation_;
  suspect_since_[0] = 0;
  count_ = 1;
}

std::uint32_t Swim::index_of(MemberEndpoint endpoint) const noexcept {
  for (std::uint32_t i = 0; i < count_; ++i) {
    if (members_[i].endpoint == endpoint) {
      return i;
    }
  }
  return kSwimMaxMembers;
}

std::uint32_t Swim::push(SwimAction* out, std::uint32_t n, std::uint32_t max, SwimAction action) const noexcept {
  if (out == nullptr || n >= max) {
    return n;
  }
  out[n] = action;
  return n + 1u;
}

void Swim::clear_probe() noexcept {
  probe_index_ = kSwimMaxMembers;
  probe_indirect_ = false;
  probe_deadline_ms_ = 0;
}

std::uint32_t Swim::alive(MemberEndpoint* out, std::uint32_t cap) const noexcept {
  std::uint32_t n = 0;
  if (out == nullptr) {
    return 0;
  }
  for (std::uint32_t i = 0; i < count_ && n < cap; ++i) {
    if (members_[i].status != SwimStatus::Dead) {
      out[n] = members_[i].endpoint;
      ++n;
    }
  }
  return n;
}

bool Swim::seed(MemberEndpoint endpoint) noexcept {
  if (endpoint == self_ || index_of(endpoint) != kSwimMaxMembers) {
    return index_of(endpoint) != kSwimMaxMembers || endpoint == self_;
  }
  if (count_ >= kSwimMaxMembers) {
    return false;
  }
  members_[count_].endpoint = endpoint;
  members_[count_].status = SwimStatus::Alive;
  members_[count_].incarnation = 0;
  suspect_since_[count_] = 0;
  ++count_;
  return true;
}

std::uint32_t Swim::suspect(std::uint32_t index, std::uint64_t now_ms, SwimAction* out, std::uint32_t n,
                           std::uint32_t max) noexcept {
  if (index >= count_ || index == self_index_) {
    return n;
  }
  if (members_[index].status == SwimStatus::Alive) {
    members_[index].status = SwimStatus::Suspected;
    suspect_since_[index] = now_ms;
  }
  SwimAction action;
  action.kind = SwimActionKind::Member;
  action.to = members_[index].endpoint;
  action.about = members_[index].endpoint;
  action.incarnation = members_[index].incarnation;
  action.status = SwimStatus::Suspected;
  return push(out, n, max, action);
}

std::uint32_t Swim::observe(MemberEndpoint endpoint, SwimStatus status, std::uint64_t incarnation, std::uint64_t now_ms,
                           SwimAction* out, std::uint32_t max) noexcept {
  std::uint32_t n = 0;
  if (endpoint == self_) {
    if ((status == SwimStatus::Suspected || status == SwimStatus::Dead) && incarnation >= self_incarnation_) {
      ++self_incarnation_;
      members_[self_index_].incarnation = self_incarnation_;
      members_[self_index_].status = SwimStatus::Alive;
      SwimAction alive;
      alive.kind = SwimActionKind::Member;
      alive.about = self_;
      alive.incarnation = self_incarnation_;
      alive.status = SwimStatus::Alive;
      n = push(out, n, max, alive);
    }
    return n;
  }
  std::uint32_t index = index_of(endpoint);
  if (index == kSwimMaxMembers) {
    if (count_ >= kSwimMaxMembers || status == SwimStatus::Dead) {
      return n;
    }
    index = count_;
    members_[index].endpoint = endpoint;
    members_[index].status = status;
    members_[index].incarnation = incarnation;
    suspect_since_[index] = status == SwimStatus::Suspected ? now_ms : 0;
    ++count_;
    return n;
  }
  SwimMember& member = members_[index];
  if (incarnation < member.incarnation) {
    return n;
  }
  if (incarnation == member.incarnation) {
    if (member.status == SwimStatus::Dead) {
      return n;
    }
    if (status == SwimStatus::Alive && member.status == SwimStatus::Suspected) {
      member.status = SwimStatus::Alive;
      suspect_since_[index] = 0;
      if (probe_index_ == index) {
        clear_probe();
      }
      return n;
    }
    if (status == SwimStatus::Suspected && member.status == SwimStatus::Alive) {
      member.status = SwimStatus::Suspected;
      suspect_since_[index] = now_ms;
      return n;
    }
    if (status != SwimStatus::Dead) {
      return n;
    }
  }
  member.incarnation = incarnation;
  member.status = status;
  suspect_since_[index] = status == SwimStatus::Suspected ? now_ms : 0;
  if (status == SwimStatus::Dead && probe_index_ == index) {
    clear_probe();
  }
  if (status == SwimStatus::Alive && probe_index_ == index) {
    clear_probe();
  }
  return n;
}

std::uint32_t Swim::tick(std::uint64_t now_ms, SwimAction* out, std::uint32_t max) noexcept {
  std::uint32_t n = 0;
  for (std::uint32_t i = 0; i < count_; ++i) {
    if (members_[i].status == SwimStatus::Suspected && suspect_since_[i] != 0 &&
        now_ms - suspect_since_[i] >= suspect_ms_) {
      members_[i].status = SwimStatus::Dead;
      suspect_since_[i] = 0;
      if (probe_index_ == i) {
        clear_probe();
      }
      SwimAction dead;
      dead.kind = SwimActionKind::Member;
      dead.about = members_[i].endpoint;
      dead.incarnation = members_[i].incarnation;
      dead.status = SwimStatus::Dead;
      n = push(out, n, max, dead);
    }
  }
  if (probe_index_ < count_ && now_ms >= probe_deadline_ms_) {
    if (!probe_indirect_) {
      std::uint32_t via = kSwimMaxMembers;
      for (std::uint32_t step = 0; step < count_; ++step) {
        const std::uint32_t candidate = (probe_index_ + 1u + step) % count_;
        if (candidate != self_index_ && candidate != probe_index_ &&
            members_[candidate].status != SwimStatus::Dead) {
          via = candidate;
          break;
        }
      }
      if (via == kSwimMaxMembers) {
        n = suspect(probe_index_, now_ms, out, n, max);
        clear_probe();
      } else {
        SwimAction req;
        req.kind = SwimActionKind::PingReq;
        req.to = members_[via].endpoint;
        req.about = members_[probe_index_].endpoint;
        req.incarnation = members_[probe_index_].incarnation;
        n = push(out, n, max, req);
        probe_indirect_ = true;
        probe_deadline_ms_ = now_ms + timeout_ms_;
      }
    } else {
      n = suspect(probe_index_, now_ms, out, n, max);
      clear_probe();
    }
  }
  if (probe_index_ < count_ || now_ms < next_period_ms_) {
    return n;
  }
  next_period_ms_ = now_ms + period_ms_;
  if (count_ < 2) {
    return n;
  }
  for (std::uint32_t step = 0; step < count_; ++step) {
    const std::uint32_t candidate = (cursor_ + step) % count_;
    if (candidate == self_index_ || members_[candidate].status == SwimStatus::Dead) {
      continue;
    }
    cursor_ = (candidate + 1u) % count_;
    probe_index_ = candidate;
    probe_indirect_ = false;
    probe_deadline_ms_ = now_ms + timeout_ms_;
    SwimAction ping;
    ping.kind = SwimActionKind::Ping;
    ping.to = members_[candidate].endpoint;
    ping.about = self_;
    ping.incarnation = self_incarnation_;
    ping.status = SwimStatus::Alive;
    n = push(out, n, max, ping);
    break;
  }
  return n;
}

std::uint32_t Swim::on_ping(MemberEndpoint from, std::uint64_t incarnation, std::uint64_t now_ms, SwimAction* out,
                           std::uint32_t max) noexcept {
  std::uint32_t n = observe(from, SwimStatus::Alive, incarnation, now_ms, out, max);
  SwimAction ack;
  ack.kind = SwimActionKind::PingAck;
  ack.to = from;
  ack.about = self_;
  ack.incarnation = self_incarnation_;
  ack.status = SwimStatus::Alive;
  return push(out, n, max, ack);
}

std::uint32_t Swim::on_ack(MemberEndpoint about, std::uint64_t incarnation, std::uint64_t now_ms, SwimAction* out,
                          std::uint32_t max) noexcept {
  std::uint32_t n = observe(about, SwimStatus::Alive, incarnation, now_ms, out, max);
  if (req_from_ < count_ && about == req_target_) {
    SwimAction ack;
    ack.kind = SwimActionKind::PingAck;
    ack.to = members_[req_from_].endpoint;
    ack.about = about;
    ack.incarnation = incarnation;
    ack.status = SwimStatus::Alive;
    n = push(out, n, max, ack);
    req_from_ = kSwimMaxMembers;
  }
  return n;
}

std::uint32_t Swim::on_pingreq(MemberEndpoint from, MemberEndpoint target, std::uint64_t incarnation,
                              std::uint64_t now_ms, SwimAction* out, std::uint32_t max) noexcept {
  // Incarnation 0 does not override a member we already know. An unknown
  // requester is not admitted here; Hello is the join.
  std::uint32_t n = 0;
  if (index_of(from) == kSwimMaxMembers) {
    return n;
  }
  n = observe(from, SwimStatus::Alive, incarnation, now_ms, out, max);
  req_from_ = index_of(from);
  req_target_ = target;
  SwimAction ping;
  ping.kind = SwimActionKind::Ping;
  ping.to = target;
  ping.about = self_;
  ping.incarnation = self_incarnation_;
  return push(out, n, max, ping);
}

}  // namespace kestrel

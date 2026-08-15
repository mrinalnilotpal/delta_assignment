#include "oms/Signal.h"

#include <cstring>

namespace oms {

namespace {
template <typename T>
void put(std::vector<uint8_t>& b, const T& v) {
  const auto* p = reinterpret_cast<const uint8_t*>(&v);
  b.insert(b.end(), p, p + sizeof(T));
}

template <typename T>
bool get(const std::vector<uint8_t>& b, std::size_t& off, T& v) {
  if (off + sizeof(T) > b.size()) return false;
  std::memcpy(&v, b.data() + off, sizeof(T));
  off += sizeof(T);
  return true;
}
}  // namespace

std::vector<uint8_t> serialize_signal(const SignalMessage& m) {
  std::vector<uint8_t> b;
  put(b, m.schema_version);
  put(b, m.sequence);
  put(b, m.generated_at);
  put(b, m.strategy_id);
  const uint32_t n = static_cast<uint32_t>(m.targets.size());
  put(b, n);
  for (const auto& t : m.targets) {
    put(b, t.instrument);
    put(b, t.signed_target);
  }
  return b;
}

bool deserialize_signal(const std::vector<uint8_t>& b, SignalMessage& out) {
  std::size_t off = 0;
  uint32_t n = 0;
  if (!get(b, off, out.schema_version)) return false;
  if (!get(b, off, out.sequence)) return false;
  if (!get(b, off, out.generated_at)) return false;
  if (!get(b, off, out.strategy_id)) return false;
  if (!get(b, off, n)) return false;
  out.targets.clear();
  out.targets.reserve(n);
  for (uint32_t i = 0; i < n; ++i) {
    TargetPosition t;
    if (!get(b, off, t.instrument)) return false;
    if (!get(b, off, t.signed_target)) return false;
    out.targets.push_back(t);
  }
  return off == b.size();   // reject trailing garbage
}

SignalVerdict SignalGate::check(const SignalMessage& msg, Timestamp now) {
  // (0) Do-not-trade: reject everything until startup reconciliation clears it.
  if (!trading_enabled_) {
    if (log_) log_->log(LogLevel::Warn,
                        "signal rejected: do-not-trade (startup reconcile not complete)");
    return SignalVerdict::RejectDNT;
  }

  // (1) Absurd values at ingest: reject, do not carry into delta computation.
  for (const auto& t : msg.targets) {
    const Quantity a = t.signed_target < 0 ? -t.signed_target : t.signed_target;
    if (a > cfg_.max_abs_target) {
      if (log_) log_->log(LogLevel::Error,
                          "signal rejected: absurd target inst=" +
                              std::to_string(t.instrument) +
                              " qty=" + std::to_string(t.signed_target));
      return SignalVerdict::RejectAbsurd;
    }
  }

  // (2) Age: too old to act on.
  if (now - msg.generated_at > cfg_.max_signal_age) {
    if (log_) log_->log(LogLevel::Warn,
                        "signal rejected: stale age producer=" +
                            std::to_string(msg.strategy_id) +
                            " gen_at=" + std::to_string(msg.generated_at));
    return SignalVerdict::RejectAge;
  }

  // (3) Sequence gap: wrap-aware, threshold-based.
  Producer& p = producers_[msg.strategy_id];
  SignalVerdict verdict = SignalVerdict::Accept;
  if (p.seen) {
    const uint64_t mod = cfg_.seq_modulo;
    const uint64_t delta = (msg.sequence + mod - p.expected_seq) % mod;
    if (delta == 0) {
      verdict = SignalVerdict::Accept;                 // exactly in order
    } else if (delta >= mod - cfg_.miss_threshold) {
      // Close to the wrap maximum => went backwards => duplicate/reorder.
      if (log_) log_->log(LogLevel::Warn,
                          "signal dropped: duplicate/reorder producer=" +
                              std::to_string(msg.strategy_id) +
                              " seq=" + std::to_string(msg.sequence));
      return SignalVerdict::DuplicateDropped;          // do NOT advance expected
    } else {
      // Forward gap: data loss is recoverable, the next full snapshot heals it.
      if (log_) log_->log(LogLevel::Warn,
                          "signal gap: producer=" + std::to_string(msg.strategy_id) +
                              " expected=" + std::to_string(p.expected_seq) +
                              " got=" + std::to_string(msg.sequence) +
                              " missed=" + std::to_string(delta));
      verdict = SignalVerdict::GapAcceptedLogged;
    }
  }

  p.seen = true;
  p.expected_seq = (msg.sequence + 1) % cfg_.seq_modulo;
  p.last_accept = now;
  return verdict;
}

bool SignalGate::heartbeat_alive(StrategyId producer, Timestamp now) const {
  auto it = producers_.find(producer);
  if (it == producers_.end() || !it->second.seen) return false;
  const Timestamp deadline = cfg_.hb_interval * static_cast<Timestamp>(cfg_.hb_threshold);
  return (now - it->second.last_accept) <= deadline;
}

}  // namespace oms

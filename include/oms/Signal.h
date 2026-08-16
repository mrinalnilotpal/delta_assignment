#pragma once

#include "oms/Logging.h"
#include "oms/Types.h"

namespace oms {

// One instrument's signed target position.
struct TargetPosition {
  InstrumentId instrument{0};
  Quantity     signed_target{0};
};

// A published target book (spec 2.8). `generated_at` is the producer clock;
// `sequence` is monotonic per producer, so staleness is detectable.
struct SignalMessage {
  uint32_t                    schema_version{1};
  uint64_t                    sequence{0};
  Timestamp                   generated_at{0};
  StrategyId                  strategy_id{0};
  std::vector<TargetPosition> targets;
};

// ---- Transport: in-process queue + length-prefixed binary codec -------------
class SignalTransport {
 public:
  virtual ~SignalTransport() = default;
  virtual bool try_pop(SignalMessage& out) = 0;
};

class InProcessSignalQueue : public SignalTransport {
 public:
  void push(const SignalMessage& m) { q_.push_back(m); }
  bool try_pop(SignalMessage& out) override {
    if (q_.empty()) return false;
    out = std::move(q_.front());
    q_.pop_front();
    return true;
  }
  std::size_t size() const { return q_.size(); }

 private:
  std::deque<SignalMessage> q_;
};

// Length-prefixed binary codec (the "file/stdin binary reader" is a thin adapter
// over this). Returns false on a malformed/truncated buffer.
std::vector<uint8_t> serialize_signal(const SignalMessage&);
bool deserialize_signal(const std::vector<uint8_t>&, SignalMessage& out);

// ---- Staleness detection: three independent checks --------------------------
enum class SignalVerdict : uint8_t {
  Accept,             // apply this signal
  RejectDNT,          // do-not-trade: startup reconciliation has not cleared yet
  RejectAge,          // too old: now - generated_at > max_signal_age
  RejectAbsurd,       // NaN/absurd target value at ingest
  GapAcceptedLogged,  // forward sequence gap: recoverable, applied, logged
  DuplicateDropped,   // backward/duplicate sequence: dropped
};

struct SignalGateConfig {
  Timestamp max_signal_age{5'000'000'000};   // 5s
  uint64_t  seq_modulo{1ull << 16};          // wrap space (mirrors uint16 seq)
  uint64_t  miss_threshold{5};
  Timestamp hb_interval{60'000'000'000};     // 60s
  int       hb_threshold{3};
  Quantity  max_abs_target{1'000'000'000};   // absurd beyond this
};

// Detects staleness at the TRANSPORT layer (not per-value). Age + sequence-gap
// checks are per-message; heartbeat is time-based (you cannot detect "nothing
// arrived" without a clock).
class SignalGate {
 public:
  explicit SignalGate(SignalGateConfig cfg = {}, ILogSink* log = nullptr)
      : cfg_(cfg), log_(log) {}

  SignalVerdict check(const SignalMessage& msg, Timestamp now);

  // Startup gate (spec 2.10 / brief §6): the system boots in do-not-trade and
  // rejects every signal until startup reconciliation clears it. Wire the app's
  // StartupReport.can_start into this before accepting any signal on boot.
  void enable_trading(bool on) { trading_enabled_ = on; }
  bool trading_enabled() const { return trading_enabled_; }

  // Heartbeat: false => producer presumed dead (stop new work, cancel resting).
  bool heartbeat_alive(StrategyId producer, Timestamp now) const;

 private:
  struct Producer {
    uint64_t  expected_seq{0};
    bool      seen{false};
    Timestamp last_accept{0};
  };

  SignalGateConfig                       cfg_;
  ILogSink*                              log_;
  bool                                   trading_enabled_{false};   // boot in DNT
  std::unordered_map<StrategyId, Producer> producers_;
};

}  // namespace oms

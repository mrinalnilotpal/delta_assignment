#pragma once

#include "oms/Logging.h"
#include "oms/RingBuffer.h"
#include "oms/Types.h"

namespace oms {

// Circuit-breaker state machine (spec 2.6):
//   Healthy -> Degraded -> Down -> (dwell) -> Probing -> Healthy
enum class HealthState : uint8_t { Healthy, Degraded, Down, Probing };

// Which composite signal caused the most recent trip.
enum class TripReason : uint8_t { None, Connectivity, RejectionRate, AckLatency };

struct HealthScore {
  HealthState state{HealthState::Healthy};
  double      weight{1.0};   // routing weight; 0.0 when Down
  TripReason  reason{TripReason::None};
};

// Thresholds. Defaults are production-ish; tests inject small values.
struct HealthConfig {
  // Connectivity: staleness tolerance for "nothing arrived".
  Timestamp   heartbeat_tolerance_ns = 5'000'000'000;   // 5s

  // Rejection rate over a rolling window (false cancel-rejects excluded).
  std::size_t reject_window      = 128;
  std::size_t reject_min_samples = 20;
  double      reject_high        = 0.20;   // trip above
  double      reject_low         = 0.10;   // recover below (hysteresis)

  // Ack latency: p99 over a rolling window.
  std::size_t latency_window   = 128;
  LatencyNs   latency_high_ns  = 5'000'000;   // 5ms trip
  LatencyNs   latency_low_ns   = 2'000'000;   // 2ms recover

  // Dwell + probing.
  Timestamp degrade_dwell_ns = 1'000'000'000;   // stay bad this long -> Down
  Timestamp down_dwell_ns    = 2'000'000'000;   // stay good this long -> Probing
  int       probe_required   = 5;               // successful round trips to restore
  double    probe_weight     = 0.25;
  double    degraded_weight  = 0.50;
};

// Health is updated from LIVE EVENTS ONLY. A timer (tick) is used solely to
// evaluate time-based staleness ("nothing arrived"), which cannot be detected
// without a clock. All threshold arithmetic lives off the hot path.
class HealthModel {
 public:
  virtual ~HealthModel() = default;

  virtual void on_order_sent(VenueId, Timestamp) = 0;
  virtual void on_ack(VenueId, LatencyNs) = 0;
  virtual void on_reject(VenueId, bool is_false_cancel_reject) = 0;
  virtual void on_fill(VenueId) = 0;
  virtual void on_timeout(VenueId) = 0;
  virtual void on_disconnect(VenueId) = 0;
  virtual void on_reconnect(VenueId) = 0;

  virtual HealthScore score(VenueId) const = 0;
  virtual bool is_tradeable(VenueId) const = 0;
};

class CircuitBreakerHealthModel : public HealthModel {
 public:
  explicit CircuitBreakerHealthModel(HealthConfig cfg = {}, ILogSink* log = nullptr);

  // Inject a deterministic clock for tests (defaults to steady_clock ns).
  void set_clock(std::function<Timestamp()> clock) { clock_ = std::move(clock); }

  void register_venue(VenueId v);

  void on_order_sent(VenueId, Timestamp) override;
  void on_ack(VenueId, LatencyNs) override;
  void on_reject(VenueId, bool is_false_cancel_reject) override;
  void on_fill(VenueId) override;
  void on_timeout(VenueId) override;
  void on_disconnect(VenueId) override;
  void on_reconnect(VenueId) override;

  HealthScore score(VenueId) const override;
  bool is_tradeable(VenueId) const override;

  // Timer-driven evaluation of time-based staleness only.
  void tick(Timestamp now);

  // Diagnostics.
  HealthState state(VenueId) const;
  uint64_t trips(VenueId) const;
  uint64_t unresolved(VenueId) const;

 private:
  struct VenueState {
    bool                  connected{true};
    Timestamp             last_seen{0};
    RingBuffer<uint8_t>   rejects;      // 1 = reject, 0 = ack success
    RingBuffer<LatencyNs> latencies;
    HealthState           state{HealthState::Healthy};
    TripReason            reason{TripReason::None};
    Timestamp             degraded_since{0};
    Timestamp             good_since{0};   // when conditions turned good while Down
    int                   probe_success{0};
    uint64_t              trips{0};
    uint64_t              unresolved{0};

    explicit VenueState(const HealthConfig& c)
        : rejects(c.reject_window), latencies(c.latency_window) {}
  };

  struct Conditions {
    bool       any_high{false};
    bool       all_low{false};
    TripReason reason{TripReason::None};
  };

  Conditions assess(const VenueState& v, Timestamp now) const;
  void evaluate(VenueId id, VenueState& v, Timestamp now);
  VenueState& must_get(VenueId v);
  const VenueState& must_get(VenueId v) const;

  HealthConfig                          cfg_;
  ILogSink*                             log_;
  std::function<Timestamp()>            clock_;
  std::unordered_map<VenueId, VenueState> venues_;
};

}  // namespace oms

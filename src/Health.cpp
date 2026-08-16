#include "oms/Health.h"

#include "oms/Clock.h"
#include "oms/Detail.h"

namespace oms {

CircuitBreakerHealthModel::CircuitBreakerHealthModel(HealthConfig cfg, ILogSink* log)
    : cfg_(cfg), log_(log), clock_(steady_now_ns) {}

void CircuitBreakerHealthModel::register_venue(VenueId v) {
  auto it = venues_.find(v);
  if (it == venues_.end()) {
    VenueState st(cfg_);
    st.last_seen = clock_();
    venues_.emplace(v, std::move(st));
  }
}

CircuitBreakerHealthModel::VenueState& CircuitBreakerHealthModel::must_get(VenueId v) {
  auto it = venues_.find(v);
  assert(it != venues_.end() && "venue not registered with health model");
  return it->second;
}

const CircuitBreakerHealthModel::VenueState&
CircuitBreakerHealthModel::must_get(VenueId v) const {
  auto it = venues_.find(v);
  assert(it != venues_.end() && "venue not registered with health model");
  return it->second;
}

CircuitBreakerHealthModel::Conditions
CircuitBreakerHealthModel::assess(const VenueState& v, Timestamp now) const {
  Conditions c;

  const bool conn_bad =
      !v.connected || (now - v.last_seen > cfg_.heartbeat_tolerance_ns);

  const bool enough = v.rejects.size() >= cfg_.reject_min_samples;
  const double rej = v.rejects.fraction([](uint8_t x) { return x == 1; });
  const bool rej_high = enough && rej > cfg_.reject_high;
  const bool rej_low  = !enough || rej < cfg_.reject_low;

  const bool have_lat = !v.latencies.empty();
  const LatencyNs p99 = v.latencies.percentile(0.99);
  const bool lat_high = have_lat && p99 > cfg_.latency_high_ns;
  const bool lat_low  = !have_lat || p99 < cfg_.latency_low_ns;

  c.any_high = conn_bad || rej_high || lat_high;
  c.all_low  = !conn_bad && rej_low && lat_low;

  if (conn_bad)      c.reason = TripReason::Connectivity;
  else if (rej_high) c.reason = TripReason::RejectionRate;
  else if (lat_high) c.reason = TripReason::AckLatency;
  else               c.reason = TripReason::None;

  return c;
}

void CircuitBreakerHealthModel::evaluate(VenueId id, VenueState& v, Timestamp now) {
  const Conditions c = assess(v, now);
  const HealthState prev = v.state;

  switch (v.state) {
    case HealthState::Healthy:
      if (c.any_high) {
        v.state = HealthState::Degraded;
        v.degraded_since = now;
        v.reason = c.reason;
        ++v.trips;
        ++v.unresolved;
      }
      break;

    case HealthState::Degraded:
      if (c.all_low) {
        v.state = HealthState::Healthy;
        v.reason = TripReason::None;
        if (v.unresolved > 0) --v.unresolved;
      } else if (c.any_high && now - v.degraded_since >= cfg_.degrade_dwell_ns) {
        v.state = HealthState::Down;
        v.reason = c.reason;
        v.good_since = 0;
      }
      break;

    case HealthState::Down:
      if (c.all_low) {
        if (v.good_since == 0) {
          v.good_since = now;
        } else if (now - v.good_since >= cfg_.down_dwell_ns) {
          v.state = HealthState::Probing;
          v.probe_success = 0;
        }
      } else {
        v.good_since = 0;   // reset dwell on any relapse
      }
      break;

    case HealthState::Probing:
      if (c.any_high) {
        v.state = HealthState::Down;
        v.reason = c.reason;
        v.good_since = 0;
      } else if (v.probe_success >= cfg_.probe_required) {
        v.state = HealthState::Healthy;
        v.reason = TripReason::None;
        if (v.unresolved > 0) --v.unresolved;
      }
      break;
  }

  if (log_ && v.state != prev) {
    log_->log(v.state == HealthState::Healthy ? LogLevel::Info : LogLevel::Warn,
              std::string("health venue=") + std::to_string(id) +
                  " " + detail::name(prev) + "->" + detail::name(v.state) +
                  " reason=" + detail::name(v.reason) +
                  " trips=" + std::to_string(v.trips) +
                  " unresolved=" + std::to_string(v.unresolved));
  }
}

void CircuitBreakerHealthModel::on_order_sent(VenueId v, Timestamp ts) {
  auto& st = must_get(v);
  st.last_seen = ts;
  evaluate(v, st, clock_());
}

void CircuitBreakerHealthModel::on_ack(VenueId v, LatencyNs lat) {
  auto& st = must_get(v);
  const Timestamp now = clock_();
  st.last_seen = now;
  st.latencies.push(lat);
  st.rejects.push(0);  // successful send outcome
  if (st.state == HealthState::Probing && lat <= cfg_.latency_high_ns) {
    ++st.probe_success;
  }
  evaluate(v, st, now);
}

void CircuitBreakerHealthModel::on_reject(VenueId v, bool is_false_cancel_reject) {
  auto& st = must_get(v);
  const Timestamp now = clock_();
  st.last_seen = now;
  // False cancel-rejects are artifacts of the cancel/fill race and MUST NOT
  // pollute the rejection-rate signal.
  if (!is_false_cancel_reject) st.rejects.push(1);
  evaluate(v, st, now);
}

void CircuitBreakerHealthModel::on_fill(VenueId v) {
  auto& st = must_get(v);
  const Timestamp now = clock_();
  st.last_seen = now;
  evaluate(v, st, now);
}

void CircuitBreakerHealthModel::on_timeout(VenueId v) {
  auto& st = must_get(v);
  const Timestamp now = clock_();
  st.rejects.push(1);   // an unacked send is an order-path failure
  evaluate(v, st, now);
}

void CircuitBreakerHealthModel::on_disconnect(VenueId v) {
  auto& st = must_get(v);
  st.connected = false;
  evaluate(v, st, clock_());
}

void CircuitBreakerHealthModel::on_reconnect(VenueId v) {
  auto& st = must_get(v);
  st.connected = true;
  st.last_seen = clock_();
  evaluate(v, st, st.last_seen);
}

void CircuitBreakerHealthModel::tick(Timestamp now) {
  for (auto& [id, st] : venues_) evaluate(id, st, now);
}

HealthScore CircuitBreakerHealthModel::score(VenueId v) const {
  const auto& st = must_get(v);
  double weight = 0.0;
  switch (st.state) {
    case HealthState::Healthy:  weight = 1.0; break;
    case HealthState::Degraded: weight = cfg_.degraded_weight; break;
    case HealthState::Probing:  weight = cfg_.probe_weight; break;
    case HealthState::Down:     weight = 0.0; break;
  }
  return HealthScore{st.state, weight, st.reason};
}

bool CircuitBreakerHealthModel::is_tradeable(VenueId v) const {
  return must_get(v).state != HealthState::Down;
}

HealthState CircuitBreakerHealthModel::state(VenueId v) const { return must_get(v).state; }
uint64_t CircuitBreakerHealthModel::trips(VenueId v) const { return must_get(v).trips; }
uint64_t CircuitBreakerHealthModel::unresolved(VenueId v) const { return must_get(v).unresolved; }

}  // namespace oms

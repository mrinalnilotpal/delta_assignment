#include <catch2/catch_test_macros.hpp>

#include "oms/Health.h"

using namespace oms;

namespace {
HealthConfig fast_config() {
  HealthConfig c;
  c.reject_window = 20;
  c.reject_min_samples = 5;
  c.reject_high = 0.50;
  c.reject_low = 0.20;
  c.latency_window = 20;
  c.latency_high_ns = 1'000;
  c.latency_low_ns = 500;
  c.heartbeat_tolerance_ns = 1'000'000;   // large: connectivity won't trip here
  c.degrade_dwell_ns = 5;
  c.down_dwell_ns = 5;
  c.probe_required = 2;
  return c;
}
}  // namespace

TEST_CASE("rejection rate trips, hysteresis prevents flap, probing restores",
          "[health][hysteresis]") {
  Timestamp t = 1'000;
  CircuitBreakerHealthModel h(fast_config());
  h.set_clock([&t] { return t; });
  h.register_venue(0);
  REQUIRE(h.state(0) == HealthState::Healthy);

  // Enough rejects to trip -> Degraded.
  for (int i = 0; i < 5; ++i) h.on_reject(0, /*is_false_cancel_reject=*/false);
  CHECK(h.state(0) == HealthState::Degraded);
  CHECK(h.is_tradeable(0));           // Degraded is still tradeable
  CHECK(h.trips(0) == 1);

  // Hysteresis: one success does not clear it (rate still above the low band).
  h.on_ack(0, 100);
  CHECK(h.state(0) == HealthState::Degraded);

  // Persisting past the dwell -> Down (not tradeable).
  t += 50;
  h.tick(t);
  CHECK(h.state(0) == HealthState::Down);
  CHECK_FALSE(h.is_tradeable(0));
  CHECK(h.score(0).weight == 0.0);

  // Recover the underlying signal: many good acks age the rejects out.
  for (int i = 0; i < 20; ++i) h.on_ack(0, 100);
  // Dwell in good state -> Probing.
  t += 50;
  h.tick(t);
  CHECK(h.state(0) == HealthState::Probing);
  CHECK(h.is_tradeable(0));
  CHECK(h.score(0).weight < 1.0);     // reduced weight while probing

  // Complete the probe round trips -> fully Healthy.
  h.on_ack(0, 100);
  h.on_ack(0, 100);
  CHECK(h.state(0) == HealthState::Healthy);
  CHECK(h.unresolved(0) == 0);
}

TEST_CASE("false cancel-rejects are excluded from the rejection rate",
          "[health][false-cancel-reject]") {
  Timestamp t = 1'000;
  CircuitBreakerHealthModel h(fast_config());
  h.set_clock([&t] { return t; });
  h.register_venue(0);

  // Many false cancel-rejects must NOT trip the venue.
  for (int i = 0; i < 20; ++i) h.on_reject(0, /*is_false_cancel_reject=*/true);
  CHECK(h.state(0) == HealthState::Healthy);
  CHECK(h.trips(0) == 0);
}

TEST_CASE("ack latency p99 trips the venue", "[health][latency]") {
  Timestamp t = 1'000;
  CircuitBreakerHealthModel h(fast_config());
  h.set_clock([&t] { return t; });
  h.register_venue(0);

  for (int i = 0; i < 20; ++i) h.on_ack(0, /*latency=*/50'000);  // way over high
  CHECK(h.state(0) == HealthState::Degraded);
  CHECK(h.score(0).reason == TripReason::AckLatency);
}

TEST_CASE("disconnect trips connectivity immediately", "[health][connectivity]") {
  Timestamp t = 1'000;
  CircuitBreakerHealthModel h(fast_config());
  h.set_clock([&t] { return t; });
  h.register_venue(0);

  h.on_disconnect(0);
  CHECK(h.state(0) == HealthState::Degraded);
  CHECK(h.score(0).reason == TripReason::Connectivity);
}

TEST_CASE("time-based staleness is detected by the timer tick", "[health][staleness]") {
  Timestamp t = 1'000;
  HealthConfig c = fast_config();
  c.heartbeat_tolerance_ns = 100;   // small so staleness trips
  CircuitBreakerHealthModel h(c);
  h.set_clock([&t] { return t; });
  h.register_venue(0);

  t += 1'000;          // nothing arrived for longer than the tolerance
  h.tick(t);
  CHECK(h.state(0) == HealthState::Degraded);
  CHECK(h.score(0).reason == TripReason::Connectivity);
}

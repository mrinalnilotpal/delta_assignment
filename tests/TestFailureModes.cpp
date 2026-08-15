#include <catch2/catch_test_macros.hpp>

#include "oms/OrderId.h"
#include "TestUtil.h"

using namespace oms;
using namespace oms::test;

// One test per row of the 2.11 failure-mode table. Every test runs against the
// deterministic seeded SimulatedExchange.

TEST_CASE("mode 1: confirmation never arrives -> ack timeout, possibly live", "[fail][mode1]") {
  SimConfig cfg;
  cfg.ack_latency_ns = 1'000'000'000'000;    // confirm effectively never arrives
  cfg.fill_latency_ns = 1'000'000'000'000;
  SimHarness h(cfg);

  const OrderIdRaw id = h.om->submit({42, Side::Buy, 100, 10'000, OrderType::Limit});
  REQUIRE(id != 0);
  h.poll_n(5);                                // advance logical time; no confirm
  REQUIRE(h.om->find(id)->status == OrderStatus::Sent);

  h.om->check_ack_timeouts(h.sim->now(), /*timeout=*/1'000);

  CHECK(h.om->ack_timeout_count() == 1);
  CHECK(h.om->find(id)->ack_timed_out);
  CHECK(h.om->find(id) != nullptr);           // treated as possibly live, NOT retired
  CHECK(h.log.contains("ACK TIMEOUT"));
}

TEST_CASE("mode 2: fill and cancel-ack out of order -> fill wins, benign reject", "[fail][mode2]") {
  SimConfig cfg;
  cfg.fill_latency_ns = 1'000'000'000'000;
  SimHarness h(cfg);

  const OrderIdRaw id = h.om->submit({42, Side::Buy, 100, 10'000, OrderType::Limit});
  h.poll_n(2);
  REQUIRE(h.om->cancel(id));

  h.sim->inject_fill(id, 100, 10'000, /*trade=*/7, /*final=*/true);   // fill completes first
  h.poll_n(1);
  CHECK(h.om->position(42).net == 100);
  CHECK(h.om->false_cancel_reject_count() == 1);

  h.sim->inject_cancel_reject(id);            // late cancel-reject: benign
  h.poll_n(1);
  CHECK_FALSE(h.om->killed());
}

TEST_CASE("mode 3: unsolicited cancel is applied and counted", "[fail][mode3]") {
  SimConfig cfg;
  cfg.fill_latency_ns = 1'000'000'000'000;
  SimHarness h(cfg);

  const OrderIdRaw id = h.om->submit({42, Side::Buy, 100, 10'000, OrderType::Limit});
  h.poll_n(2);
  h.sim->inject_fill(id, 40, 10'000, /*trade=*/1, /*final=*/false);   // partial
  h.poll_n(1);
  h.sim->inject_unsolicited_cancel(id);
  h.poll_n(1);

  CHECK(h.om->find(id)->status == OrderStatus::Cancelled);
  CHECK(h.om->position(42).net == 40);              // partial applied
  CHECK(h.om->pending_quantity(42) == 0);           // residual released; algo requeues target
  CHECK(h.om->unsolicited_cancel_count() == 1);
}

TEST_CASE("mode 4: fill for unknown id reconciles, recurrence hard-stops", "[fail][mode4]") {
  SimHarness h;
  h.om->set_unknown_fill_hardstop_threshold(2);
  const OrderIdRaw bogus = encode_order_id(0, 9, 100, 3);

  h.om->on_fill(bogus, 10, 100, 1, 0);
  CHECK_FALSE(h.om->killed());
  CHECK(h.om->reconcile_requested());
  CHECK(h.om->unknown_fill_count() == 1);

  h.om->on_fill(bogus, 10, 100, 2, 0);
  h.om->on_fill(bogus, 10, 100, 3, 0);
  CHECK(h.om->killed());
}

TEST_CASE("mode 5: duplicate fill (same trade id) is dropped and counted", "[fail][mode5]") {
  SimConfig cfg;
  cfg.fill_latency_ns = 1'000'000'000'000;
  SimHarness h(cfg);

  const OrderIdRaw id = h.om->submit({42, Side::Buy, 100, 10'000, OrderType::Limit});
  h.poll_n(2);
  h.sim->inject_fill(id, 50, 10'000, /*trade=*/1, /*final=*/false);
  h.poll_n(1);
  h.sim->inject_fill(id, 50, 10'000, /*trade=*/1, /*final=*/false);   // duplicate id
  h.poll_n(1);

  CHECK(h.om->position(42).net == 50);              // not double counted
  CHECK(h.log.contains("duplicate_trade"));
  CHECK(h.om->out_of_sequence_dropped() >= 1);
}

TEST_CASE("mode 6: stale fill after recycle caught by generation bits", "[fail][mode6]") {
  SimConfig cfg;
  cfg.fill_latency_ns = 1'000'000'000'000;
  SimHarness h(cfg);

  const OrderIdRaw id = h.om->submit({42, Side::Buy, 100, 10'000, OrderType::Limit});
  const OrderId d = decode_order_id(id);
  // Craft an id for the SAME slot but a stale (earlier) generation.
  const OrderIdRaw stale = encode_order_id(d.venue, static_cast<uint16_t>(d.generation - 1),
                                           d.slot, d.sequence);

  h.om->on_fill(stale, 10, 10'000, /*trade=*/99, /*ts=*/0);

  CHECK(h.om->position(42).net == 0);               // not applied
  CHECK(h.log.contains("unknown_order"));
  CHECK(h.om->reconcile_requested());               // treated like mode 4
}

TEST_CASE("mode 7: disconnect keeps in-flight; reconnect requests reconcile", "[fail][mode7]") {
  SimConfig cfg;
  cfg.fill_latency_ns = 1'000'000'000'000;
  SimHarness h(cfg);

  const OrderIdRaw id = h.om->submit({42, Side::Buy, 100, 10'000, OrderType::Limit});
  h.poll_n(2);
  REQUIRE(h.om->find(id) != nullptr);

  h.sim->set_connected(false);
  h.om->on_venue_disconnect(0);
  CHECK(h.om->find(id) != nullptr);                 // in-flight NOT assumed dead
  CHECK(h.om->live_order_count() == 1);

  h.sim->set_connected(true);
  h.om->on_venue_reconnect(0);
  CHECK(h.om->reconcile_requested());
}

TEST_CASE("mode 8: reject storm demotes the venue and engages all-down", "[fail][mode8]") {
  HealthConfig hc;
  hc.reject_min_samples = 5;
  hc.reject_high = 0.5;
  hc.degrade_dwell_ns = 0;                           // trip to Down without waiting
  hc.down_dwell_ns = 1'000'000'000'000;             // and stay down for the test
  SimConfig cfg;
  cfg.reject_prob = 1.0;                             // every order rejected
  SimHarness h(cfg, hc);

  for (int i = 0; i < 12; ++i) {
    h.om->submit({42, Side::Buy, 10, 10'000, OrderType::Limit});
    h.poll_n(2);
  }
  h.health.tick(h.sim->now() + 1);
  CHECK_FALSE(h.health.is_tradeable(0));            // demoted by rejection rate

  const OrderIdRaw id = h.om->submit({42, Side::Buy, 10, 10'000, OrderType::Limit});
  CHECK(id == 0);                                    // all-down: not submitted
  CHECK(h.om->rebalance_incomplete());
}

TEST_CASE("mode 10: pool exhaustion is a hard stop with a diagnostic", "[fail][mode10]") {
  CapturingLogSink log;
  SimConfig cfg;
  cfg.fill_latency_ns = 1'000'000'000'000;           // keep orders live -> fill the pool
  auto sim_owned = std::make_unique<SimulatedExchange>(static_cast<VenueId>(0), cfg);
  SimulatedExchange* sim = sim_owned.get();
  ExchangeRegistry registry;
  registry.add(std::move(sim_owned));
  StubHealthModel health;
  health.tradeable[0] = true;
  HealthAwareRouter router(health, registry.venues());
  OrderManager om(registry, router, health, &log, /*pool_capacity=*/2);
  om.set_clock([sim] { return sim->now(); });
  sim->set_event_sink(&om);

  CHECK(om.submit({1, Side::Buy, 10, 100, OrderType::Limit}) != 0);
  CHECK(om.submit({1, Side::Buy, 10, 100, OrderType::Limit}) != 0);
  CHECK(om.submit({1, Side::Buy, 10, 100, OrderType::Limit}) == 0);   // exhausted
  CHECK(om.killed());
  CHECK(log.contains("ORDER_POOL_EXHAUSTED"));
}

#include <catch2/catch_test_macros.hpp>

#include "oms/OrderId.h"
#include "TestUtil.h"

using namespace oms;
using namespace oms::test;

TEST_CASE("normal lifecycle: confirm then full fill updates position", "[om][lifecycle]") {
  SimHarness h;
  const OrderIdRaw id = h.om->submit({42, Side::Buy, 100, 10'000, OrderType::Limit});
  REQUIRE(id != 0);
  CHECK(h.om->pending_quantity(42) == 100);

  h.poll_n(6);

  CHECK(h.om->position(42).net == 100);
  CHECK(h.om->pending_quantity(42) == 0);
  CHECK(h.om->live_order_count() == 0);
  const Order* o = h.om->find(id);        // still queryable during grace window
  REQUIRE(o != nullptr);
  CHECK(o->status == OrderStatus::Filled);
  CHECK(o->filled_size == 100);
  CHECK(o->avg_fill_price() == 10'000);
  CHECK_FALSE(h.om->killed());
}

TEST_CASE("sell order moves position negative", "[om][lifecycle]") {
  SimHarness h;
  const OrderIdRaw id = h.om->submit({7, Side::Sell, 50, 500, OrderType::Limit});
  REQUIRE(id != 0);
  CHECK(h.om->pending_quantity(7) == -50);
  h.poll_n(6);
  CHECK(h.om->position(7).net == -50);
  CHECK(h.om->pending_quantity(7) == 0);
}

TEST_CASE("partial fills: position reflects each fill BEFORE subscribers run",
          "[om][ordering-invariant]") {
  SimConfig cfg;
  cfg.partial_chunks = 4;
  SimHarness h(cfg);

  struct Checker : OrderEventListener {
    OrderManager* om;
    InstrumentId  inst;
    Quantity      seen{0};
    void on_order_event(const OrderEvent& e) override {
      if (e.kind == OrderEventKind::PartialFill || e.kind == OrderEventKind::Fill) {
        seen += e.last_qty;
        // The position must ALREADY include this fill when the subscriber runs.
        CHECK(om->position(inst).net == seen);
      }
    }
  } checker;
  checker.om = h.om.get();
  checker.inst = 3;
  h.om->subscribe(&checker);

  const OrderIdRaw id = h.om->submit({3, Side::Buy, 100, 10'000, OrderType::Limit});
  REQUIRE(id != 0);
  h.poll_n(10);

  CHECK(checker.seen == 100);
  CHECK(h.om->position(3).net == 100);
}

TEST_CASE("a single unknown fill reconciles; recurrence kills (mode 4)", "[om][out-of-sequence]") {
  SimHarness h;
  h.om->set_unknown_fill_hardstop_threshold(3);
  const OrderIdRaw bogus = encode_order_id(/*venue=*/0, /*gen=*/5, /*slot=*/10, /*seq=*/7);
  REQUIRE(h.om->find(bogus) == nullptr);

  // First sighting: not applied, not killed, but a reconcile is requested.
  h.om->on_fill(bogus, 10, 100, /*trade=*/1, /*ts=*/0);
  CHECK_FALSE(h.om->killed());
  CHECK(h.om->reconcile_requested());
  CHECK(h.log.contains("unknown_order"));

  // Recur beyond the threshold -> sticky hard stop.
  for (int i = 0; i < 3; ++i) h.om->on_fill(bogus, 10, 100, static_cast<TradeId>(2 + i), 0);
  CHECK(h.om->killed());
  CHECK(h.log.contains("ORDER_FILLED_NOT_IN_SYSTEM"));
  CHECK(h.om->submit({1, Side::Buy, 1, 1, OrderType::Limit}) == 0);   // sticky
}

TEST_CASE("fill for a terminal order is dropped, not killed", "[om][out-of-sequence]") {
  SimHarness h;
  const OrderIdRaw id = h.om->submit({42, Side::Buy, 100, 10'000, OrderType::Limit});
  h.poll_n(6);
  REQUIRE(h.om->find(id) != nullptr);
  REQUIRE(h.om->find(id)->status == OrderStatus::Filled);

  h.sim->inject_fill(id, 10, 10'000, /*trade=*/999, /*final=*/false);
  h.poll_n(1);

  CHECK_FALSE(h.om->killed());                 // terminal, not unknown
  CHECK(h.log.contains("terminal_state"));
  CHECK(h.om->position(42).net == 100);   // unchanged
}

TEST_CASE("duplicate trade id is de-duplicated", "[om][out-of-sequence]") {
  SimConfig cfg;
  cfg.fill_latency_ns = 1'000'000'000'000;    // suppress auto fills; use injectors
  SimHarness h(cfg);

  const OrderIdRaw id = h.om->submit({42, Side::Buy, 100, 10'000, OrderType::Limit});
  h.poll_n(2);                                 // confirm arrives
  REQUIRE(h.om->find(id)->status == OrderStatus::Confirmed);

  h.sim->inject_fill(id, 50, 10'000, /*trade=*/1, /*final=*/false);
  h.poll_n(1);
  CHECK(h.om->position(42).net == 50);

  h.sim->inject_fill(id, 50, 10'000, /*trade=*/1, /*final=*/false);   // duplicate
  h.poll_n(1);
  CHECK(h.om->position(42).net == 50);     // not double counted
  CHECK(h.log.contains("duplicate_trade"));

  h.sim->inject_fill(id, 50, 10'000, /*trade=*/2, /*final=*/true);
  h.poll_n(1);
  CHECK(h.om->position(42).net == 100);
}

TEST_CASE("cancel/fill race: fill wins, cancel-reject is benign", "[om][race]") {
  SimConfig cfg;
  cfg.fill_latency_ns = 1'000'000'000'000;
  SimHarness h(cfg);

  const OrderIdRaw id = h.om->submit({42, Side::Buy, 100, 10'000, OrderType::Limit});
  h.poll_n(2);
  REQUIRE(h.om->find(id)->status == OrderStatus::Confirmed);

  REQUIRE(h.om->cancel(id));                    // cancel in flight
  CHECK(h.om->find(id)->pending_cancel);

  // A fill completes the order before the cancel resolves.
  h.sim->inject_fill(id, 100, 10'000, /*trade=*/7, /*final=*/true);
  h.poll_n(1);
  CHECK(h.om->position(42).net == 100);
  CHECK(h.om->false_cancel_reject_count() == 1);

  // The cancel-reject that follows is an artifact of the fill: benign.
  h.sim->inject_cancel_reject(id);
  h.poll_n(1);
  CHECK(h.om->false_cancel_reject_count() == 1);
  CHECK_FALSE(h.om->killed());
}

TEST_CASE("unsolicited cancel terminates the order and clears pending", "[om][unsolicited]") {
  SimConfig cfg;
  cfg.fill_latency_ns = 1'000'000'000'000;
  SimHarness h(cfg);

  const OrderIdRaw id = h.om->submit({42, Side::Buy, 100, 10'000, OrderType::Limit});
  h.poll_n(2);
  REQUIRE(h.om->find(id)->status == OrderStatus::Confirmed);

  h.sim->inject_unsolicited_cancel(id);
  h.poll_n(1);

  CHECK(h.om->find(id)->status == OrderStatus::Cancelled);
  CHECK(h.om->pending_quantity(42) == 0);
  CHECK(h.om->live_order_count() == 0);
  CHECK(h.log.contains("unsolicited cancel"));
}

TEST_CASE("all-down policy: stop submitting, cancel working, mark incomplete", "[om][all-down]") {
  // Wire manually with a stub health model so we can force all-down.
  CapturingLogSink log;
  SimConfig cfg;
  cfg.fill_latency_ns = 1'000'000'000'000;   // keep the order working, not filled
  auto sim_owned = std::make_unique<SimulatedExchange>(static_cast<VenueId>(0), cfg);
  SimulatedExchange* sim = sim_owned.get();
  ExchangeRegistry registry;
  registry.add(std::move(sim_owned));

  StubHealthModel health;
  health.tradeable[0] = true;
  HealthAwareRouter router(health, registry.venues());
  OrderManager om(registry, router, health, &log);
  om.set_clock([sim] { return sim->now(); });
  sim->set_event_sink(&om);

  const OrderIdRaw id = om.submit({42, Side::Buy, 100, 10'000, OrderType::Limit});
  REQUIRE(id != 0);
  for (int i = 0; i < 2; ++i) sim->poll();      // confirm
  REQUIRE(om.live_order_count() == 1);

  health.tradeable[0] = false;                   // venue goes down
  const OrderIdRaw id2 = om.submit({42, Side::Buy, 50, 10'000, OrderType::Limit});
  CHECK(id2 == 0);                               // not submitted
  CHECK(om.rebalance_incomplete());
  CHECK(log.contains("ALL-DOWN"));

  for (int i = 0; i < 3; ++i) sim->poll();       // working order gets cancelled
  CHECK(om.find(id)->status == OrderStatus::Cancelled);
  CHECK(om.live_order_count() == 0);
}

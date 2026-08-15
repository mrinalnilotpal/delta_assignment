#include <catch2/catch_test_macros.hpp>

#include "oms/Reconciliation.h"
#include "TestUtil.h"

using namespace oms;
using namespace oms::test;

TEST_CASE("periodic reconcile: in sync -> no action", "[recon][periodic]") {
  SimHarness h;
  h.om->submit({42, Side::Buy, 100, 10'000, OrderType::Limit});
  h.poll_n(6);
  REQUIRE(h.om->position(42).net == 100);   // OMS and sim both at +100

  PeriodicReconciler rec(*h.om, h.registry, PeriodicReconConfig{0, 50, 2}, &h.log);
  auto records = rec.reconcile(h.sim->now());

  for (const auto& r : records) CHECK(r.action == DriftAction::InSync);
  CHECK(rec.corrective_orders() == 0);
  CHECK_FALSE(rec.halted());
}

TEST_CASE("periodic reconcile: in-flight excluded from drift", "[recon][periodic]") {
  SimConfig cfg;
  cfg.fill_latency_ns = 1'000'000'000'000;   // order stays working, unfilled
  SimHarness h(cfg);

  const OrderIdRaw id = h.om->submit({42, Side::Buy, 100, 10'000, OrderType::Limit});
  h.poll_n(2);
  REQUIRE(h.om->find(id)->status == OrderStatus::Confirmed);
  REQUIRE(h.om->pending_quantity(42) == 100);   // 100 in flight, 0 filled

  // Exchange filled-position is 0; OMS filled-position is 0 -> NO drift, even
  // though there is 100 in flight. (If in-flight were counted, this would look
  // like drift and emit a bogus corrective order.)
  PeriodicReconciler rec(*h.om, h.registry, PeriodicReconConfig{0, 500, 1}, &h.log);
  auto records = rec.reconcile(h.sim->now());
  for (const auto& r : records) CHECK(r.action == DriftAction::InSync);
  CHECK(rec.corrective_orders() == 0);
}

TEST_CASE("periodic reconcile: drift persists then auto-heals within band", "[recon][periodic]") {
  SimHarness h;
  h.sim->set_position(42, 100);   // exchange holds +100 the OMS never saw

  PeriodicReconciler rec(*h.om, h.registry, PeriodicReconConfig{/*thr=*/0, /*band=*/200, /*persist=*/2},
                         &h.log);

  auto c1 = rec.reconcile(h.sim->now());
  // First sighting: not acted on yet (must persist across cycles).
  bool pending = false;
  for (const auto& r : c1) if (r.inst == 42) pending = (r.action == DriftAction::PendingConfirm);
  CHECK(pending);
  CHECK(rec.corrective_orders() == 0);

  auto c2 = rec.reconcile(h.sim->now());
  bool healed = false;
  for (const auto& r : c2) if (r.inst == 42) healed = (r.action == DriftAction::AutoHealed);
  CHECK(healed);
  CHECK(rec.corrective_orders() == 1);   // a corrective order was emitted
  CHECK_FALSE(rec.halted());
}

TEST_CASE("periodic reconcile: drift beyond band halts (sticky) and re-arms", "[recon][periodic]") {
  SimHarness h;
  h.sim->set_position(42, 10'000);   // huge unexplained drift

  PeriodicReconciler rec(*h.om, h.registry, PeriodicReconConfig{/*thr=*/0, /*band=*/50, /*persist=*/1},
                         &h.log);

  auto c1 = rec.reconcile(h.sim->now());
  bool halted = false;
  for (const auto& r : c1) if (r.inst == 42) halted = (r.action == DriftAction::Halted);
  CHECK(halted);
  CHECK(rec.halted());
  CHECK(rec.halts() == 1);
  CHECK(h.log.contains("HALT"));

  // Sticky: another cycle does nothing until re-armed.
  auto c2 = rec.reconcile(h.sim->now());
  CHECK(c2.empty());

  rec.rearm();
  CHECK_FALSE(rec.halted());
}

TEST_CASE("periodic reconcile does not block order flow", "[recon][periodic][independence]") {
  SimHarness h;
  const OrderIdRaw id = h.om->submit({42, Side::Buy, 100, 10'000, OrderType::Limit});
  h.poll_n(1);   // confirm in flight; order not yet filled

  // Reconcile runs on its own timer, interleaved with the execution path.
  PeriodicReconciler rec(*h.om, h.registry, PeriodicReconConfig{0, 50, 2}, &h.log);
  rec.reconcile(h.sim->now());

  // Execution proceeds unaffected: the order still fills.
  h.poll_n(6);
  CHECK(h.om->position(42).net == 100);
  CHECK(h.om->find(id)->status == OrderStatus::Filled);
}

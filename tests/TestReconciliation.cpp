#include <catch2/catch_test_macros.hpp>

#include "oms/Reconciliation.h"

using namespace oms;

namespace {
ReconAction action_for(const StartupReport& r, InstrumentId inst) {
  for (const auto& l : r.lines)
    if (l.instrument == inst) return l.action;
  return ReconAction::Proceed;
}
}  // namespace

TEST_CASE("matching positions proceed", "[recon]") {
  StartupReconciler rec(StartupReconConfig{5});
  auto r = rec.reconcile({{1, 100}}, /*has_persisted=*/true, /*reachable=*/true, {{1, 100}});
  CHECK(r.can_start);
  CHECK(action_for(r, 1) == ReconAction::Proceed);
  CHECK(r.adopted.at(1) == 100);
}

TEST_CASE("cold start adopts the exchange", "[recon]") {
  StartupReconciler rec(StartupReconConfig{5});
  auto r = rec.reconcile({}, /*has_persisted=*/false, /*reachable=*/true, {{1, 100}});
  CHECK(r.can_start);
  CHECK(action_for(r, 1) == ReconAction::AdoptColdStart);
  CHECK(r.adopted.at(1) == 100);
}

TEST_CASE("small drift auto-heals to the exchange", "[recon]") {
  StartupReconciler rec(StartupReconConfig{5});
  auto r = rec.reconcile({{1, 100}}, true, true, {{1, 103}});   // drift 3 <= 5
  CHECK(r.can_start);
  CHECK(action_for(r, 1) == ReconAction::AdoptAutoHeal);
  CHECK(r.adopted.at(1) == 103);
}

TEST_CASE("large drift hard stops", "[recon]") {
  StartupReconciler rec(StartupReconConfig{5});
  auto r = rec.reconcile({{1, 100}}, true, true, {{1, 200}});   // drift 100 > 5
  CHECK_FALSE(r.can_start);
  CHECK(action_for(r, 1) == ReconAction::HardStop);
  CHECK(r.adopted.empty());
}

TEST_CASE("unreachable exchange hard stops", "[recon]") {
  StartupReconciler rec(StartupReconConfig{5});
  auto r = rec.reconcile({{1, 100}}, true, /*reachable=*/false, {});
  CHECK_FALSE(r.can_start);
  REQUIRE(r.lines.size() == 1);
  CHECK(r.lines[0].action == ReconAction::HardStop);
}

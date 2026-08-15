#include <catch2/catch_test_macros.hpp>

#include "oms/Netting.h"

using namespace oms;

namespace {
constexpr InstrumentId kInst = 5;
}

TEST_CASE("netting collapses same-side deltas into one net order", "[netting]") {
  NettingEngine ne;
  ne.set_target(1, kInst, 60);   // delta +60
  ne.set_target(2, kInst, 40);   // delta +40

  auto orders = ne.compute_cycle({{kInst, 10}});
  REQUIRE(orders.size() == 1);
  CHECK(orders[0].instrument == kInst);
  CHECK(orders[0].delta == 100);
}

TEST_CASE("zero net delta sends nothing but books the transfer", "[netting]") {
  NettingEngine ne;
  ne.set_target(1, kInst, 100);    // delta +100
  ne.set_target(2, kInst, -100);   // delta -100 (opposing)

  auto orders = ne.compute_cycle({{kInst, 10}});
  CHECK(orders.empty());                                   // no crossing orders sent
  CHECK(ne.netting_saved_notional() == (100 + 100) * 10 / 2);
  // Attributed sub-positions move to their targets even though no trade occurred.
  CHECK(ne.attributed(1, kInst) == 100);
  CHECK(ne.attributed(2, kInst) == -100);
  CHECK(ne.global_position(kInst) == 0);
  CHECK(ne.check_invariant());
}

TEST_CASE("fills attribute pro-rata and preserve the invariant", "[netting]") {
  NettingEngine ne;
  ne.set_target(1, kInst, 60);
  ne.set_target(2, kInst, 40);
  auto orders = ne.compute_cycle({{kInst, 10}});
  REQUIRE(orders.size() == 1);

  ne.attribute_fill(kInst, 100, 10);   // full fill
  CHECK(ne.attributed(1, kInst) == 60);
  CHECK(ne.attributed(2, kInst) == 40);
  CHECK(ne.global_position(kInst) == 100);
  CHECK(ne.check_invariant());
}

TEST_CASE("rounding residual goes to the largest contributor deterministically", "[netting]") {
  NettingEngine ne;
  ne.set_target(1, kInst, 1);
  ne.set_target(2, kInst, 1);
  ne.set_target(3, kInst, 1);
  auto orders = ne.compute_cycle({{kInst, 10}});
  REQUIRE(orders.size() == 1);
  CHECK(orders[0].delta == 3);

  ne.attribute_fill(kInst, 1, 10);   // one unit, three equal contributors
  // Equal remainders + equal weights -> tie-break by smallest strategy id.
  CHECK(ne.attributed(1, kInst) == 1);
  CHECK(ne.attributed(2, kInst) == 0);
  CHECK(ne.attributed(3, kInst) == 0);
  CHECK(ne.global_position(kInst) == 1);
  CHECK(ne.check_invariant());
}

TEST_CASE("seeded sub-positions keep global == sum(sub)", "[netting]") {
  NettingEngine ne;
  ne.set_attributed(1, kInst, 30);
  ne.set_attributed(2, kInst, -10);
  CHECK(ne.global_position(kInst) == 20);
  CHECK(ne.check_invariant());
}

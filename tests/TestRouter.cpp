#include <catch2/catch_test_macros.hpp>

#include "oms/Router.h"
#include "TestUtil.h"

using namespace oms;
using namespace oms::test;

TEST_CASE("router picks the healthiest tradeable venue", "[router]") {
  StubHealthModel health;
  for (VenueId v : {0, 1, 2}) health.tradeable[v] = true;
  health.weight[0] = 0.50;   // degraded
  health.weight[1] = 1.00;   // healthy
  health.weight[2] = 0.25;   // probing

  HealthAwareRouter router(health, {0, 1, 2});
  auto v = router.select(42, Side::Buy, 100);
  REQUIRE(v.has_value());
  CHECK(*v == 1);
}

TEST_CASE("router skips untradeable venues", "[router]") {
  StubHealthModel health;
  for (VenueId v : {0, 1, 2}) health.tradeable[v] = true;
  health.weight[0] = 0.50;
  health.weight[1] = 1.00;
  health.weight[2] = 0.25;

  health.tradeable[1] = false;   // best venue goes down
  HealthAwareRouter router(health, {0, 1, 2});
  auto v = router.select(42, Side::Buy, 100);
  REQUIRE(v.has_value());
  CHECK(*v == 0);                // next-best tradeable
}

TEST_CASE("router returns nullopt when all venues are down (all-down)", "[router][all-down]") {
  StubHealthModel health;
  for (VenueId v : {0, 1, 2}) health.tradeable[v] = false;

  HealthAwareRouter router(health, {0, 1, 2});
  auto v = router.select(42, Side::Buy, 100);
  CHECK_FALSE(v.has_value());
}

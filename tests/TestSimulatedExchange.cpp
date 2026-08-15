#include <catch2/catch_test_macros.hpp>

#include "oms/Order.h"
#include "TestUtil.h"

using namespace oms;
using namespace oms::test;

namespace {
// Drive a sim with a fixed script and record the exact event stream.
std::vector<std::string> run_scripted(uint64_t seed) {
  SimConfig cfg;
  cfg.seed = seed;
  cfg.partial_chunks = 3;
  cfg.duplicate_fill_prob = 0.5;
  cfg.unsolicited_cancel_prob = 0.1;
  cfg.reject_prob = 0.1;

  SimulatedExchange sim(0, cfg);
  RecordingSink sink;
  sim.set_event_sink(&sink);

  for (uint32_t i = 1; i <= 5; ++i) {
    Order o;
    o.internal_id = i;
    o.instrument = 1;
    o.side = Side::Buy;
    o.size = 30;
    o.limit_price = 100;
    sim.place_order(o);
  }
  for (int i = 0; i < 20; ++i) sim.poll();
  return sink.events;
}
}  // namespace

TEST_CASE("simulated exchange is deterministic for a fixed seed", "[sim][determinism]") {
  const auto a = run_scripted(12345);
  const auto b = run_scripted(12345);
  CHECK(a == b);
}

TEST_CASE("different seeds produce different event streams", "[sim][determinism]") {
  const auto a = run_scripted(1);
  const auto b = run_scripted(2);
  CHECK(a != b);
}

TEST_CASE("reordered fill-before-ack still yields the correct position", "[sim][reorder]") {
  SimConfig cfg;
  cfg.reorder_fill_before_ack = true;
  cfg.partial_chunks = 1;
  SimHarness h(cfg);

  const OrderIdRaw id = h.om->submit({42, Side::Buy, 100, 10'000, OrderType::Limit});
  REQUIRE(id != 0);
  h.poll_n(6);

  CHECK(h.om->position(42).net == 100);
  CHECK_FALSE(h.om->killed());
}

TEST_CASE("injected duplicate fills never double-count the position", "[sim][duplicate]") {
  SimConfig cfg;
  cfg.duplicate_fill_prob = 1.0;   // every fill delivered twice
  cfg.partial_chunks = 2;
  SimHarness h(cfg);

  const OrderIdRaw id = h.om->submit({42, Side::Buy, 100, 10'000, OrderType::Limit});
  REQUIRE(id != 0);
  h.poll_n(10);

  CHECK(h.om->position(42).net == 100);
  CHECK_FALSE(h.om->killed());
}

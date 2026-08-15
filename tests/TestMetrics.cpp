#include <catch2/catch_test_macros.hpp>

#include "oms/MarketData.h"
#include "oms/Metrics.h"
#include "TestUtil.h"

using namespace oms;
using namespace oms::test;

namespace {
class FixedBook : public MarketDataSource {
 public:
  TopOfBook book{};
  TopOfBook top_of_book(InstrumentId) const override { return book; }
  Quantity  volume_since(InstrumentId, Timestamp) const override { return 0; }
};
}  // namespace

TEST_CASE("Distribution reports percentiles from a bounded ring", "[metrics]") {
  Distribution d(1000);
  for (int i = 1; i <= 100; ++i) d.add(i);
  CHECK(d.count() == 100);
  CHECK(d.p50() >= 49);
  CHECK(d.p50() <= 51);
  CHECK(d.max() == 100);
  CHECK(d.min() == 1);
}

TEST_CASE("metrics: slippage, ack latency and time-to-fill are captured", "[metrics]") {
  SimHarness h;   // ack_latency 1ms, fill in a couple polls
  FixedBook mds;
  mds.book = TopOfBook{9'999, 10'001, 10, 10, 0};   // mid 10000

  MetricsCollector m(*h.om, &mds);
  h.om->subscribe(&m);

  // Buy that fills at 10100 vs a 10000 arrival mid -> +100 bps slippage.
  h.om->submit({42, Side::Buy, 100, 10'100, OrderType::Limit});
  h.poll_n(8);

  CHECK(m.filled_orders() == 1);
  CHECK(m.ack_latency_ns().count() == 1);
  CHECK(m.ack_latency_ns().p50() == 1'000'000);
  CHECK(m.time_to_fill_ns().count() == 1);
  CHECK(m.time_to_fill_ns().p50() > 0);
  REQUIRE(m.slippage_arrival_bps().count() == 1);
  CHECK(m.slippage_arrival_bps().p50() == 100);
}

TEST_CASE("metrics: cycle fill rate and completion", "[metrics]") {
  SimHarness h;
  MetricsCollector m(*h.om);

  m.record_cycle(100, 100);   // complete
  m.record_cycle(100, 50);    // incomplete
  m.record_cycle(80, 80);     // complete

  CHECK(m.cycles() == 3);
  CHECK(m.completed_cycles() == 2);
  CHECK(m.rebalance_completion_rate() > 0.66);
  CHECK(m.rebalance_completion_rate() < 0.67);
  CHECK(m.fill_rate_bps().max() == 10'000);   // a 100% cycle
  CHECK(m.fill_rate_bps().min() == 5'000);    // the 50% cycle
}

TEST_CASE("metrics: reconciliation-tagged orders are excluded", "[metrics]") {
  SimHarness h;
  FixedBook mds;
  mds.book = TopOfBook{9'999, 10'001, 10, 10, 0};
  MetricsCollector m(*h.om, &mds);
  h.om->subscribe(&m);

  OrderRequest req{42, Side::Buy, 100, 10'100, OrderType::Limit};
  req.reconciliation = true;
  h.om->submit(req);
  h.poll_n(8);

  CHECK(m.filled_orders() == 0);                    // excluded from execution quality
  CHECK(m.slippage_arrival_bps().count() == 0);
}

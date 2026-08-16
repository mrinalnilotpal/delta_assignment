#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "oms/ExecutionAlgo.h"
#include "oms/MarketData.h"
#include "TestUtil.h"

using namespace oms;
using namespace oms::test;

namespace {

// Market data with settable book and a fixed observed volume per interval.
class MockMds : public MarketDataSource {
 public:
  TopOfBook book{};
  Quantity  per_interval_volume{0};
  TopOfBook top_of_book(InstrumentId) const override { return book; }
  Quantity  volume_since(InstrumentId, Timestamp) const override { return per_interval_volume; }
};

// Captures confirmed child order ids in submission order.
struct CaptureIds : OrderEventListener {
  std::vector<OrderIdRaw> confirmed;
  void on_order_event(const OrderEvent& e) override {
    if (e.kind == OrderEventKind::Confirmed) confirmed.push_back(e.internal_id);
  }
};

// A config that confirms orders but never auto-fills (fills scheduled far out).
SimConfig no_fill_cfg() {
  SimConfig c;
  c.ack_latency_ns = 1'000'000;
  c.fill_latency_ns = 1'000'000'000'000;   // effectively never within a test
  c.step_ns = 1'000'000;
  return c;
}

constexpr InstrumentId kInst = 42;

}  // namespace

TEST_CASE("TWAP splits target into N equal slices", "[algo][twap]") {
  SimHarness h(no_fill_cfg());
  MockMds mds;
  mds.book = TopOfBook{100, 101, 10, 10, 0};

  AlgoConfig ac;
  ac.om = h.om.get();
  ac.mds = &mds;
  ac.log = &h.log;
  ac.worker_index = 7;
  ac.worker_offset = 0;
  ac.algo_tag = 1;
  ac.slices = 10;

  auto twap = AlgoFactory::create("twap", ac);
  AlgoDispatcher disp(*h.om);
  disp.register_algo(twap.get(), 0, 7);
  h.om->subscribe(&disp);

  ParentOrder p;
  p.instrument = kInst;
  p.side = Side::Buy;
  p.qty = 100;
  p.start = 0;
  p.end = 100;
  p.limit = 101;
  twap->start(p);

  for (Timestamp t = 0; t < 100; t += 10) {
    twap->on_timer(t);
    h.poll_n(3);   // deliver confirms (no fills)
  }

  CHECK(twap->stats().child_orders == 10);
  CHECK(h.om->pending_quantity(kInst) == 100);   // all working, evenly split
  CHECK_FALSE(twap->is_done());
}

TEST_CASE("TWAP completes the parent when children fill", "[algo][twap]") {
  SimHarness h;   // default config auto-fills
  MockMds mds;
  mds.book = TopOfBook{100, 101, 10, 10, 0};

  AlgoConfig ac;
  ac.om = h.om.get();
  ac.mds = &mds;
  ac.worker_index = 9;
  ac.slices = 5;
  auto twap = AlgoFactory::create("twap", ac);
  AlgoDispatcher disp(*h.om);
  disp.register_algo(twap.get(), 0, 9);
  h.om->subscribe(&disp);

  ParentOrder p;
  p.instrument = kInst;
  p.side = Side::Buy;
  p.qty = 100;
  p.start = 0;
  p.end = 50;
  p.limit = 101;
  twap->start(p);

  for (Timestamp t = 0; t <= 50; t += 10) {
    twap->on_timer(t);
    h.poll_n(6);
  }

  CHECK(h.om->position(kInst).net == 100);
  CHECK(twap->stats().filled_qty == 100);
  CHECK(twap->is_done());
  CHECK_FALSE(twap->stats().incomplete_at_deadline);
}

TEST_CASE("TWAP absorbs a prior partial fill in the next slice", "[algo][twap]") {
  SimHarness h(no_fill_cfg());
  MockMds mds;
  mds.book = TopOfBook{100, 101, 10, 10, 0};
  CaptureIds cap;
  h.om->subscribe(&cap);

  AlgoConfig ac;
  ac.om = h.om.get();
  ac.mds = &mds;
  ac.worker_index = 3;
  ac.slices = 2;
  auto twap = AlgoFactory::create("twap", ac);
  AlgoDispatcher disp(*h.om);
  disp.register_algo(twap.get(), 0, 3);
  h.om->subscribe(&disp);

  ParentOrder p;
  p.instrument = kInst;
  p.side = Side::Buy;
  p.qty = 100;
  p.start = 0;
  p.end = 100;
  p.limit = 101;
  twap->start(p);

  twap->on_timer(0);        // slice 0: ceil(100/2) = 50
  h.poll_n(3);
  REQUIRE(cap.confirmed.size() == 1);
  CHECK(h.om->pending_quantity(kInst) == 50);

  // 30 of the first child fills (20 still working).
  h.sim->inject_fill(cap.confirmed[0], 30, 100, 1, false);
  h.poll_n(2);
  CHECK(h.om->position(kInst).net == 30);

  twap->on_timer(50);       // slice 1 (final): remaining = 100-30-20 = 50
  h.poll_n(3);

  CHECK(twap->stats().child_orders == 2);
  CHECK(h.om->position(kInst).net == 30);
  CHECK(h.om->pending_quantity(kInst) == 70);   // 20 leftover + 50 new slice
}

TEST_CASE("TWAP canonical: buy 100 over 10 slices, slice-3 partial, no over-send",
          "[algo][twap]") {
  SimHarness h(no_fill_cfg());
  MockMds mds;
  mds.book = TopOfBook{100, 101, 10, 10, 0};
  CaptureIds cap;
  h.om->subscribe(&cap);

  AlgoConfig ac;
  ac.om = h.om.get();
  ac.mds = &mds;
  ac.worker_index = 31;
  ac.slices = 10;
  auto twap = AlgoFactory::create("twap", ac);
  AlgoDispatcher disp(*h.om);
  disp.register_algo(twap.get(), 0, 31);
  h.om->subscribe(&disp);

  ParentOrder p;
  p.instrument = kInst;
  p.side = Side::Buy;
  p.qty = 100;
  p.start = 0;
  p.end = 100;
  p.limit = 101;
  twap->start(p);

  // Slices 0,1,2 (each 10), then partially fill the 3rd child before continuing.
  for (Timestamp t = 0; t <= 20; t += 10) { twap->on_timer(t); h.poll_n(3); }
  REQUIRE(cap.confirmed.size() == 3);
  h.sim->inject_fill(cap.confirmed[2], 6, 100, /*trade=*/1, /*final=*/false);
  h.poll_n(2);

  for (Timestamp t = 30; t < 100; t += 10) { twap->on_timer(t); h.poll_n(3); }

  CHECK(twap->stats().child_orders == 10);
  CHECK(h.om->position(kInst).net == 6);                 // the partial fill
  CHECK(h.om->pending_quantity(kInst) == 94);            // remainder working
  CHECK(h.om->position(kInst).net + h.om->pending_quantity(kInst) == 100);  // no over-send
}

TEST_CASE("algo requeues residual after an unsolicited cancel (mode 3)", "[algo][twap][fail]") {
  SimConfig cfg = no_fill_cfg();
  SimHarness h(cfg);
  MockMds mds;
  mds.book = TopOfBook{100, 101, 10, 10, 0};
  CaptureIds cap;
  h.om->subscribe(&cap);

  AlgoConfig ac;
  ac.om = h.om.get();
  ac.mds = &mds;
  ac.worker_index = 41;
  ac.slices = 4;
  auto twap = AlgoFactory::create("twap", ac);
  AlgoDispatcher disp(*h.om);
  disp.register_algo(twap.get(), 0, 41);
  h.om->subscribe(&disp);

  ParentOrder p;
  p.instrument = kInst;
  p.side = Side::Buy;
  p.qty = 100;
  p.start = 0;
  p.end = 100;
  p.limit = 101;
  twap->start(p);

  twap->on_timer(0);          // slice 0: ceil(100/4)=25
  h.poll_n(3);
  REQUIRE(cap.confirmed.size() == 1);

  // Exchange unsolicited-cancels the child; the algo must requeue the residual
  // (parent target unchanged), not shrink the parent.
  h.sim->inject_unsolicited_cancel(cap.confirmed[0]);
  h.poll_n(2);
  CHECK(h.om->pending_quantity(kInst) == 0);   // child gone

  twap->on_timer(25);          // next slice re-sizes to the full remaining target
  h.poll_n(3);
  CHECK(h.om->pending_quantity(kInst) > 0);     // residual requeued
}

TEST_CASE("TWAP marks the parent incomplete at the deadline", "[algo][twap]") {
  SimHarness h(no_fill_cfg());
  MockMds mds;
  mds.book = TopOfBook{100, 101, 10, 10, 0};

  AlgoConfig ac;
  ac.om = h.om.get();
  ac.mds = &mds;
  ac.log = &h.log;
  ac.worker_index = 5;
  ac.slices = 1;
  auto twap = AlgoFactory::create("twap", ac);
  AlgoDispatcher disp(*h.om);
  disp.register_algo(twap.get(), 0, 5);
  h.om->subscribe(&disp);

  ParentOrder p;
  p.instrument = kInst;
  p.side = Side::Buy;
  p.qty = 100;
  p.start = 0;
  p.end = 10;
  p.limit = 101;
  twap->start(p);

  twap->on_timer(0);    // sends the full 100 (single slice)
  h.poll_n(3);
  twap->on_timer(10);   // deadline: cancels resting, residual==inflight so no cross
  h.poll_n(3);          // cancel-ack clears in-flight
  twap->on_timer(11);   // finalize

  CHECK(twap->is_done());
  CHECK(twap->stats().incomplete_at_deadline);
  CHECK(h.om->position(kInst).net == 0);
}

TEST_CASE("POV sizes a child at rate * observed volume", "[algo][pov]") {
  SimHarness h(no_fill_cfg());
  MockMds mds;
  mds.book = TopOfBook{100, 101, 10, 10, 0};
  mds.per_interval_volume = 500;

  AlgoConfig ac;
  ac.om = h.om.get();
  ac.mds = &mds;
  ac.worker_index = 11;
  ac.participation_rate = 0.10;
  auto pov = AlgoFactory::create("pov", ac);
  AlgoDispatcher disp(*h.om);
  disp.register_algo(pov.get(), 0, 11);
  h.om->subscribe(&disp);

  ParentOrder p;
  p.instrument = kInst;
  p.side = Side::Buy;
  p.qty = 1000;
  p.start = 0;
  p.end = 1000;
  p.limit = 101;
  pov->start(p);

  pov->on_timer(10);   // 0.1 * 500 = 50
  h.poll_n(3);

  CHECK(pov->stats().child_orders == 1);
  CHECK(pov->stats().market_volume == 500);
  CHECK(h.om->pending_quantity(kInst) == 50);
}

TEST_CASE("POV idles below the volume floor", "[algo][pov]") {
  SimHarness h(no_fill_cfg());
  MockMds mds;
  mds.per_interval_volume = 0;   // dried up

  AlgoConfig ac;
  ac.om = h.om.get();
  ac.mds = &mds;
  ac.worker_index = 13;
  ac.participation_rate = 0.10;
  ac.min_volume_floor = 10;
  auto pov = AlgoFactory::create("pov", ac);
  AlgoDispatcher disp(*h.om);
  disp.register_algo(pov.get(), 0, 13);
  h.om->subscribe(&disp);

  ParentOrder p;
  p.instrument = kInst;
  p.side = Side::Buy;
  p.qty = 1000;
  p.start = 0;
  p.end = 1000;
  p.limit = 101;
  pov->start(p);

  pov->on_timer(10);
  h.poll_n(2);

  CHECK(pov->stats().idle_intervals == 1);
  CHECK(pov->stats().child_orders == 0);
  CHECK(h.om->pending_quantity(kInst) == 0);
}

TEST_CASE("POV degrades to a TWAP schedule when shortfall grows", "[algo][pov]") {
  SimHarness h(no_fill_cfg());
  MockMds mds;
  mds.book = TopOfBook{100, 101, 10, 10, 0};
  mds.per_interval_volume = 100;

  AlgoConfig ac;
  ac.om = h.om.get();
  ac.mds = &mds;
  ac.log = &h.log;
  ac.worker_index = 17;
  ac.participation_rate = 0.50;
  ac.catch_up_threshold = 10;      // shortfall 50 > 10 -> degrade
  ac.residual_twap_slices = 5;
  auto pov = AlgoFactory::create("pov", ac);
  AlgoDispatcher disp(*h.om);
  disp.register_algo(pov.get(), 0, 17);
  h.om->subscribe(&disp);

  ParentOrder p;
  p.instrument = kInst;
  p.side = Side::Buy;
  p.qty = 1000;
  p.start = 0;
  p.end = 1000;
  p.limit = 101;
  pov->start(p);

  pov->on_timer(10);
  h.poll_n(3);

  CHECK(pov->stats().degraded_to_twap);
  // The residual is now worked by the held TWAP instance, so a child is live.
  CHECK(h.om->pending_quantity(kInst) > 0);
}

TEST_CASE("POV cumulative participation tracks the target rate", "[algo][pov]") {
  SimHarness h;   // auto-fills
  MockMds mds;
  mds.book = TopOfBook{100, 101, 10, 10, 0};
  mds.per_interval_volume = 500;

  AlgoConfig ac;
  ac.om = h.om.get();
  ac.mds = &mds;
  ac.worker_index = 21;
  ac.participation_rate = 0.10;
  auto pov = AlgoFactory::create("pov", ac);
  AlgoDispatcher disp(*h.om);
  disp.register_algo(pov.get(), 0, 21);
  h.om->subscribe(&disp);

  ParentOrder p;
  p.instrument = kInst;
  p.side = Side::Buy;
  p.qty = 1000;
  p.start = 0;
  p.end = 10'000;
  p.limit = 101;
  pov->start(p);

  for (int i = 1; i <= 10; ++i) {
    pov->on_timer(static_cast<Timestamp>(i) * 10);
    h.poll_n(6);
  }

  const AlgoStats s = pov->stats();
  REQUIRE(s.market_volume == 5000);
  const double ratio = static_cast<double>(s.filled_qty) / static_cast<double>(s.market_volume);
  CHECK(std::abs(ratio - 0.10) < 0.02);
}

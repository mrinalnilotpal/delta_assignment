#include <catch2/catch_test_macros.hpp>

#include "oms/Netting.h"
#include "oms/Signal.h"
#include "oms/SignalProducer.h"

using namespace oms;

namespace {
SignalProducerConfig cfg() {
  SignalProducerConfig c;
  c.num_strategies       = 50;
  c.instruments          = {1, 2, 3, 4, 5, 6, 7, 8};
  c.targets_per_strategy = 4;
  return c;
}
}  // namespace

TEST_CASE("producer emits one message per strategy indexed 1..50") {
  SignalProducer p(cfg());
  const auto msgs = p.produce_cycle(1'000);

  REQUIRE(msgs.size() == 50);
  for (StrategyId s = 1; s <= 50; ++s) {
    const auto& m = msgs[s - 1];
    CHECK(m.strategy_id == s);              // dense 1..50 indexing
    CHECK(m.generated_at == 1'000);
    CHECK(m.sequence == 0);                 // first cycle
    CHECK(m.targets.size() == 4);
  }
}

TEST_CASE("producer sequence is monotonic per strategy across cycles") {
  SignalProducer p(cfg());
  p.produce_cycle(1'000);
  const auto second = p.produce_cycle(2'000);
  for (const auto& m : second) CHECK(m.sequence == 1);
}

TEST_CASE("producer output survives the binary codec round-trip") {
  SignalProducer p(cfg());
  for (const auto& m : p.produce_cycle(5'000)) {
    const auto bytes = serialize_signal(m);
    SignalMessage decoded;
    REQUIRE(deserialize_signal(bytes, decoded));
    CHECK(decoded.strategy_id == m.strategy_id);
    CHECK(decoded.sequence == m.sequence);
    REQUIRE(decoded.targets.size() == m.targets.size());
    for (std::size_t i = 0; i < m.targets.size(); ++i) {
      CHECK(decoded.targets[i].instrument == m.targets[i].instrument);
      CHECK(decoded.targets[i].signed_target == m.targets[i].signed_target);
    }
  }
}

TEST_CASE("producer is deterministic for a fixed seed") {
  SignalProducer a(cfg());
  SignalProducer b(cfg());
  const auto ma = a.produce_cycle(1'000);
  const auto mb = b.produce_cycle(1'000);
  REQUIRE(ma.size() == mb.size());
  for (std::size_t i = 0; i < ma.size(); ++i) {
    REQUIRE(ma[i].targets.size() == mb[i].targets.size());
    for (std::size_t j = 0; j < ma[i].targets.size(); ++j) {
      CHECK(ma[i].targets[j].instrument == mb[i].targets[j].instrument);
      CHECK(ma[i].targets[j].signed_target == mb[i].targets[j].signed_target);
    }
  }
}

TEST_CASE("50-strategy pipeline: gate + netting collapse targets and hold invariant") {
  std::unordered_map<InstrumentId, Price> prices;
  for (InstrumentId i = 1; i <= 8; ++i) prices[i] = 100 + i;

  SignalProducer p(cfg());
  SignalGate gate;
  NettingEngine netting;

  // Boots in do-not-trade: nothing is accepted until reconciliation clears.
  CHECK(gate.check(p.produce_cycle(10).front(), 10) == SignalVerdict::RejectDNT);
  gate.enable_trading(true);

  std::size_t target_lines = 0;
  for (const auto& m : p.produce_cycle(1'000)) {
    REQUIRE(gate.check(m, 1'000) == SignalVerdict::Accept);
    for (const auto& t : m.targets) {
      netting.set_target(m.strategy_id, t.instrument, t.signed_target);
      ++target_lines;
    }
  }

  const auto orders = netting.compute_cycle(prices);
  for (const auto& o : orders) netting.attribute_fill(o.instrument, o.delta, prices[o.instrument]);

  // 50 strategies over 8 instruments must collapse to at most 8 net orders.
  CHECK(orders.size() <= 8);
  CHECK(orders.size() < target_lines);
  CHECK(netting.check_invariant());
}

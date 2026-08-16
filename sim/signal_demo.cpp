// Demonstrates the signal pipeline end-to-end with strategies indexed 1..50:
//
//   SignalProducer -> binary codec -> InProcessSignalQueue -> SignalGate
//                  -> NettingEngine -> net orders (+ simulated fills / attribution)
//
// It is not a predictor: targets are seeded and deterministic. The point is to
// show 50 strategies with overlapping instruments collapsing into net orders, the
// startup do-not-trade gate, and the global == Sum(sub-positions) invariant holding.

#include <iostream>

#include "oms/Netting.h"
#include "oms/Signal.h"
#include "oms/SignalProducer.h"

using namespace oms;

int main() {
  // ---- Universe ------------------------------------------------------------
  std::vector<InstrumentId> universe;
  std::unordered_map<InstrumentId, Price> prices;
  for (InstrumentId i = 1; i <= 12; ++i) {
    universe.push_back(i);
    prices[i] = 100 + i;   // integer tick prices
  }

  SignalProducerConfig pcfg;
  pcfg.num_strategies      = 50;   // strategies indexed 1..50
  pcfg.instruments         = universe;
  pcfg.targets_per_strategy = 6;
  SignalProducer producer(pcfg);

  InProcessSignalQueue queue;
  SignalGate gate;                 // boots in do-not-trade
  NettingEngine netting;

  // ---- Startup gate: signals are rejected until reconciliation clears ------
  Timestamp now = 1'000'000'000;
  {
    auto boot = producer.produce_cycle(now);
    const auto v = gate.check(boot.front(), now);
    std::cout << "startup (DNT) verdict for strategy 1: "
              << (v == SignalVerdict::RejectDNT ? "RejectDNT (correct)" : "UNEXPECTED")
              << "\n";
  }
  gate.enable_trading(true);       // startup reconciliation cleared

  // ---- Run a few cycles ----------------------------------------------------
  const int kCycles = 4;
  uint64_t accepted = 0, rejected = 0, target_lines = 0, net_orders = 0;

  for (int c = 0; c < kCycles; ++c) {
    now += 1'000'000'000;   // 1s per cycle

    // Producer -> codec -> queue.
    for (const auto& msg : producer.produce_cycle(now)) {
      const auto bytes = serialize_signal(msg);
      SignalMessage decoded;
      if (!deserialize_signal(bytes, decoded)) {
        std::cerr << "codec round-trip failed\n";
        return 1;
      }
      queue.push(decoded);
    }

    // Queue -> gate -> netting.
    SignalMessage m;
    while (queue.try_pop(m)) {
      const auto v = gate.check(m, now);
      if (v == SignalVerdict::Accept || v == SignalVerdict::GapAcceptedLogged) {
        ++accepted;
        for (const auto& t : m.targets) {
          netting.set_target(m.strategy_id, t.instrument, t.signed_target);
          ++target_lines;
        }
      } else {
        ++rejected;
      }
    }

    // Netting -> net orders, then simulate full fills so attribution runs.
    const auto orders = netting.compute_cycle(prices);
    net_orders += orders.size();
    for (const auto& o : orders) netting.attribute_fill(o.instrument, o.delta, prices[o.instrument]);
  }

  // ---- Report --------------------------------------------------------------
  std::cout << "\n--- signal pipeline summary ---\n"
            << "strategies              : 1.." << producer.strategy_count() << "\n"
            << "instruments             : " << universe.size() << "\n"
            << "cycles                  : " << kCycles << "\n"
            << "signals accepted        : " << accepted << "\n"
            << "signals rejected        : " << rejected << "\n"
            << "raw target lines        : " << target_lines << "\n"
            << "net orders emitted      : " << net_orders
            << "  (collapsed from " << target_lines << " strategy targets)\n"
            << "netting saved notional  : " << netting.netting_saved_notional() << "\n"
            << "invariant global==Sum(sub): "
            << (netting.check_invariant() ? "OK" : "VIOLATED") << "\n";

  return netting.check_invariant() ? 0 : 1;
}

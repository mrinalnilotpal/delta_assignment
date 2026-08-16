#pragma once

#include "oms/Signal.h"
#include "oms/Types.h"

namespace oms {

// Test/demo stand-in for the research pipeline. It does NOT predict anything: it
// emits deterministic, seeded target books for strategies indexed 1..num_strategies
// over a shared instrument universe, so the transport -> gate -> netting path can be
// exercised end-to-end with reproducible numbers. Real signals arrive over
// SignalTransport; this just fabricates plausible SignalMessages.
struct SignalProducerConfig {
  StrategyId                num_strategies{50};    // strategies indexed 1..N
  std::vector<InstrumentId> instruments;           // shared universe
  uint32_t                  targets_per_strategy{8};
  Quantity                  max_abs_target{500};
  uint64_t                  seed{0x5eed'1234'5678'9abcull};
};

class SignalProducer {
 public:
  explicit SignalProducer(SignalProducerConfig cfg) : cfg_(std::move(cfg)), rng_(cfg_.seed) {}

  // One message per strategy for this cycle. `sequence` is monotonic per strategy
  // across cycles so the gate's gap check has something real to track.
  std::vector<SignalMessage> produce_cycle(Timestamp now);

  StrategyId strategy_count() const { return cfg_.num_strategies; }

 private:
  SignalProducerConfig                     cfg_;
  uint64_t                                 rng_;
  std::unordered_map<StrategyId, uint64_t> seq_;
};

}  // namespace oms

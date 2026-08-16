#include "oms/SignalProducer.h"

#include "oms/Detail.h"

namespace oms {

std::vector<SignalMessage> SignalProducer::produce_cycle(Timestamp now) {
  std::vector<SignalMessage> out;
  if (cfg_.instruments.empty() || cfg_.num_strategies == 0) return out;
  out.reserve(cfg_.num_strategies);

  const uint32_t per = cfg_.targets_per_strategy > cfg_.instruments.size()
                           ? static_cast<uint32_t>(cfg_.instruments.size())
                           : cfg_.targets_per_strategy;

  for (StrategyId s = 1; s <= cfg_.num_strategies; ++s) {
    SignalMessage m;
    m.strategy_id  = s;
    m.generated_at = now;
    m.sequence     = seq_[s]++;   // monotonic per strategy

    // Each strategy targets a rotating, overlapping window so strategies collide
    // on shared instruments -- that is what gives netting something to collapse.
    const std::size_t n = cfg_.instruments.size();
    const std::size_t start = detail::next_rand(rng_) % n;
    for (uint32_t i = 0; i < per; ++i) {
      const InstrumentId inst = cfg_.instruments[(start + i) % n];
      const int64_t span = 2 * cfg_.max_abs_target + 1;
      const Quantity target =
          static_cast<Quantity>(detail::next_rand(rng_) % static_cast<uint64_t>(span)) - cfg_.max_abs_target;
      m.targets.push_back({inst, target});
    }
    out.push_back(std::move(m));
  }
  return out;
}

}  // namespace oms
